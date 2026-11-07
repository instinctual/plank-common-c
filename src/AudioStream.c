#include "Limelight-internal.h"

static int64_t nextAudioPresentationTimeUs;

int initializeAudioStream(void) {
    nextAudioPresentationTimeUs = -1;
    return 0;
}

void destroyAudioStream(void) {
}

int LiSubmitPlankAudioPacket(const unsigned char* packet,
                                      int packetLength,
                                      uint16_t frameSamples,
                                      uint32_t missingSamples,
                                      uint64_t ptsMs) {
    if (frameSamples == 0 || frameSamples > 5760 || packetLength < 0 ||
            (packetLength != 0 && packet == NULL) ||
            (missingSamples != 0 && packetLength != 0) ||
            frameSamples % 48 != 0 || missingSamples > 48000 ||
            (missingSamples != 0 && missingSamples % frameSamples != 0) ||
            (missingSamples == 0 && ptsMs > (uint64_t)(INT64_MAX / 1000 - 1000))) {
        return -1;
    }

    const int64_t frameDurationUs = (int64_t)frameSamples * 1000000 / 48000;
    if (missingSamples != 0) {
        uint32_t missingFrames = missingSamples / frameSamples;
        while (missingFrames-- != 0) {
            AudioCallbacks.decodeAndPlaySample(NULL, 0, nextAudioPresentationTimeUs);
            if (nextAudioPresentationTimeUs >= 0) {
                if (nextAudioPresentationTimeUs > INT64_MAX - frameDurationUs)
                    nextAudioPresentationTimeUs = -1;
                else
                    nextAudioPresentationTimeUs += frameDurationUs;
            }
        }
    }
    else {
        const int64_t presentationTimeUs = (int64_t)ptsMs * 1000;
        AudioCallbacks.decodeAndPlaySample((char*)packet, packetLength, presentationTimeUs);
        nextAudioPresentationTimeUs = presentationTimeUs + frameDurationUs;
    }

    return 0;
}

void stopAudioStream(void) {
    AudioCallbacks.stop();
    AudioCallbacks.cleanup();
}

int startAudioStream(void* audioContext, int arFlags) {
    OPUS_MULTISTREAM_CONFIGURATION chosenConfig;

    if (HighQualitySurroundEnabled) {
        LC_ASSERT(HighQualitySurroundSupported);
        LC_ASSERT(HighQualityOpusConfig.channelCount != 0);
        LC_ASSERT(HighQualityOpusConfig.streams != 0);
        chosenConfig = HighQualityOpusConfig;
    }
    else {
        LC_ASSERT(NormalQualityOpusConfig.channelCount != 0);
        LC_ASSERT(NormalQualityOpusConfig.streams != 0);
        chosenConfig = NormalQualityOpusConfig;
    }

    chosenConfig.samplesPerFrame = 48 * AudioPacketDuration;
    const int err = AudioCallbacks.init(StreamConfig.audioConfiguration,
                                        &chosenConfig, audioContext, arFlags);
    if (err != 0) {
        return err;
    }

    AudioCallbacks.start();
    return 0;
}
