#include "Limelight-internal.h"
#include "plank_transport_input.h"
#include <stdatomic.h>
#include <stddef.h>

static atomic_bool initialized;
static atomic_bool inputFailed;
static PlankNativeInputSender nativeInputSender;
static void* nativeInputSenderContext;
static LINKED_BLOCKING_QUEUE packetQueue;
static LINKED_BLOCKING_QUEUE packetHolderFreeList;
static PLT_THREAD inputSendThread;

#define MAX_QUEUED_INPUT_PACKETS 150
#define INLINE_INPUT_SIZE PLANK_TRANSPORT_INPUT_PEN_SIZE
#define LI_WHEEL_DELTA 120
#define MOTION_BATCHING_INTERVAL_MS 1
#define CLAMP(val, min, max) (((val) < (min)) ? (min) : (((val) > (max)) ? (max) : (val)))

// Own the final native payload. Small events reuse a bounded holder pool;
// variable-length text and raw HID own their bytes until the sender accepts them.
typedef struct _PACKET_HOLDER {
    LINKED_BLOCKING_QUEUE_ENTRY entry;
    size_t length;
    uint8_t type;
    uint8_t payload[];
} PACKET_HOLDER, *PPACKET_HOLDER;

int initializeInputStream(void) {
    initialized = false;
    inputFailed = false;
    LbqInitializeLinkedBlockingQueue(&packetQueue, MAX_QUEUED_INPUT_PACKETS);
    LbqInitializeLinkedBlockingQueue(&packetHolderFreeList, MAX_QUEUED_INPUT_PACKETS);
    return 0;
}

void destroyInputStream(void) {
    PLINKED_BLOCKING_QUEUE_ENTRY entry, next;
    entry = LbqDestroyLinkedBlockingQueue(&packetQueue);
    while (entry != NULL) {
        next = entry->flink;
        free(entry->data);
        entry = next;
    }
    entry = LbqDestroyLinkedBlockingQueue(&packetHolderFreeList);
    while (entry != NULL) {
        next = entry->flink;
        free(entry->data);
        entry = next;
    }
}

static void freePacketHolder(PPACKET_HOLDER holder) {
    if (holder->length > INLINE_INPUT_SIZE ||
            LbqOfferQueueItem(&packetHolderFreeList, holder, &holder->entry) != LBQ_SUCCESS) {
        free(holder);
    }
}

static PPACKET_HOLDER allocatePacketHolder(uint8_t type, size_t length) {
    PPACKET_HOLDER holder = NULL;
    if (length == 0 || length > PLANK_TRANSPORT_INPUT_MAX_PAYLOAD_SIZE) return NULL;
    if (length <= INLINE_INPUT_SIZE) {
        int result = LbqPollQueueElement(&packetHolderFreeList, (void**)&holder);
        if (result == LBQ_INTERRUPTED) return NULL;
        LC_ASSERT(result == LBQ_SUCCESS || result == LBQ_NO_ELEMENT);
    }
    if (!holder) {
        size_t capacity = length > INLINE_INPUT_SIZE ? length : INLINE_INPUT_SIZE;
        holder = malloc(offsetof(PACKET_HOLDER, payload) + capacity);
    }
    if (holder) {
        holder->type = type;
        holder->length = length;
    }
    return holder;
}

static bool isAbsoluteMousePosition(const void* data) {
    return ((const PACKET_HOLDER*)data)->type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE;
}

static bool canBatchMousePosition(const void* data, const void* context) {
    (void)context;
    return isAbsoluteMousePosition(data);
}

static bool canBatchPenEvent(const void* data, const void* context) {
    const PACKET_HOLDER* next = data;
    const PACKET_HOLDER* current = context;
    // Event kind, tool and buttons are ordering barriers. In particular, never
    // replace a pen hover with an eraser hover across a tool transition.
    return next->type == PLANK_TRANSPORT_INPUT_PEN &&
           memcmp(next->payload, current->payload, 3) == 0;
}

static void failInput(int error) {
    if (!atomic_exchange(&inputFailed, true)) {
        LbqSignalQueueShutdown(&packetQueue);
        ListenerCallbacks.connectionTerminated(error < 0 ? error : -1);
    }
}

static int offerInputPacket(PPACKET_HOLDER holder) {
    PPACKET_HOLDER replaced = NULL;
    int result = LbqOfferQueueItemTimed(&packetQueue, holder, &holder->entry, 100,
        holder->type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE ? isAbsoluteMousePosition : NULL,
        (void**)&replaced);
    if (result == LBQ_BOUND_EXCEEDED) {
        Limelog("Input queue remained full; stopping rather than losing a transition\n");
        failInput(-1);
    }
    if (result != LBQ_SUCCESS) freePacketHolder(holder);
    if (replaced) freePacketHolder(replaced);
    return result;
}

