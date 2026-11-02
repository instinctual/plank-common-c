#include "Limelight-internal.h"
#include <stdatomic.h>

typedef struct _QUEUED_REFERENCE_FRAME_CONTROL {
    uint32_t startFrame;
    uint32_t endFrame;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_REFERENCE_FRAME_CONTROL, *PQUEUED_REFERENCE_FRAME_CONTROL;

typedef enum _PLANK_CALLBACK_TYPE {
    SC_CALLBACK_RAW_HID_CONTROL,
    SC_CALLBACK_VIDEO_BITRATE_APPLIED,
    SC_CALLBACK_CURSOR_SHAPE,
    SC_CALLBACK_CURSOR_POSITION,
} PLANK_CALLBACK_TYPE;

typedef struct _QUEUED_ASYNC_CALLBACK {
    PLANK_CALLBACK_TYPE type;
    unsigned char* variableData;
    unsigned int variableLength;
    union {
        struct {
            uint32_t requestedKbps;
            uint32_t appliedKbps;
            uint32_t peakKbps;
        } videoBitrateApplied;
    } data;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_ASYNC_CALLBACK, *PQUEUED_ASYNC_CALLBACK;

static PLT_THREAD invalidateRefFramesThread;
static PLT_THREAD requestIdrFrameThread;
static PLT_THREAD asyncCallbackThread;
static LINKED_BLOCKING_QUEUE referenceFrameControlQueue;
static LINKED_BLOCKING_QUEUE asyncCallbackQueue;
static PLT_EVENT idrFrameRequiredEvent;

static PlankNativeControlSender nativeControlSender;
static void* nativeControlSenderContext;
static atomic_bool stopping = true;
static atomic_bool initialized;
static bool started;
static bool referenceFrameThreadStarted;
// One atomic snapshot keeps callback type and start time consistent for the
// failure diagnostic. Zero means idle; low byte is type+1, upper bits are ms.
static atomic_uint_fast64_t activeAsyncCallback;
static uint32_t lastSeenFrame;

// A large 4K IDR can occupy the configured stream for several hundred
// milliseconds. Keep retries far enough apart that they cannot overlap and
// amplify loss into an IDR bitrate storm.
#define IDR_FRAME_REQUEST_MIN_INTERVAL_MS 1000

static void freeReferenceFrameList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    while (entry != NULL) {
        PLINKED_BLOCKING_QUEUE_ENTRY nextEntry = entry->flink;
        free(entry->data);
        entry = nextEntry;
    }
}

static void freeAsyncCallbackList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    while (entry != NULL) {
        PQUEUED_ASYNC_CALLBACK queuedCb =
            (PQUEUED_ASYNC_CALLBACK)entry->data;
        PLINKED_BLOCKING_QUEUE_ENTRY nextEntry = entry->flink;
        free(queuedCb->variableData);
        free(queuedCb);
        entry = nextEntry;
    }
}

int initializeControlStream(void) {
    if (nativeControlSender == NULL) {
        Limelog("PLANK native control sender is required\n");
        return -1;
    }

    stopping = false;
    initialized = true;
    started = false;
    referenceFrameThreadStarted = false;
    atomic_store(&activeAsyncCallback, 0);
    lastSeenFrame = 0;

    PltCreateEvent(&idrFrameRequiredEvent);
    LbqInitializeLinkedBlockingQueue(&referenceFrameControlQueue, 20);
    LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 30);
    return 0;
}

void destroyControlStream(void) {
    LC_ASSERT(stopping);
    LC_ASSERT(!started);

    PltCloseEvent(&idrFrameRequiredEvent);
    freeReferenceFrameList(
        LbqDestroyLinkedBlockingQueue(&referenceFrameControlQueue));
    freeAsyncCallbackList(
        LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue));
    initialized = false;
}

static void terminateForNativeSendFailure(const char* operation, int error) {
    Limelog("Native %s failed: %d\n", operation, error);
    ListenerCallbacks.connectionTerminated(error < 0 ? error : -1);
}

static bool sendNativeControl(uint32_t type, uint32_t value1,
                              uint32_t value2, const char* operation) {
    int result;

    if (nativeControlSender == NULL) {
        terminateForNativeSendFailure(operation, -1);
        return false;
    }

    result = nativeControlSender(nativeControlSenderContext, type, value1,
                                 value2);
    if (result != 0) {
        terminateForNativeSendFailure(operation, result);
        return false;
    }
    return true;
}

static void requestIdrFrame(void) {
    if (sendNativeControl(LI_SC_NATIVE_CONTROL_REQUEST_IDR, 0, 0,
                          "IDR frame request")) {
        Limelog("Native IDR frame request sent\n");
    }
}

