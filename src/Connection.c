#include "Limelight-internal.h"

static int stage = STAGE_NONE;
static ConnListenerConnectionTerminated originalTerminationCallback;
static bool alreadyTerminated;
static PLT_THREAD terminationCallbackThread;
static int terminationCallbackErrorCode;
static bool NativeSessionConfigurationPending;
static PLANK_NATIVE_SESSION_CONFIGURATION NativeSessionConfiguration;
static uint32_t ActiveServiceFlags;

// Common globals
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
int NegotiatedVideoFormat;
volatile bool ConnectionInterrupted;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
int AudioPacketDuration;
bool ReferenceFrameInvalidationSupported;
uint32_t SunshineFeatureFlags;

// Connection stages
static const char* stageNames[STAGE_MAX] = {
    "none",
    "platform initialization",
    "audio stream initialization",
    "session negotiation",
    "control stream initialization",
    "video stream initialization",
    "input stream initialization",
    "control stream establishment",
    "video stream establishment",
    "audio stream establishment",
    "input stream establishment"
};

// Get the name of the current stage based on its number
const char* LiGetStageName(int stage) {
    return stageNames[stage];
}

int LiSetPlankNativeSessionConfiguration(
    const PLANK_NATIVE_SESSION_CONFIGURATION* configuration) {
    if (stage != STAGE_NONE) {
        return -1;
    }
    // A rejected replacement must not leave an older launch consumable.
    NativeSessionConfigurationPending = false;
    memset(&NativeSessionConfiguration, 0, sizeof(NativeSessionConfiguration));
    if (configuration == NULL ||
            configuration->structSize != sizeof(*configuration) ||
            configuration->sessionPort == 0 ||
            configuration->sessionPort > UINT16_MAX ||
            configuration->negotiatedVideoFormat == 0 ||
            (configuration->serviceFlags & ~PLANK_NATIVE_SERVICE_MASK) != 0 ||
            ((configuration->serviceFlags & PLANK_NATIVE_SERVICE_LOCAL_CURSOR) != 0) !=
                ((configuration->hostFeatureFlags & LI_FF_LOCAL_CURSOR) != 0) ||
            configuration->referenceFrameInvalidationSupported > 1) {
        return -1;
    }
    if (configuration->serviceFlags & PLANK_NATIVE_SERVICE_AUDIO) {
        if (configuration->audioPacketDurationMs == 0 ||
            configuration->audioPacketDurationMs > 120 ||
            configuration->opusConfiguration.sampleRate != 48000 ||
            configuration->opusConfiguration.channelCount <= 0 ||
            configuration->opusConfiguration.channelCount >
                AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT ||
            configuration->opusConfiguration.streams <= 0 ||
            configuration->opusConfiguration.streams >
                configuration->opusConfiguration.channelCount ||
            configuration->opusConfiguration.coupledStreams < 0 ||
            configuration->opusConfiguration.coupledStreams >
                configuration->opusConfiguration.streams ||
            configuration->opusConfiguration.streams +
                configuration->opusConfiguration.coupledStreams !=
                configuration->opusConfiguration.channelCount) {
            return -1;
        }
        for (int i = 0; i < configuration->opusConfiguration.channelCount; i++) {
            if (configuration->opusConfiguration.mapping[i] >=
                    configuration->opusConfiguration.channelCount) {
                return -1;
            }
        }
    }
    else if (configuration->audioPacketDurationMs != 0 ||
             configuration->opusConfiguration.sampleRate != 0 ||
             configuration->opusConfiguration.channelCount != 0 ||
             configuration->opusConfiguration.streams != 0 ||
             configuration->opusConfiguration.coupledStreams != 0) {
        return -1;
    }
    NativeSessionConfiguration = *configuration;
    NativeSessionConfigurationPending = true;
    return 0;
}

// Interrupt a pending connection attempt. This interruption happens asynchronously
// so it is not safe to start another connection before LiStartConnection() returns.
void LiInterruptConnection(void) {
    // Signal anyone waiting on the global interrupted flag
    ConnectionInterrupted = true;
}

uint32_t LiGetPlankNativeServiceFlags(void) {
    return ActiveServiceFlags;
}