static bool sendInputPacket(uint8_t type, const uint8_t* payload, size_t length) {
    const uint64_t deadline = PltGetMillis() + 2000;
    int result;
    do {
        result = nativeInputSender(nativeInputSenderContext, type, payload, length);
        if (result != LI_PLANK_INPUT_WOULD_BLOCK) break;
        // WOULD_BLOCK does not transfer ownership. Retain this exact event and
        // its position in the queue until accepted or the session is stopped.
        if (!initialized || inputFailed) return false;
        if (PltGetMillis() >= deadline) break;
        PltSleepMs(1);
    } while (true);
    if (result != 0) {
        Limelog("Native KyProto input send failed: %d\n", result);
        failInput(result);
        return false;
    }
    return true;
}

void LiSetPlankNativeInputSender(PlankNativeInputSender sender, void* context) {
    nativeInputSender = sender;
    nativeInputSenderContext = context;
}

// Reject malformed/truncated UTF-8 before queuing. A code point may not straddle
// native text messages, including overlong encodings, surrogates or > U+10FFFF.
static size_t utf8CodePointLength(const uint8_t* text, size_t remaining) {
    if (!remaining) return 0;
    uint8_t first = text[0];
    size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                    first >= 0xe0 && first <= 0xef ? 3 :
                    first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!length || length > remaining) return 0;
    for (size_t i = 1; i < length; ++i)
        if ((text[i] & 0xc0) != 0x80) return 0;
    if ((first == 0xe0 && text[1] < 0xa0) || (first == 0xed && text[1] >= 0xa0) ||
        (first == 0xf0 && text[1] < 0x90) || (first == 0xf4 && text[1] >= 0x90)) return 0;
    return length;
}

static void inputSendThreadProc(void* context) {
    (void)context;
    uint64_t lastMousePacketTime = 0, lastPenPacketTime = 0;
    while (!PltIsThreadInterrupted(&inputSendThread)) {
        PPACKET_HOLDER holder;
        if (LbqWaitForQueueElement(&packetQueue, (void**)&holder) != LBQ_SUCCESS) return;
        bool mouse = holder->type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE;
        bool pen = holder->type == PLANK_TRANSPORT_INPUT_PEN &&
            (holder->payload[0] == LI_TOUCH_EVENT_HOVER || holder->payload[0] == LI_TOUCH_EVENT_MOVE);
        if (mouse || pen) {
            uint64_t* last = mouse ? &lastMousePacketTime : &lastPenPacketTime;
            uint64_t now = PltGetMillis();
            if (now < *last + MOTION_BATCHING_INTERVAL_MS) {
                PltSleepMs((int)(*last + MOTION_BATCHING_INTERVAL_MS - now));
                now = PltGetMillis();
            }
            for (;;) {
                PPACKET_HOLDER next;
                if (LbqPollQueueElementIf(&packetQueue, (void**)&next,
                        mouse ? canBatchMousePosition : canBatchPenEvent, holder) != LBQ_SUCCESS) break;
                freePacketHolder(holder);
                holder = next;
            }
            *last = now;
        }
        bool sent = true;
        if (holder->type == PLANK_TRANSPORT_INPUT_UTF8_TEXT) {
            for (size_t offset = 0; offset < holder->length;) {
                size_t length = utf8CodePointLength(holder->payload + offset, holder->length - offset);
                LC_ASSERT(length != 0); // Validated by the producer before ownership transfer.
                if (!length || !sendInputPacket(holder->type, holder->payload + offset, length)) {
                    sent = false;
                    break;
                }
                offset += length;
            }
        } else {
            sent = sendInputPacket(holder->type, holder->payload, holder->length);
        }
        freePacketHolder(holder);
        if (!sent) return;
    }
}

int startInputStream(void) {
    if (!nativeInputSender) {
        Limelog("PLANK native input sender is required\n");
        return -1;
    }
    int result = PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread);
    if (result == 0) initialized = true;
    return result;
}

int stopInputStream(void) {
    initialized = false;
    LbqSignalQueueShutdown(&packetHolderFreeList);
    LbqSignalQueueDrain(&packetQueue);
    PltJoinThread(&inputSendThread);
    return 0;
}

int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight) {
    if (!initialized) return -2;
    if (referenceWidth <= 1 || referenceHeight <= 1) return -1;
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE,
                                                PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE_SIZE);
    if (!holder) return -1;
    plank_transport_input_encode_absolute_mouse(holder->payload,
        CLAMP(x, 0, referenceWidth - 1), CLAMP(y, 0, referenceHeight - 1),
        referenceWidth - 1, referenceHeight - 1);
    return offerInputPacket(holder);
}

