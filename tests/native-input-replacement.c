// Deterministic scheduling fixture for the actual input worker. The ELF linker
// wrapper pauses after an atomic poll; it never changes queue or packet data.
#include "Limelight-internal.h"
#include "plank_transport_input.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed at %d\n", __LINE__); exit(2); } } while (0)
static atomic_bool holdSender, senderBlocked, armPoll, pollBlocked, releasePoll;
static atomic_uint absoluteCount, penCount, totalCount;

static void pauseAfterInspection(void) {
    if (atomic_exchange(&armPoll, false)) {
        atomic_store(&pollBlocked, true);
        while (!atomic_load(&releasePoll)) PltSleepMs(1);
    }
}

int __real_LbqPollQueueElementIf(PLINKED_BLOCKING_QUEUE queue, void **data,
                                bool (*matches)(const void *, const void *),
                                const void *context);
int __wrap_LbqPollQueueElementIf(PLINKED_BLOCKING_QUEUE queue, void **data,
                                bool (*matches)(const void *, const void *),
                                const void *context) {
    int result = __real_LbqPollQueueElementIf(queue, data, matches, context);
    // The next absolute packet is a pen barrier. Atomic inspection returns
    // NO_ELEMENT without borrowing a pointer; replacement is now harmless.
    if (result == LBQ_NO_ELEMENT) pauseAfterInspection();
    return result;
}

static int recordInput(void *context, uint8_t type, const uint8_t *data, size_t size) {
    (void)context; (void)data; (void)size;
    if (atomic_load(&holdSender)) {
        atomic_store(&senderBlocked, true);
        while (atomic_load(&holdSender)) PltSleepMs(1);
    }
    if (type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE) atomic_fetch_add(&absoluteCount, 1);
    if (type == PLANK_TRANSPORT_INPUT_PEN) atomic_fetch_add(&penCount, 1);
    atomic_fetch_add(&totalCount, 1);
    return 0;
}

static void waitFlag(atomic_bool *flag) {
    for (int i = 0; i < 1000 && !atomic_load(flag); ++i) PltSleepMs(1);
    CHECK(atomic_load(flag));
}

static void penHover(float x) {
    CHECK(LiSendPenEvent(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN, 0,
                        x, 0.5f, 0.0f, 0, 0, 0, 0) == 0);
}

int main(void) {
    CHECK(initializePlatform() == 0);
    SunshineFeatureFlags = LI_FF_PEN_TOUCH_EVENTS;
    LiSetPlankNativeInputSender(recordInput, NULL);
    CHECK(initializeInputStream() == 0 && startInputStream() == 0);
    atomic_store(&holdSender, true);
    CHECK(LiSendHighResScrollEvent(15) == 0);
    waitFlag(&senderBlocked);
    penHover(0.1f);
    CHECK(LiSendMousePositionEvent(100, 100, 1920, 1080) == 0);
    atomic_store(&armPoll, true);
    atomic_store(&holdSender, false);
    waitFlag(&pollBlocked);

    // After the sender inspects the absolute head==tail, the producer replaces
    // it and recycles its holder as the following pen packet. A borrowed peek
    // pointer becomes invalid here, whereas the conditional poll is finished.
    CHECK(LiSendMousePositionEvent(0, 0, 1920, 1080) == 0);
    penHover(0.8f);
    atomic_store(&releasePoll, true);
    CHECK(stopInputStream() == 0);
    destroyInputStream();
    cleanupPlatform();

    printf("total=%u absolute=%u pen=%u expected_absolute=1 expected_pen=2\n",
           atomic_load(&totalCount), atomic_load(&absoluteCount), atomic_load(&penCount));
    return atomic_load(&absoluteCount) == 1 && atomic_load(&penCount) == 2 ? 0 : 1;
}