static void requestInvalidateReferenceFrames(uint32_t startFrame,
                                             uint32_t endFrame) {
    LC_ASSERT(startFrame <= endFrame);

    if (sendNativeControl(
            LI_SC_NATIVE_CONTROL_INVALIDATE_REFERENCE_FRAMES,
            startFrame, endFrame, "reference-frame invalidation request")) {
        Limelog("Native invalidate reference frame request sent (%u to %u)\n",
                startFrame, endFrame);
    }
}

void LiRequestIdrFrame(void) {
    if (!initialized || stopping) {
        return;
    }

    freeReferenceFrameList(
        LbqFlushQueueItems(&referenceFrameControlQueue));
    PltSetEvent(&idrFrameRequiredEvent);
}

void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    PQUEUED_REFERENCE_FRAME_CONTROL request;

    if (!initialized || stopping || startFrame > endFrame) {
        return;
    }
    if (!isReferenceFrameInvalidationEnabled()) {
        LiRequestIdrFrame();
        return;
    }

    request = malloc(sizeof(*request));
    if (request == NULL) {
        LiRequestIdrFrame();
        return;
    }
    request->startFrame = startFrame;
    request->endFrame = endFrame;
    if (LbqOfferQueueItem(&referenceFrameControlQueue, request,
                          &request->entry) != LBQ_SUCCESS) {
        free(request);
        LiRequestIdrFrame();
    }
}

void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) {
    (void)frameIsLTR;
    lastSeenFrame = frameIndex;
}

static void requestIdrFrameFunc(void* context) {
    uint64_t lastIdrRequestTimeMs = 0;
    (void)context;

    while (!PltIsThreadInterrupted(&requestIdrFrameThread)) {
        uint64_t now;

        PltWaitForEvent(&idrFrameRequiredEvent);
        PltClearEvent(&idrFrameRequiredEvent);
        if (stopping) {
            return;
        }

        now = PltGetMillis();
        if (lastIdrRequestTimeMs != 0 &&
                now - lastIdrRequestTimeMs <
                    IDR_FRAME_REQUEST_MIN_INTERVAL_MS) {
            uint64_t delayMs = IDR_FRAME_REQUEST_MIN_INTERVAL_MS -
                               (now - lastIdrRequestTimeMs);
            Limelog("Delaying repeated IDR frame request by %llu ms\n",
                    (unsigned long long)delayMs);
            PltSleepMs((int)delayMs);
            if (stopping ||
                    PltIsThreadInterrupted(&requestIdrFrameThread)) {
                return;
            }
        }

        freeReferenceFrameList(
            LbqFlushQueueItems(&referenceFrameControlQueue));
        requestIdrFrame();
        lastIdrRequestTimeMs = PltGetMillis();
    }
}

static void referenceFrameControlFunc(void* context) {
    (void)context;

    while (!PltIsThreadInterrupted(&invalidateRefFramesThread)) {
        PQUEUED_REFERENCE_FRAME_CONTROL request;
        uint32_t startFrame;
        uint32_t endFrame;

        if (LbqWaitForQueueElement(&referenceFrameControlQueue,
                                   (void**)&request) != LBQ_SUCCESS) {
            return;
        }

        startFrame = request->startFrame;
        endFrame = request->endFrame;
        free(request);
        while (LbqPollQueueElement(&referenceFrameControlQueue,
                                   (void**)&request) == LBQ_SUCCESS) {
            if (request->startFrame < startFrame) {
                startFrame = request->startFrame;
            }
            if (request->endFrame > endFrame) {
                endFrame = request->endFrame;
            }
            free(request);
        }
        requestInvalidateReferenceFrames(startFrame, endFrame);
    }
}

static bool callbackMatches(const void* data, const void* context) {
    return ((const QUEUED_ASYNC_CALLBACK*)data)->type ==
           ((const QUEUED_ASYNC_CALLBACK*)context)->type;
}