// Stop the connection by undoing the step at the current stage and those before it
void LiStopConnection(void) {
    // Disable termination callbacks now
    alreadyTerminated = true;

    // Set the interrupted flag
    LiInterruptConnection();

    if (stage == STAGE_INPUT_STREAM_START) {
        Limelog("Stopping input stream...");
        if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_INPUT) stopInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_AUDIO_STREAM_START) {
        Limelog("Stopping audio stream...");
        if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_AUDIO) stopAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_START) {
        Limelog("Stopping video stream...");
        stopVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_START) {
        Limelog("Stopping control stream...");
        stopControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_INPUT_STREAM_INIT) {
        Limelog("Cleaning up input stream...");
        if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_INPUT) destroyInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_INIT) {
        Limelog("Cleaning up video stream...");
        destroyVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_INIT) {
        Limelog("Cleaning up control stream...");
        destroyControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_SESSION_NEGOTIATION) {
        // Nothing to do
        stage--;
    }
    if (stage == STAGE_AUDIO_STREAM_INIT) {
        Limelog("Cleaning up audio stream...");
        if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_AUDIO) destroyAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_PLATFORM_INIT) {
        Limelog("Cleaning up platform...");
        cleanupPlatform();
        stage--;
        Limelog("done\n");
    }
    LC_ASSERT(stage == STAGE_NONE);
    ActiveServiceFlags = 0;
    

}

static void terminationCallbackThreadFunc(void* context)
{
    // Invoke the client's termination callback
    originalTerminationCallback(terminationCallbackErrorCode);
}

// This shim callback runs the client's connectionTerminated() callback on a
// separate thread. This is neccessary because other internal threads directly
// invoke this callback. That can result in a deadlock if the client
// calls LiStopConnection() in the callback when the cleanup code
// attempts to join the thread that the termination callback (and LiStopConnection)
// is running on.
static void ClInternalConnectionTerminated(int errorCode)
{
    int err;

    // Avoid recursion and issuing multiple callbacks
    if (alreadyTerminated || ConnectionInterrupted) {
        return;
    }

    terminationCallbackErrorCode = errorCode;
    alreadyTerminated = true;

    // Invoke the termination callback on a separate thread
    err = PltCreateThread("AsyncTerm", terminationCallbackThreadFunc, NULL, &terminationCallbackThread);
    if (err != 0) {
        // Nothing we can safely do here, so we'll just assert on debug builds
        Limelog("Failed to create termination thread: %d\n", err);
        LC_ASSERT(err == 0);
    }

    // Detach the thread since we never wait on it
    PltDetachThread(&terminationCallbackThread);
}

