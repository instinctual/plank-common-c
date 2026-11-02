// Actual common-c input worker, with an in-memory native sender only.
// No host, socket, input device, desktop or upstream application version.
#include "Limelight-internal.h"
#include "plank_transport_input.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d failed\n", __LINE__); exit(1); } } while (0)
static struct { uint8_t type; size_t size; uint8_t payload[PLANK_TRANSPORT_INPUT_MAX_PAYLOAD_SIZE]; } records[64];
static atomic_uint count;
static atomic_bool holdSender, senderBlocked;
static int sendNative(void* context, uint8_t type, const uint8_t* data, size_t size) {
    (void)context;
    if (atomic_load(&holdSender)) {
        atomic_store(&senderBlocked, true);
        while (atomic_load(&holdSender)) PltSleepMs(1);
    }
    unsigned index = atomic_load(&count);
    CHECK(index < 64 && size <= sizeof(records[index].payload));
    records[index].type = type; records[index].size = size;
    memcpy(records[index].payload, data, size);
    atomic_store(&count, index + 1);
    return 0;
}
static void waitFor(unsigned total) {
    for (unsigned i = 0; atomic_load(&count) < total && i < 200; ++i) PltSleepMs(5);
    if (atomic_load(&count) != total) fprintf(stderr, "expected %u events, received %u\n", total, atomic_load(&count));
    CHECK(atomic_load(&count) == total);
}
static void sendPen(uint8_t event, uint8_t buttons, float position) {
    CHECK(LiSendPenEvent(event, LI_TOOL_TYPE_PEN, buttons, position, 0.5f,
                        0.75f, 0, 0, 0, 0) == 0);
}
int main(void) {
    CHECK(initializePlatform() == 0);
    SunshineFeatureFlags = LI_FF_PEN_TOUCH_EVENTS | LI_FF_RAW_HID_TABLET;
    LiSetPlankNativeInputSender(sendNative, NULL);
    CHECK(initializeInputStream() == 0 && startInputStream() == 0);
    CHECK(LiSendMousePositionEvent(123, 456, 1920, 1080) == 0); waitFor(1);
    CHECK(records[0].type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE && records[0].size == 8);
    CHECK(plank_transport_input_read_u16(records[0].payload) == 123);
    CHECK(plank_transport_input_read_u16(records[0].payload + 2) == 456);
    CHECK(plank_transport_input_read_u16(records[0].payload + 4) == 1919);
    CHECK(plank_transport_input_read_u16(records[0].payload + 6) == 1079);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0); waitFor(2);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT) == 0); waitFor(3);
    CHECK(records[1].type == PLANK_TRANSPORT_INPUT_MOUSE_BUTTON && records[1].payload[1] == 1);
    CHECK(records[2].type == PLANK_TRANSPORT_INPUT_MOUSE_BUTTON && records[2].payload[1] == 0);
    CHECK(LiSendKeyboardEvent2(0x41, KEY_ACTION_DOWN, MODIFIER_CTRL, 0) == 0); waitFor(4);
    CHECK(LiSendKeyboardEvent2(0x41, KEY_ACTION_UP, 0, 0) == 0); waitFor(5);
    CHECK(records[3].type == PLANK_TRANSPORT_INPUT_KEYBOARD && records[3].payload[2] == 1);
    CHECK(records[3].payload[3] == MODIFIER_CTRL && records[4].payload[2] == 0);
    CHECK(LiSendHighResScrollEvent(30) == 0); waitFor(6);
    CHECK(LiSendHighResHScrollEvent(-30) == 0); waitFor(7);
    CHECK(records[5].type == PLANK_TRANSPORT_INPUT_VERTICAL_SCROLL);
    CHECK(plank_transport_input_read_u16(records[5].payload) == 30);
    CHECK(records[6].type == PLANK_TRANSPORT_INPUT_HORIZONTAL_SCROLL);
    CHECK((int16_t)plank_transport_input_read_u16(records[6].payload) == -30);

    // Hold the real sender while a complete drag queues. Motion may coalesce
    // with adjacent motion, but must never overwrite a position across a
    // press/release boundary. No sleeps between producer events hide the race.
    atomic_store(&holdSender, true);
    CHECK(LiSendKeyboardEvent2(0x41, KEY_ACTION_UP, 0, 0) == 0);
    for (unsigned i = 0; !atomic_load(&senderBlocked) && i < 200; ++i) PltSleepMs(5);
    CHECK(atomic_load(&senderBlocked));
    CHECK(LiSendMousePositionEvent(50, 50, 1920, 1080) == 0);
    CHECK(LiSendMousePositionEvent(100, 100, 1920, 1080) == 0);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0);
    CHECK(LiSendMousePositionEvent(300, 150, 1920, 1080) == 0);
    CHECK(LiSendMousePositionEvent(400, 200, 1920, 1080) == 0);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT) == 0);
    CHECK(LiSendMousePositionEvent(800, 300, 1920, 1080) == 0);
    atomic_store(&holdSender, false);
    waitFor(13);
    CHECK(records[8].type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE);
    CHECK(plank_transport_input_read_u16(records[8].payload) == 100);
    CHECK(plank_transport_input_read_u16(records[8].payload + 2) == 100);
    CHECK(records[9].type == PLANK_TRANSPORT_INPUT_MOUSE_BUTTON && records[9].payload[1] == 1);
    CHECK(records[10].type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE);
    CHECK(plank_transport_input_read_u16(records[10].payload) == 400);
    CHECK(plank_transport_input_read_u16(records[10].payload + 2) == 200);
    CHECK(records[11].type == PLANK_TRANSPORT_INPUT_MOUSE_BUTTON && records[11].payload[1] == 0);
    CHECK(records[12].type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE);
    CHECK(plank_transport_input_read_u16(records[12].payload) == 800);

    // The sender is blocked after the press and key-down have reached the
    // host. A burst of 150 positions must leave room for both releases.
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT) == 0); waitFor(14);
    CHECK(LiSendKeyboardEvent2(0x41, KEY_ACTION_DOWN, 0, 0) == 0); waitFor(15);
    atomic_store(&senderBlocked, false);
    atomic_store(&holdSender, true);
    CHECK(LiSendHighResScrollEvent(15) == 0);
    for (unsigned i = 0; !atomic_load(&senderBlocked) && i < 200; ++i) PltSleepMs(5);
    CHECK(atomic_load(&senderBlocked));
    for (short x = 0; x < 150; ++x) {
        CHECK(LiSendMousePositionEvent(x, 200, 1920, 1080) == 0);
    }
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT) == 0);
    CHECK(LiSendKeyboardEvent2(0x41, KEY_ACTION_UP, 0, 0) == 0);
    atomic_store(&holdSender, false);
    waitFor(19);
    CHECK(records[15].type == PLANK_TRANSPORT_INPUT_VERTICAL_SCROLL);
    CHECK(records[16].type == PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE);
    CHECK(plank_transport_input_read_u16(records[16].payload) == 149);
    CHECK(records[17].type == PLANK_TRANSPORT_INPUT_MOUSE_BUTTON && records[17].payload[1] == 0);
    CHECK(records[18].type == PLANK_TRANSPORT_INPUT_KEYBOARD && records[18].payload[2] == 0);

    // Pen motion coalesces only across equal event/button state. Tip-down,
    // tip-up and barrel-button transitions must remain in order.
    atomic_store(&senderBlocked, false);
    atomic_store(&holdSender, true);
    CHECK(LiSendHighResScrollEvent(15) == 0);
    for (unsigned i = 0; !atomic_load(&senderBlocked) && i < 200; ++i) PltSleepMs(5);
    CHECK(atomic_load(&senderBlocked));
    sendPen(LI_TOUCH_EVENT_HOVER, 0, 0.1f);
    sendPen(LI_TOUCH_EVENT_HOVER, 0, 0.2f);
    sendPen(LI_TOUCH_EVENT_DOWN, 0, 0.3f);
    sendPen(LI_TOUCH_EVENT_MOVE, 0, 0.4f);
    sendPen(LI_TOUCH_EVENT_MOVE, 0, 0.5f);
    sendPen(LI_TOUCH_EVENT_MOVE, LI_PEN_BUTTON_PRIMARY, 0.6f);
    sendPen(LI_TOUCH_EVENT_UP, 0, 0.7f);
    sendPen(LI_TOUCH_EVENT_HOVER, 0, 0.8f);
    atomic_store(&holdSender, false);
    waitFor(26);
    const uint8_t events[] = { LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_DOWN,
        LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_HOVER };
    const float positions[] = { 0.2f, 0.3f, 0.5f, 0.6f, 0.7f, 0.8f };
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(records[20 + i].type == PLANK_TRANSPORT_INPUT_PEN);
        CHECK(records[20 + i].payload[0] == events[i]);
        CHECK(records[20 + i].payload[2] == (i == 3 ? LI_PEN_BUTTON_PRIMARY : 0));
        uint8_t expected[PLANK_TRANSPORT_INPUT_PEN_SIZE];
        plank_transport_input_encode_pen(expected, events[i], LI_TOOL_TYPE_PEN,
            i == 3 ? LI_PEN_BUTTON_PRIMARY : 0, 0, 0, positions[i], 0.5f, 0.75f, 0, 0);
        CHECK(records[20 + i].size == sizeof(expected));
        CHECK(memcmp(records[20 + i].payload, expected, sizeof(expected)) == 0);
    }
    // Tool changes are ordering barriers even when both events are hover.
    atomic_store(&senderBlocked, false);
    atomic_store(&holdSender, true);
    CHECK(LiSendHighResScrollEvent(15) == 0);
    for (unsigned i = 0; !atomic_load(&senderBlocked) && i < 200; ++i) PltSleepMs(5);
    CHECK(atomic_load(&senderBlocked));
    sendPen(LI_TOUCH_EVENT_HOVER, 0, 0.1f);
    CHECK(LiSendPenEvent(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_ERASER, 0,
                        0.2f, 0.5f, 0.5f, 0, 0, 0, 0) == 0);
    sendPen(LI_TOUCH_EVENT_HOVER, 0, 0.3f);
    atomic_store(&holdSender, false);
    waitFor(30);
    CHECK(records[27].payload[1] == LI_TOOL_TYPE_PEN);
    CHECK(records[28].payload[1] == LI_TOOL_TYPE_ERASER);
    CHECK(records[29].payload[1] == LI_TOOL_TYPE_PEN);

    // Long HID payloads must not enter/reuse the small-event allocation pool.
    unsigned char raw[sizeof(PLANK_RAW_HID_WIRE_HEADER) + PLANK_RAW_HID_MAX_PAYLOAD_SIZE];
    for (unsigned i = 0; i < sizeof(raw); ++i) raw[i] = (unsigned char)(i * 29);
    CHECK(LiSendRawHidEvent(raw, sizeof(raw)) == 0); waitFor(31);
    CHECK(records[30].type == PLANK_TRANSPORT_INPUT_RAW_HID_WACOM);
    CHECK(records[30].size == sizeof(raw) && !memcmp(records[30].payload, raw, sizeof(raw)));
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT) == 0); waitFor(32);
    CHECK(LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT) == 0); waitFor(33);
    CHECK(LiSendRawHidEvent(raw, sizeof(PLANK_RAW_HID_WIRE_HEADER)) == 0); waitFor(34);
    CHECK(records[33].size == sizeof(PLANK_RAW_HID_WIRE_HEADER));

    const char text[] = { 'A', (char)0xc3, (char)0xa9, (char)0xf0, (char)0x9f, (char)0x92, (char)0xa1 };
    CHECK(LiSendUtf8TextEvent(text, sizeof(text)) == 0); waitFor(37);
    CHECK(records[34].size == 1 && records[34].payload[0] == 'A');
    CHECK(records[35].size == 2 && !memcmp(records[35].payload, text + 1, 2));
    CHECK(records[36].size == 4 && !memcmp(records[36].payload, text + 3, 4));
    CHECK(LiSendUtf8TextEvent(text + 3, 3) != 0); // Truncated four-byte code point.
    const char surrogate[] = { (char)0xed, (char)0xa0, (char)0x80 };
    const char overlong[] = { (char)0xc0, (char)0x80 };
    CHECK(LiSendUtf8TextEvent(surrogate, sizeof(surrogate)) != 0);
    CHECK(LiSendUtf8TextEvent(overlong, sizeof(overlong)) != 0);
    CHECK(LiSendMouseButtonEvent(0, BUTTON_LEFT) != 0);
    CHECK(LiSendKeyboardEvent2(0x41, 0, 0, 0) != 0);
    CHECK(stopInputStream() == 0); destroyInputStream(); cleanupPlatform();
    puts("native_input_wire=pass absolute_geometry=1 buttons=1 modifiers=1 scrolling=1 queued_drag_order=1 queue_pressure_releases=1 pen_barriers=1 appversion_required=0");
}
