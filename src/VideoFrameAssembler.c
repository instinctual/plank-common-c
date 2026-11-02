#include "Limelight-internal.h"
#include <stdatomic.h>

// Parameter sets plus picture data: AVC has three views, HEVC four. AUD/SEI
// prefixes are skipped. The picture view includes all subsequent slices.
struct _PLANK_VIDEO_BUFFER {
    atomic_uint references;
    atomic_bool submitted;
    int capacity, length, entryCount;
    QUEUED_DECODE_UNIT queued;
    LENTRY entries[4];
    unsigned char data[];
};
static PLANK_VIDEO_BUFFER* assemblingBuffer;

PLANK_VIDEO_BUFFER* LiAllocateVideoBuffer(int capacity) {
    if (capacity <= 0 || capacity > PLANK_VIDEO_BUFFER_LIMIT) return NULL;
    PLANK_VIDEO_BUFFER* buffer = malloc(sizeof(*buffer) + (size_t)capacity + PLANK_VIDEO_BUFFER_PADDING);
    if (!buffer) return NULL;
    atomic_init(&buffer->references, 1);
    atomic_init(&buffer->submitted, false);
    buffer->capacity = capacity;
    buffer->length = buffer->entryCount = 0;
    return buffer;
}
unsigned char* LiVideoBufferData(PLANK_VIDEO_BUFFER* buffer) { return buffer ? buffer->data : NULL; }
void LiRetainVideoBuffer(PLANK_VIDEO_BUFFER* buffer) {
    if (buffer) atomic_fetch_add_explicit(&buffer->references, 1, memory_order_relaxed);
}
void LiReleaseVideoBuffer(PLANK_VIDEO_BUFFER* buffer) {
    if (buffer && atomic_fetch_sub_explicit(&buffer->references, 1, memory_order_acq_rel) == 1) free(buffer);
}
const unsigned char* LiVideoFrameData(const DECODE_UNIT* unit) {
    if (!unit || !unit->frameBuffer || !unit->bufferList) return NULL;
    const char* start = unit->bufferList->data;
    const char* next = start;
    for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
        if (entry->data != next) return NULL;
        next += entry->length;
    }
    return next == (const char*)unit->frameBuffer->data + unit->frameBuffer->length &&
        next - start == unit->fullLength ? (const unsigned char*)start : NULL;
}

static PLENTRY nalChainHead;
static PLENTRY nalChainTail;
static int nalChainDataLength;

static unsigned int nextFrameNumber;
static atomic_bool waitingForIdrFrame;
static int frameType;
static uint16_t frameHostProcessingLatency;
static uint64_t frameReceiveTimeUs;
static uint64_t framePresentationTimeUs;
static uint32_t frameTimestamp90Khz;
static atomic_bool dropStatePending;
static atomic_bool idrFrameProcessed;

#define DR_CLEANUP -1000

#define CONSECUTIVE_DROP_LIMIT 120
static unsigned int consecutiveFrameDrops;

static LINKED_BLOCKING_QUEUE decodeUnitQueue;

typedef struct _BUFFER_DESC {
    char* data;
    unsigned int offset;
    unsigned int length;
} BUFFER_DESC, *PBUFFER_DESC;

#define H264_NAL_TYPE(x) ((x) & 0x1F)
#define HEVC_NAL_TYPE(x) (((x) & 0x7E) >> 1)

#define H264_NAL_TYPE_SEI 6
#define H264_NAL_TYPE_SPS 7
#define H264_NAL_TYPE_PPS 8
#define H264_NAL_TYPE_AUD 9
#define H264_NAL_TYPE_FILLER 12
#define HEVC_NAL_TYPE_VPS 32
#define HEVC_NAL_TYPE_SPS 33
#define HEVC_NAL_TYPE_PPS 34
#define HEVC_NAL_TYPE_AUD 35
#define HEVC_NAL_TYPE_FILLER 38
#define HEVC_NAL_TYPE_SEI 39

// Init
void initializeVideoFrameAssembler(void) {
    LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);

    nextFrameNumber = 1;
    waitingForIdrFrame = true;
    frameHostProcessingLatency = 0;
    frameReceiveTimeUs = 0;
    framePresentationTimeUs = 0;
    frameTimestamp90Khz = 0;
    dropStatePending = false;
    idrFrameProcessed = false;
}

// Free the NAL chain
static void cleanupFrameState(void) {
    nalChainHead = nalChainTail = NULL;
    nalChainDataLength = 0;
    LiReleaseVideoBuffer(assemblingBuffer);
    assemblingBuffer = NULL;
}