// Starts the connection to the streaming machine
int LiStartConnection(PSERVER_INFORMATION serverInfo, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
    PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, void* renderContext, int drFlags,
    void* audioContext, int arFlags) {
    int err;

    if (!NativeSessionConfigurationPending) {
        Limelog("PLANK native session configuration is required\n");
        return -1;
    }
    ActiveServiceFlags = NativeSessionConfiguration.serviceFlags;

    if (drCallbacks != NULL && (drCallbacks->capabilities & CAPABILITY_PULL_RENDERER) && drCallbacks->submitDecodeUnit) {
        Limelog("CAPABILITY_PULL_RENDERER cannot be set with a submitDecodeUnit callback\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    if (drCallbacks != NULL && (drCallbacks->capabilities & CAPABILITY_PULL_RENDERER) && (drCallbacks->capabilities & CAPABILITY_DIRECT_SUBMIT)) {
        Limelog("CAPABILITY_PULL_RENDERER and CAPABILITY_DIRECT_SUBMIT cannot be set together\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    if (serverInfo->serverCodecModeSupport == 0) {
        Limelog("serverCodecModeSupport field in SERVER_INFORMATION must be set!\n");
        LC_ASSERT(false);
        err = -1;
        goto Cleanup;
    }

    // Native PLANK setup is authoritative, not an upstream appversion string.

    // Replace missing callbacks with placeholders
    fixupMissingCallbacks(&drCallbacks, &arCallbacks, &clCallbacks);
    memcpy(&VideoCallbacks, drCallbacks, sizeof(VideoCallbacks));
    memcpy(&AudioCallbacks, arCallbacks, sizeof(AudioCallbacks));

#ifdef LC_DEBUG_RECORD_MODE
    // Install the pass-through recorder callbacks
    setRecorderCallbacks(&VideoCallbacks, &AudioCallbacks);
#endif

    // Hook the termination callback so we can avoid issuing a termination callback
    // after LiStopConnection() is called.
    //
    // Initialize ListenerCallbacks before anything that could call Limelog().
    originalTerminationCallback = clCallbacks->connectionTerminated;
    memcpy(&ListenerCallbacks, clCallbacks, sizeof(ListenerCallbacks));
    ListenerCallbacks.connectionTerminated = ClInternalConnectionTerminated;

    NegotiatedVideoFormat = (int)NativeSessionConfiguration.negotiatedVideoFormat;
    SunshineFeatureFlags = NativeSessionConfiguration.hostFeatureFlags;
    ReferenceFrameInvalidationSupported =
        NativeSessionConfiguration.referenceFrameInvalidationSupported != 0;
    memcpy(&StreamConfig, streamConfig, sizeof(StreamConfig));

    Limelog("Native QUIC session negotiation is complete\n");

    alreadyTerminated = false;
    ConnectionInterrupted = false;
    
    // Validate the audio configuration
    if ((ActiveServiceFlags & PLANK_NATIVE_SERVICE_AUDIO) &&
            (MAGIC_BYTE_FROM_AUDIO_CONFIG(StreamConfig.audioConfiguration) != 0xCA ||
             CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT)) {
        Limelog("Invalid audio configuration specified\n");
        err = -1;
        goto Cleanup;
    }

    // Height must not be odd or NVENC will fail to initialize
    if (StreamConfig.height & 0x1) {
        Limelog("Encoder height must not be odd. Rounding %d to %d\n",
                StreamConfig.height,
                StreamConfig.height & ~0x1);
        StreamConfig.height = StreamConfig.height & ~0x1;
    }

    // Dimensions over 8192 aren't supported at all (even on Turing)
    if (StreamConfig.width > 8192 || StreamConfig.height > 8192) {
        Limelog("WARNING: Streaming at resolutions above 8K will likely fail! Trying anyway!\n");
    }

    
    Limelog("Initializing platform...");
    ListenerCallbacks.stageStarting(STAGE_PLATFORM_INIT);
    err = initializePlatform();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_PLATFORM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_PLATFORM_INIT);
    ListenerCallbacks.stageComplete(STAGE_PLATFORM_INIT);
    Limelog("done\n");

    // The owner has already authenticated and connected the native transport.
    // Media setup must not perform a second DNS/socket reachability check.
    Limelog("Initializing audio stream...");
    ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_INIT);
    err = (ActiveServiceFlags & PLANK_NATIVE_SERVICE_AUDIO) ? initializeAudioStream() : 0;
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_AUDIO_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_INIT);
    Limelog("done\n");

    HighQualitySurroundSupported = false;
    HighQualitySurroundEnabled = false;
    NormalQualityOpusConfig = NativeSessionConfiguration.opusConfiguration;
    memset(&HighQualityOpusConfig, 0, sizeof(HighQualityOpusConfig));
    AudioPacketDuration = (int)NativeSessionConfiguration.audioPacketDurationMs;

    Limelog("Starting session negotiation...");
    ListenerCallbacks.stageStarting(STAGE_SESSION_NEGOTIATION);
    stage++;
    LC_ASSERT(stage == STAGE_SESSION_NEGOTIATION);
    ListenerCallbacks.stageComplete(STAGE_SESSION_NEGOTIATION);
    Limelog("done\n");

    Limelog("Initializing control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_INIT);
    err = initializeControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_INIT);
    Limelog("done\n");

    Limelog("Initializing video stream...");
    ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_INIT);
    initializeVideoStream();
    stage++;
    LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_INIT);
    Limelog("done\n");

    Limelog("Initializing input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_INIT);
    if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_INPUT) initializeInputStream();
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_INIT);
    Limelog("done\n");

    Limelog("Starting control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_START);
    err = startControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_START);
    Limelog("done\n");

    Limelog("Starting video stream...");
    ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_START);
    err = startVideoStream(renderContext, drFlags);
    if (err != 0) {
        Limelog("Video stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_VIDEO_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_VIDEO_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_START);
    Limelog("done\n");

    Limelog("Starting audio stream...");
    ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_START);
    err = (ActiveServiceFlags & PLANK_NATIVE_SERVICE_AUDIO) ? startAudioStream(audioContext, arFlags) : 0;
    if (err != 0) {
        Limelog("Audio stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_AUDIO_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_START);
    Limelog("done\n");

    Limelog("Starting input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_START);
    err = (ActiveServiceFlags & PLANK_NATIVE_SERVICE_INPUT) ? startInputStream() : 0;
    if (err != 0) {
        Limelog("Input stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_INPUT_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_START);
    Limelog("done\n");
    
    // Wiggle the mouse a bit to wake the display up
    if (ActiveServiceFlags & PLANK_NATIVE_SERVICE_INPUT) {
        LiSendMouseMoveEvent(1, 1);
        PltSleepMs(10);
        LiSendMouseMoveEvent(-1, -1);
        PltSleepMs(10);
    }

    ListenerCallbacks.connectionStarted();

Cleanup:
    NativeSessionConfigurationPending = false;
    memset(&NativeSessionConfiguration, 0, sizeof(NativeSessionConfiguration));
    if (err != 0) {
        // Undo any work we've done here before failing
        LiStopConnection();
    }
    return err;
}