static void asyncCallbackThreadFunc(void* context) {
    PQUEUED_ASYNC_CALLBACK queuedCb;
    PQUEUED_ASYNC_CALLBACK nextCb;
    (void)context;

    while (LbqWaitForQueueElement(&asyncCallbackQueue,
                                  (void**)&queuedCb) == LBQ_SUCCESS) {
        atomic_store(&activeAsyncCallback,
                     (PltGetMillis() << 8) | (queuedCb->type + 1));
        switch (queuedCb->type) {
        case SC_CALLBACK_RAW_HID_CONTROL:
            if (ListenerCallbacks.rawHidControl != NULL) {
                ListenerCallbacks.rawHidControl(queuedCb->variableData,
                                                queuedCb->variableLength);
            }
            break;
        case SC_CALLBACK_VIDEO_BITRATE_APPLIED:
            if (ListenerCallbacks.videoBitrateApplied != NULL) {
                ListenerCallbacks.videoBitrateApplied(
                    queuedCb->data.videoBitrateApplied.requestedKbps,
                    queuedCb->data.videoBitrateApplied.appliedKbps,
                    queuedCb->data.videoBitrateApplied.peakKbps);
            }
            break;
        case SC_CALLBACK_CURSOR_SHAPE:
            if (ListenerCallbacks.cursorChunk != NULL) {
                ListenerCallbacks.cursorChunk(queuedCb->variableData,
                                              queuedCb->variableLength);
            }
            break;
        case SC_CALLBACK_CURSOR_POSITION:
            while (LbqPollQueueElementIf(&asyncCallbackQueue,
                      (void**)&nextCb, callbackMatches, queuedCb) == LBQ_SUCCESS) {
                LC_ASSERT(nextCb != queuedCb);
                free(queuedCb->variableData);
                free(queuedCb);
                queuedCb = nextCb;
            }
            if (ListenerCallbacks.cursorPosition != NULL) {
                ListenerCallbacks.cursorPosition(queuedCb->variableData,
                                                 queuedCb->variableLength);
            }
            break;
        default:
            LC_ASSERT(false);
            break;
        }

        atomic_store(&activeAsyncCallback, 0);
        free(queuedCb->variableData);
        free(queuedCb);
    }
}

int startControlStream(void) {
    int err;

    if (!initialized || nativeControlSender == NULL) {
        Limelog("PLANK native control sender is required\n");
        return -1;
    }

    err = PltCreateThread("ReqIdrFrame", requestIdrFrameFunc, NULL,
                          &requestIdrFrameThread);
    if (err != 0) {
        stopping = true;
        return err;
    }

    err = PltCreateThread("CtrlAsyncCb", asyncCallbackThreadFunc, NULL,
                          &asyncCallbackThread);
    if (err != 0) {
        stopping = true;
        PltSetEvent(&idrFrameRequiredEvent);
        PltInterruptThread(&requestIdrFrameThread);
        PltJoinThread(&requestIdrFrameThread);
        return err;
    }

    if (isReferenceFrameInvalidationEnabled()) {
        err = PltCreateThread("InvRefFrames", referenceFrameControlFunc,
                              NULL, &invalidateRefFramesThread);
        if (err != 0) {
            stopping = true;
            PltSetEvent(&idrFrameRequiredEvent);
            LbqSignalQueueShutdown(&asyncCallbackQueue);
            PltInterruptThread(&requestIdrFrameThread);
            PltJoinThread(&requestIdrFrameThread);
            PltInterruptThread(&asyncCallbackThread);
            PltJoinThread(&asyncCallbackThread);
            return err;
        }
        referenceFrameThreadStarted = true;
    }

    started = true;
    Limelog("Native KyProto control workers started\n");
    return 0;
}

int stopControlStream(void) {
    if (!started) {
        stopping = true;
        return 0;
    }

    stopping = true;
    LbqSignalQueueShutdown(&referenceFrameControlQueue);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    PltSetEvent(&idrFrameRequiredEvent);

    PltInterruptThread(&requestIdrFrameThread);
    PltInterruptThread(&asyncCallbackThread);
    PltJoinThread(&requestIdrFrameThread);
    PltJoinThread(&asyncCallbackThread);

    if (referenceFrameThreadStarted) {
        PltInterruptThread(&invalidateRefFramesThread);
        PltJoinThread(&invalidateRefFramesThread);
        referenceFrameThreadStarted = false;
    }
    started = false;
    return 0;
}

static bool isCursorPositionCallback(const void* data) {
    return ((const QUEUED_ASYNC_CALLBACK*)data)->type == SC_CALLBACK_CURSOR_POSITION;
}

static const char* callbackName(unsigned type) {
    switch (type) {
    case SC_CALLBACK_RAW_HID_CONTROL: return "tablet-control";
    case SC_CALLBACK_VIDEO_BITRATE_APPLIED: return "bitrate-confirmation";
    case SC_CALLBACK_CURSOR_SHAPE: return "cursor-shape";
    case SC_CALLBACK_CURSOR_POSITION: return "cursor-position";
    default: return "idle";
    }
}