// Cleanup frame state and set that we're waiting for an IDR Frame
static void dropFrameState(void) {
    dropStatePending = false;
    waitingForIdrFrame = true;

    // Count the number of consecutive frames dropped
    consecutiveFrameDrops++;

    // If we reach our limit, immediately request an IDR frame and reset
    if (consecutiveFrameDrops == CONSECUTIVE_DROP_LIMIT) {
        Limelog("Reached consecutive drop limit\n");

        // Restart the count
        consecutiveFrameDrops = 0;

        // Request an IDR frame
        waitingForIdrFrame = true;
        LiRequestIdrFrame();
    }

    cleanupFrameState();
}

// Cleanup the list of decode units
static void freeDecodeUnitList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        // Complete this with a failure status
        LiCompleteVideoFrame(entry->data, DR_CLEANUP);

        entry = nextEntry;
    }
}

void stopVideoFrameAssembler(void) {
    LbqSignalQueueShutdown(&decodeUnitQueue);
}

void destroyVideoFrameAssembler(void) {
    freeDecodeUnitList(LbqDestroyLinkedBlockingQueue(&decodeUnitQueue));
    cleanupFrameState();
}

// NB: This function also ensures an additional byte for the NALU type exists after the start sequence
static bool getAnnexBStartSequence(PBUFFER_DESC current, PBUFFER_DESC startSeq) {
    // We must not get called for other codecs
    LC_ASSERT(NegotiatedVideoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265));

    if (current->length <= 3) {
        return false;
    }

    if (current->data[current->offset] == 0 &&
        current->data[current->offset + 1] == 0) {
        if (current->data[current->offset + 2] == 0) {
            if (current->length > 4 && current->data[current->offset + 3] == 1) {
                // Frame start
                if (startSeq != NULL) {
                    startSeq->data = current->data;
                    startSeq->offset = current->offset;
                    startSeq->length = 4;
                }
                return true;
            }
        }
        else if (current->data[current->offset + 2] == 1) {
            // NAL start
            if (startSeq != NULL) {
                startSeq->data = current->data;
                startSeq->offset = current->offset;
                startSeq->length = 3;
            }
            return true;
        }
    }

    return false;
}

void validateDecodeUnitForPlayback(PDECODE_UNIT decodeUnit) {
    // Frames must always have at least one buffer
    LC_ASSERT(decodeUnit->bufferList != NULL);
    LC_ASSERT(decodeUnit->fullLength != 0);

    // Validate the buffers in the frame
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        // IDR frames always start with codec configuration data
        if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
            // H.264 IDR frames should have an SPS, PPS, then picture data
            LC_ASSERT_VT(decodeUnit->bufferList->bufferType == BUFFER_TYPE_SPS);
            LC_ASSERT_VT(decodeUnit->bufferList->next != NULL);
            LC_ASSERT_VT(decodeUnit->bufferList->next->bufferType == BUFFER_TYPE_PPS);
            LC_ASSERT_VT(decodeUnit->bufferList->next->next != NULL);
        }
        else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
            // HEVC IDR frames should have an VPS, SPS, PPS, then picture data
            LC_ASSERT_VT(decodeUnit->bufferList->bufferType == BUFFER_TYPE_VPS);
            LC_ASSERT_VT(decodeUnit->bufferList->next != NULL);
            LC_ASSERT_VT(decodeUnit->bufferList->next->bufferType == BUFFER_TYPE_SPS);
            LC_ASSERT_VT(decodeUnit->bufferList->next->next != NULL);
            LC_ASSERT_VT(decodeUnit->bufferList->next->next->bufferType == BUFFER_TYPE_PPS);
            LC_ASSERT_VT(decodeUnit->bufferList->next->next->next != NULL);
        }
        else {
            LC_ASSERT(false);
        }
    }
    else {
        LC_ASSERT(decodeUnit->frameType == FRAME_TYPE_PFRAME);

        // P frames always start with picture data
        LC_ASSERT(decodeUnit->bufferList->bufferType == BUFFER_TYPE_PICDATA);

        // We must not dequeue a P frame before an IDR frame has been successfully processed
        LC_ASSERT(idrFrameProcessed);
    }
}

bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqWaitForQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    validateDecodeUnitForPlayback(&qdu->decodeUnit);

    *frameHandle = qdu;
    *decodeUnit = &qdu->decodeUnit;
    return true;
}

bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqPollQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    validateDecodeUnitForPlayback(&qdu->decodeUnit);

    *frameHandle = qdu;
    *decodeUnit = &qdu->decodeUnit;
    return true;
}

bool LiPeekNextVideoFrame(PDECODE_UNIT* decodeUnit) {
    PQUEUED_DECODE_UNIT qdu;

    int err = LbqPeekQueueElement(&decodeUnitQueue, (void**)&qdu);
    if (err != LBQ_SUCCESS) {
        return false;
    }

    validateDecodeUnitForPlayback(&qdu->decodeUnit);

    *decodeUnit = &qdu->decodeUnit;
    return true;
}

void LiWakeWaitForVideoFrame(void) {
    LbqSignalQueueUserWake(&decodeUnitQueue);
}

// Cleanup a decode unit by freeing the buffer chain and the holder
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int drStatus) {
    PQUEUED_DECODE_UNIT qdu = handle;

    if (drStatus == DR_NEED_IDR) {
        Limelog("Requesting IDR frame on behalf of DR\n");
        requestDecoderRefresh();
    }
    else if (drStatus == DR_OK && qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
        // Remember that the IDR frame was processed. We can now use
        // reference frame invalidation.
        idrFrameProcessed = true;
    }

    // The queue record and NAL views are embedded in this same buffer.
    // A decoder may still hold a separate reference after this completion.
    LiReleaseVideoBuffer(qdu->decodeUnit.frameBuffer);
}

static bool isSeqReferenceFrameStart(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == 5;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        switch (HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length])) {
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
                return true;

            default:
                return false;
        }
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

static bool isAccessUnitDelimiter(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_AUD;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_AUD;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

static bool isSeiNal(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_SEI;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_SEI;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}

#ifdef LC_DEBUG
static bool isFillerDataNal(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    if (!getAnnexBStartSequence(buffer, &startSeq)) {
        return false;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        return H264_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == H264_NAL_TYPE_FILLER;
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        return HEVC_NAL_TYPE(startSeq.data[startSeq.offset + startSeq.length]) == HEVC_NAL_TYPE_FILLER;
    }
    else {
        LC_ASSERT(false);
        return false;
    }
}
#endif

// Advance the buffer descriptor to the start of the next NAL or end of buffer
static void skipToNextNalOrEnd(PBUFFER_DESC buffer) {
    BUFFER_DESC startSeq;

    // If we're starting on a NAL boundary, skip to the next one
    if (getAnnexBStartSequence(buffer, &startSeq)) {
        buffer->offset += startSeq.length;
        buffer->length -= startSeq.length;
    }

    // Loop until we find an Annex B start sequence (3 or 4 byte)
    while (!getAnnexBStartSequence(buffer, NULL)) {
        if (buffer->length == 0) {
            // Reached the end of the buffer
            return;
        }

        buffer->offset++;
        buffer->length--;
    }
}

// Reassemble the frame with the given frame number
static void reassembleFrame(int frameNumber) {
    if (nalChainHead != NULL) {
        PQUEUED_DECODE_UNIT qdu = &assemblingBuffer->queued;
        qdu->decodeUnit.frameBuffer = assemblingBuffer;
        qdu->decodeUnit.bufferList = nalChainHead;
        qdu->decodeUnit.fullLength = nalChainDataLength;
        qdu->decodeUnit.frameType = frameType;
        qdu->decodeUnit.frameNumber = frameNumber;
        qdu->decodeUnit.frameHostProcessingLatency = frameHostProcessingLatency;
        qdu->decodeUnit.receiveTimeUs = frameReceiveTimeUs;
        qdu->decodeUnit.presentationTimeUs = framePresentationTimeUs;
        qdu->decodeUnit.rtpTimestamp = frameTimestamp90Khz;
        qdu->decodeUnit.enqueueTimeUs = PltGetMicroseconds();

        qdu->decodeUnit.colorspace = (uint8_t)StreamConfig.colorSpace;

        // Invoke the key frame callback if needed
        if (nalChainHead->bufferType != BUFFER_TYPE_PICDATA || qdu->decodeUnit.frameType == FRAME_TYPE_IDR) {
            qdu->decodeUnit.frameType = FRAME_TYPE_IDR;
            notifyKeyFrameReceived();
        }
        else {
            qdu->decodeUnit.frameType = FRAME_TYPE_PFRAME;
        }

        nalChainHead = nalChainTail = NULL;
        nalChainDataLength = 0;
        assemblingBuffer = NULL;

        if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
            if (LbqOfferQueueItem(&decodeUnitQueue, qdu, &qdu->entry) != LBQ_SUCCESS) {
                Limelog("Video decode unit queue overflow\n");

                // RFI recovery is not supported here
                waitingForIdrFrame = true;

                // Clear NAL state for the frame that we failed to enqueue
                assemblingBuffer = qdu->decodeUnit.frameBuffer;
                nalChainHead = qdu->decodeUnit.bufferList;
                nalChainDataLength = qdu->decodeUnit.fullLength;
                dropFrameState();

                // Free all frames in the decode unit queue
                freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));

                // Request an IDR frame to recover
                LiRequestIdrFrame();
                return;
            }
        }
        else {
            // Submit the frame to the decoder
            validateDecodeUnitForPlayback(&qdu->decodeUnit);
            LiCompleteVideoFrame(qdu, VideoCallbacks.submitDecodeUnit(&qdu->decodeUnit));
        }

        // Notify the control connection
        connectionReceivedCompleteFrame(frameNumber, false);

        // Clear frame drops
        consecutiveFrameDrops = 0;

    }
}