int LiSendMouseButtonEvent(char action, int button) {
    if (!initialized) return -2;
    if (action != BUTTON_ACTION_PRESS && action != BUTTON_ACTION_RELEASE) return -1;
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_MOUSE_BUTTON,
                                                PLANK_TRANSPORT_INPUT_MOUSE_BUTTON_SIZE);
    if (!holder) return -1;
    holder->payload[0] = (uint8_t)button;
    holder->payload[1] = action == BUTTON_ACTION_PRESS ? PLANK_TRANSPORT_INPUT_ACTION_PRESS : PLANK_TRANSPORT_INPUT_ACTION_RELEASE;
    return offerInputPacket(holder);
}

int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags) {
    if (!initialized) return -2;
    if (keyAction != KEY_ACTION_DOWN && keyAction != KEY_ACTION_UP) return -1;
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_KEYBOARD,
                                                PLANK_TRANSPORT_INPUT_KEYBOARD_SIZE);
    if (!holder) return -1;
    plank_transport_input_write_u16(holder->payload, (uint16_t)keyCode);
    holder->payload[2] = keyAction == KEY_ACTION_DOWN ? PLANK_TRANSPORT_INPUT_ACTION_PRESS : PLANK_TRANSPORT_INPUT_ACTION_RELEASE;
    holder->payload[3] = (uint8_t)modifiers;
    holder->payload[4] = (uint8_t)flags;
    return offerInputPacket(holder);
}

int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers) {
    return LiSendKeyboardEvent2(keyCode, keyAction, modifiers, 0);
}

int LiSendUtf8TextEvent(const char* text, unsigned int length) {
    if (!initialized) return -2;
    if (!text || !length || length > PLANK_TRANSPORT_INPUT_MAX_PAYLOAD_SIZE) return -1;
    for (size_t offset = 0; offset < length;) {
        size_t size = utf8CodePointLength((const uint8_t*)text + offset, length - offset);
        if (!size) return -1;
        offset += size;
    }
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_UTF8_TEXT, length);
    if (!holder) return -1;
    memcpy(holder->payload, text, length);
    return offerInputPacket(holder);
}

int LiSendRawHidEvent(const unsigned char* data, unsigned int length) {
    if (!initialized) return -2;
    if (!(SunshineFeatureFlags & LI_FF_RAW_HID_TABLET) || !nativeInputSender) return -3;
    if (!data || length < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
            length > sizeof(PLANK_RAW_HID_WIRE_HEADER) + PLANK_RAW_HID_MAX_PAYLOAD_SIZE) return -4;
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_RAW_HID_WACOM, length);
    if (!holder) return -1;
    memcpy(holder->payload, data, length);
    return offerInputPacket(holder);
}

static int sendScroll(uint8_t type, short amount) {
    if (!initialized) return -2;
    if (!amount) return 0;
    PPACKET_HOLDER holder = allocatePacketHolder(type, PLANK_TRANSPORT_INPUT_SCROLL_SIZE);
    if (!holder) return -1;
    plank_transport_input_write_u16(holder->payload, (uint16_t)amount);
    return offerInputPacket(holder);
}
int LiSendHighResScrollEvent(short amount) { return sendScroll(PLANK_TRANSPORT_INPUT_VERTICAL_SCROLL, amount); }
int LiSendHighResHScrollEvent(short amount) { return sendScroll(PLANK_TRANSPORT_INPUT_HORIZONTAL_SCROLL, amount); }
int LiSendScrollEvent(signed char clicks) { return LiSendHighResScrollEvent(clicks * LI_WHEEL_DELTA); }
int LiSendHScrollEvent(signed char clicks) { return LiSendHighResHScrollEvent(clicks * LI_WHEEL_DELTA); }

int LiSendPenEvent(uint8_t eventType, uint8_t toolType, uint8_t penButtons,
                   float x, float y, float pressureOrDistance,
                   float contactAreaMajor, float contactAreaMinor,
                   uint16_t rotation, uint8_t tilt) {
    if (!initialized) return -2;
    if (!(SunshineFeatureFlags & LI_FF_PEN_TOUCH_EVENTS)) return LI_ERR_UNSUPPORTED;
    PPACKET_HOLDER holder = allocatePacketHolder(PLANK_TRANSPORT_INPUT_PEN, PLANK_TRANSPORT_INPUT_PEN_SIZE);
    if (!holder) return -1;
    plank_transport_input_encode_pen(holder->payload, eventType, toolType, penButtons, tilt,
        rotation, x, y, pressureOrDistance, contactAreaMajor, contactAreaMinor);
    return offerInputPacket(holder);
}