static bool queueAsyncCallback(PQUEUED_ASYNC_CALLBACK queuedCb) {
    PQUEUED_ASYNC_CALLBACK replaced = NULL;
    const PLANK_CALLBACK_TYPE type = queuedCb->type;
    int result = LbqOfferQueueItemTimed(&asyncCallbackQueue, queuedCb,
        &queuedCb->entry, 100,
        queuedCb->type == SC_CALLBACK_CURSOR_POSITION ? isCursorPositionCallback : NULL,
        (void**)&replaced);
    if (replaced != NULL) {
        free(replaced->variableData);
        free(replaced);
    }
    if (result == LBQ_SUCCESS) {
        return true;
    }

    free(queuedCb->variableData);
    free(queuedCb);
    if (result == LBQ_BOUND_EXCEEDED && !atomic_exchange(&stopping, true)) {
        const uint64_t active = atomic_load(&activeAsyncCallback);
        Limelog("Native callback backlog: incoming=%s active=%s active-ms=%llu limit=30 wait-ms=100\n",
                callbackName(type), callbackName((unsigned)(active & 0xff) - 1),
                (unsigned long long)(active ? PltGetMillis() - (active >> 8) : 0));
        // A lost feature-report request or attach acknowledgement can leave
        // the tablet wedged for the rest of the session. Fail visibly instead.
        terminateForNativeSendFailure("reliable callback queue remained full", -1);
    }
    return false;
}

static void queueNativeVariableCallback(PLANK_CALLBACK_TYPE type,
                                        const unsigned char* data,
                                        unsigned int length) {
    PQUEUED_ASYNC_CALLBACK queuedCb;

    if (!initialized || stopping || data == NULL || length == 0) {
        return;
    }
    queuedCb = calloc(1, sizeof(*queuedCb));
    if (queuedCb == NULL) {
        return;
    }
    queuedCb->variableData = malloc(length);
    if (queuedCb->variableData == NULL) {
        free(queuedCb);
        return;
    }
    memcpy(queuedCb->variableData, data, length);
    queuedCb->variableLength = length;
    queuedCb->type = type;
    queueAsyncCallback(queuedCb);
}

void LiNotifyPlankVideoBitrateApplied(uint32_t requestedKbps,
                                               uint32_t appliedKbps,
                                               uint32_t peakKbps) {
    PQUEUED_ASYNC_CALLBACK queuedCb;

    if (!initialized || stopping ||
            ListenerCallbacks.videoBitrateApplied == NULL) {
        return;
    }
    queuedCb = calloc(1, sizeof(*queuedCb));
    if (queuedCb == NULL) {
        return;
    }
    queuedCb->type = SC_CALLBACK_VIDEO_BITRATE_APPLIED;
    queuedCb->data.videoBitrateApplied.requestedKbps = requestedKbps;
    queuedCb->data.videoBitrateApplied.appliedKbps = appliedKbps;
    queuedCb->data.videoBitrateApplied.peakKbps = peakKbps;
    queueAsyncCallback(queuedCb);
}

void LiNotifyPlankRawHidControl(const unsigned char* data,
                                         unsigned int length) {
    if (length < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
            length > sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                         PLANK_RAW_HID_MAX_PAYLOAD_SIZE) {
        return;
    }
    queueNativeVariableCallback(SC_CALLBACK_RAW_HID_CONTROL, data, length);
}

void LiNotifyPlankCursorChunk(const unsigned char* data,
                                       unsigned int length) {
    if (length < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
            length > sizeof(PLANK_CURSOR_WIRE_HEADER) +
                         PLANK_CURSOR_MAX_CHUNK_SIZE) {
        return;
    }
    queueNativeVariableCallback(SC_CALLBACK_CURSOR_SHAPE, data, length);
}

void LiNotifyPlankCursorPosition(const unsigned char* data,
                                          unsigned int length) {
    if (length != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) {
        return;
    }
    queueNativeVariableCallback(SC_CALLBACK_CURSOR_POSITION, data, length);
}

void LiNotifyPlankHostTermination(uint32_t errorCode) {
    if (!initialized || stopping) {
        return;
    }

    Limelog("Host notified native termination reason: 0x%08x\n", errorCode);
    switch (errorCode) {
    case 0x800e9403:
        errorCode = ML_ERROR_FRAME_CONVERSION;
        break;
    case 0x800e9302:
        errorCode = ML_ERROR_PROTECTED_CONTENT;
        break;
    case 0x80030023:
        errorCode = lastSeenFrame != 0 ?
                    ML_ERROR_GRACEFUL_TERMINATION :
                    ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
        break;
    default:
        break;
    }
    ListenerCallbacks.connectionTerminated((int)errorCode);
}

void LiSetPlankNativeControlSender(
        PlankNativeControlSender sender, void* context) {
    nativeControlSender = sender;
    nativeControlSenderContext = context;
}

int LiSetVideoBitrate(int bitrateKbps) {
    if (!(SunshineFeatureFlags & LI_FF_DYNAMIC_VIDEO_BITRATE) ||
            bitrateKbps < 500 || bitrateKbps > 500000 ||
            nativeControlSender == NULL) {
        return -1;
    }

    return nativeControlSender(nativeControlSenderContext,
                               LI_SC_NATIVE_CONTROL_SET_VIDEO_BITRATE,
                               (uint32_t)bitrateKbps, 0);
}