static int getBufferFlags(char* data, int length) {
    BUFFER_DESC buffer;
    BUFFER_DESC candidate;

    // We only parse H.264 and HEVC bitstreams
    if (!(NegotiatedVideoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265))) {
        return BUFFER_TYPE_PICDATA;
    }

    buffer.data = data;
    buffer.length = (unsigned int)length;
    buffer.offset = 0;

    if (!getAnnexBStartSequence(&buffer, &candidate)) {
        return BUFFER_TYPE_PICDATA;
    }

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H264) {
        switch (H264_NAL_TYPE(candidate.data[candidate.offset + candidate.length])) {
        case H264_NAL_TYPE_SPS:
            return BUFFER_TYPE_SPS;

        case H264_NAL_TYPE_PPS:
            return BUFFER_TYPE_PPS;

        default:
            return BUFFER_TYPE_PICDATA;
        }
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        switch (HEVC_NAL_TYPE(candidate.data[candidate.offset + candidate.length])) {
            case HEVC_NAL_TYPE_SPS:
                return BUFFER_TYPE_SPS;

            case HEVC_NAL_TYPE_PPS:
                return BUFFER_TYPE_PPS;

            case HEVC_NAL_TYPE_VPS:
                return BUFFER_TYPE_VPS;

            default:
                return BUFFER_TYPE_PICDATA;
        }
    }
    else {
        LC_ASSERT(false);
        return BUFFER_TYPE_PICDATA;
    }
}

static bool queueFragment(char* data, int offset, int length) {
    if (length <= 0 || assemblingBuffer->entryCount == 4) return false;
    PLENTRY entry = &assemblingBuffer->entries[assemblingBuffer->entryCount++];
    entry->next = NULL;
    entry->length = length;
    entry->data = &data[offset];
    entry->bufferType = getBufferFlags(entry->data, length);
    nalChainDataLength += length;
    if (nalChainTail) nalChainTail->next = entry;
    else nalChainHead = entry;
    nalChainTail = entry;
    return true;
}

// Split one complete native H.264/HEVC key frame into decoder buffer entries.
static bool processAvcHevcKeyFrame(PBUFFER_DESC currentPos) {
    // We should not have any NALUs when processing a new key frame.
    LC_ASSERT(nalChainHead == NULL);
    LC_ASSERT(nalChainTail == NULL);

    while (currentPos->length != 0) {
        // Skip through any padding bytes
        if (!getAnnexBStartSequence(currentPos, NULL)) {
            skipToNextNalOrEnd(currentPos);
            if (currentPos->length == 0) return true;
        }

        // Skip prepended AUD or SEI NALUs and any padding between them.
        while (isAccessUnitDelimiter(currentPos) || isSeiNal(currentPos)) {
            skipToNextNalOrEnd(currentPos);
            if (currentPos->length == 0) return true;
        }

        int start = currentPos->offset;
        bool containsPicData = false;

        if (isSeqReferenceFrameStart(currentPos)) {
            waitingForIdrFrame = false;
            containsPicData = true;
            frameType = FRAME_TYPE_IDR;
        }

        // Move to the next NALU
        skipToNextNalOrEnd(currentPos);

        // Picture data extends through the end of the complete frame.
        if (containsPicData) {
            while (currentPos->length != 0) {
                // Any NALUs through the end of the frame must be
                // reference frame slices or filler data.
                LC_ASSERT_VT(isSeqReferenceFrameStart(currentPos) || isFillerDataNal(currentPos));
                skipToNextNalOrEnd(currentPos);
            }
        }

        if (!queueFragment(currentPos->data, start, currentPos->offset - start)) return false;
    }
    return true;
}

int LiSubmitPlankVideoFrame(PLANK_VIDEO_BUFFER* buffer,
                                     int frameLength,
                                     uint32_t frameNumber,
                                     uint32_t flags,
                                     uint64_t pts90Khz,
                                     uint16_t hostProcessingLatency) {
    BUFFER_DESC currentPos;
    bool keyFrame;

    if (buffer == NULL || frameLength <= 0 || frameLength > buffer->capacity ||
            (flags & ~PLANK_VIDEO_FRAME_FLAG_KEY) != 0 ||
            !(NegotiatedVideoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265))) {
        return -1;
    }

    if (atomic_exchange(&buffer->submitted, true)) return -1;
    const unsigned char* frame = buffer->data;
    buffer->length = frameLength;
    memset(buffer->data + frameLength, 0, PLANK_VIDEO_BUFFER_PADDING);

    keyFrame = (flags & PLANK_VIDEO_FRAME_FLAG_KEY) != 0;

    // The native receiver is the sole frame producer on this data plane. A
    // decoder refresh may still request a deferred state drop from the control
    // thread, so honor it only at this complete-frame boundary.
    if (dropStatePending) {
        dropFrameState();
    }

    // A gap can come from transport loss OR sender-side recovery skips. An
    // arriving keyframe already repairs the reference chain: requesting another
    // here makes senders which skip in-flight deltas request recovery forever.
    // Do not trust the key flag alone; validate its Annex-B contents below.
    if (frameNumber < nextFrameNumber) {
        return 1;
    }
    if (frameNumber > nextFrameNumber) {
        cleanupFrameState();
        waitingForIdrFrame = true;
        if (!keyFrame) {
            // The assembler requires an IDR, not reference invalidation, and
            // must schedule only one request for this discontinuity.
            LiRequestIdrFrame();
        }
    }

    if (waitingForIdrFrame && !keyFrame) {
        nextFrameNumber = frameNumber + 1;
        return 1;
    }

    cleanupFrameState();
    LiRetainVideoBuffer(buffer);
    assemblingBuffer = buffer;
    frameType = FRAME_TYPE_PFRAME;
    frameHostProcessingLatency = hostProcessingLatency;
    frameReceiveTimeUs = PltGetMicroseconds();
    framePresentationTimeUs = (pts90Khz * 1000000ULL) / 90000ULL;
    frameTimestamp90Khz = (uint32_t)pts90Khz;

    currentPos.data = (char*)frame;
    currentPos.offset = 0;
    currentPos.length = (unsigned int)frameLength;
    if (keyFrame) {
        const bool complete = processAvcHevcKeyFrame(&currentPos);
        PLENTRY entry = nalChainHead;
        if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
            entry = entry && entry->bufferType == BUFFER_TYPE_VPS ? entry->next : NULL;
        }
        entry = entry && entry->bufferType == BUFFER_TYPE_SPS ? entry->next : NULL;
        entry = entry && entry->bufferType == BUFFER_TYPE_PPS ? entry->next : NULL;
        if (!complete || !entry || entry->next || entry->bufferType != BUFFER_TYPE_PICDATA || frameType != FRAME_TYPE_IDR) {
            cleanupFrameState();
            waitingForIdrFrame = true;
            return -1;
        }
    }
    else {
        if (!queueFragment(currentPos.data, 0, frameLength)) {
            cleanupFrameState();
            waitingForIdrFrame = true;
            return -1;
        }
    }

    nextFrameNumber = frameNumber + 1;
    reassembleFrame((int)frameNumber);
    return 0;
}

// Dumps the decode unit queue and ensures the next frame submitted to the decoder will be
// an IDR frame
void requestDecoderRefresh(void) {
    // Wait for the next IDR frame
    waitingForIdrFrame = true;

    // Flush the decode unit queue
    freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));

    // Request the receive thread drop its state
    // on the next call. We can't do it here because
    // it may be trying to queue DUs and we'll nuke
    // the state out from under it.
    dropStatePending = true;

    // Request the IDR frame
    LiRequestIdrFrame();
}

int LiGetPendingVideoFrames(void) {
    return LbqGetItemCount(&decodeUnitQueue);
}
