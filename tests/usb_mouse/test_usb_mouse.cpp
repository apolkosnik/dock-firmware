// Host-side unit test for the mouse support (src/mouse/mouse.cpp): boot and
// report-protocol report decoding, report descriptor parsing (8, 12 and 16-bit
// deltas), the button mapping, the packet accumulator (sums across packets that
// could not be sent, saturation, send pacing) and two mice side by side.
//
// Expected values are written from the handheld contract in include/mouse/mouse.h
// and rust/src/gamepad.rs, not derived from the firmware code.
// Run with: make -C tests/usb_mouse

#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mouse/mouse.h"
#include "rust_api.h"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            if (failures <= 40) {                                     \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
                printf(__VA_ARGS__);                                  \
                printf("\n");                                         \
            }                                                         \
        }                                                             \
    } while (0)

// Contract constants, spelled out independently of mouse.h.
constexpr uint32_t kMarker = 0x40000000u;
constexpr uint32_t kLeft = 0x01;
constexpr uint32_t kRight = 0x02;
constexpr uint32_t kMiddle = 0x04;
constexpr uint32_t kBack = 0x08;
constexpr uint32_t kForward = 0x10;
constexpr uint32_t kKind = 0x101;

// HID 1.11 appendix E.10: boot mouse, 3 buttons, 8-bit X/Y, no report ID.
const uint8_t kBootMouseDesc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Constant)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

// Report-protocol mouse with report IDs: 5 buttons, 16-bit X/Y, 8-bit wheel,
// 8-bit AC Pan, plus a consumer control collection on another report ID.
const uint8_t kMouse16Desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x05,        //     Report Count (5)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x75, 0x03,        //     Report Size (3)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Constant, Variable)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0x80,  //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0,              // End Collection
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection
};

// 12-bit X/Y (as in many Microsoft mice), 3 buttons, wheel, report ID 26.
const uint8_t kMouse12Desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x1A,        //   Report ID (26)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x75, 0x05,        //     Report Size (5)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Constant, Variable)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0xF8,  //     Logical Minimum (-2047)
    0x26, 0xFF, 0x07,  //     Logical Maximum (2047)
    0x75, 0x0C,        //     Report Size (12)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

// Mouse using Push/Pop, a 4-byte extended usage and a long item.
const uint8_t kMousePushPopDesc[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x02,                    // Usage (Mouse)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x01,                    //   Report ID (1)
    0xA4,                          //   Push
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01,                    //   Usage Minimum (1)
    0x29, 0x05,                    //   Usage Maximum (5)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x05,                    //   Report Count (5)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute)
    0x75, 0x03,                    //   Report Size (3)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x01,                    //   Input (Constant)
    0xB4,                          //   Pop (usage page back to Generic Desktop)
    0x0B, 0x30, 0x00, 0x01, 0x00,  //   Usage (extended: page 1, X)
    0x09, 0x31,                    //   Usage (Y)
    0x15, 0x81,                    //   Logical Minimum (-127)
    0x25, 0x7F,                    //   Logical Maximum (127)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x02,                    //   Report Count (2)
    0x81, 0x06,                    //   Input (Data, Variable, Relative)
    0xFE, 0x02, 0x00, 0xAA, 0xBB,  //   Long item (skipped)
    0x09, 0x38,                    //   Usage (Wheel)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x06,                    //   Input (Data, Variable, Relative)
    0xC0,                          // End Collection
};

// HID 1.11 appendix E.6: boot keyboard.
const uint8_t kKeyboardDesc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

// A pointing device with absolute axes (Pointer application collection, X/Y
// 0..32767, Input Absolute): its positions are not movements, so it is not a
// mouse (and not a gamepad either).
const uint8_t kAbsolutePointerDesc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x01,        // Usage (Pointer)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x03,        //   Usage Maximum (3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x03,        //   Report Count (3)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x75, 0x05,        //   Report Size (5)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Constant)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection
};

// A gamepad with X/Y axes and 8 buttons in a Gamepad application collection.
const uint8_t kGamepadDesc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0, 0xC0,
};

void CheckField(const mouse::Field& f, bool present, unsigned offset, unsigned size, bool is_signed, const char* name)
{
    CHECK(f.present == present, "%s present %d != %d", name, f.present, present);
    if (present && f.present) {
        CHECK(f.bit_offset == offset, "%s offset %u != %u", name, f.bit_offset, offset);
        CHECK(f.bit_size == size, "%s size %u != %u", name, f.bit_size, size);
        CHECK(f.is_signed == is_signed, "%s signed %d != %d", name, f.is_signed, is_signed);
    }
}

void CheckReport(const mouse::Report& r, uint32_t buttons, int32_t dx, int32_t dy, int32_t wheel, int line)
{
    CHECK(r.buttons == buttons, "line %d buttons %08" PRIx32 " != %08" PRIx32, line, r.buttons, buttons);
    CHECK(r.dx == dx, "line %d dx %" PRId32 " != %" PRId32, line, r.dx, dx);
    CHECK(r.dy == dy, "line %d dy %" PRId32 " != %" PRId32, line, r.dy, dy);
    CHECK(r.wheel == wheel, "line %d wheel %" PRId32 " != %" PRId32, line, r.wheel, wheel);
}

void CheckData(const GamepadData& g, uint32_t buttons, int lx, int ly, int wheel, int line)
{
    CHECK(g.buttons == (kMarker | buttons), "line %d buttons %08" PRIx32 " != %08" PRIx32, line, g.buttons,
          kMarker | buttons);
    CHECK((g.buttons & 0x80000000u) == 0, "line %d light gun bit set", line);
    CHECK(g.lx == lx, "line %d lx %d != %d", line, g.lx, lx);
    CHECK(g.ly == ly, "line %d ly %d != %d", line, g.ly, ly);
    CHECK(static_cast<int16_t>(g.lz) == wheel, "line %d lz %d != %d", line, static_cast<int16_t>(g.lz), wheel);
    CHECK(g.rx == 0 && g.ry == 0 && g.rz == 0, "line %d rx/ry/rz not zero", line);
}

void TestConstants()
{
    static_assert(sizeof(GamepadData) == 16, "GamepadData size");
    static_assert(offsetof(GamepadData, buttons) == 0, "buttons offset");
    static_assert(offsetof(GamepadData, lx) == 4, "lx offset");
    static_assert(offsetof(GamepadData, ly) == 6, "ly offset");
    static_assert(offsetof(GamepadData, lz) == 8, "lz offset");
    static_assert(offsetof(GamepadData, rx) == 10, "rx offset");
    static_assert(offsetof(GamepadData, ry) == 12, "ry offset");
    static_assert(offsetof(GamepadData, rz) == 14, "rz offset");

    CHECK(mouse::kMouseMarker == kMarker, "marker");
    CHECK(mouse::kButtonLeft == kLeft && mouse::kButtonRight == kRight && mouse::kButtonMiddle == kMiddle &&
              mouse::kButtonBack == kBack && mouse::kButtonForward == kForward,
          "button bits");
    CHECK(mouse::kGamepadKindMouse == kKind, "kind");
}

void TestBootReports()
{
    mouse::Report r;
    const uint8_t a[] = {0x01, 0x05, 0xFB};
    CHECK(mouse::ParseBootReport(a, sizeof(a), &r), "3-byte boot report");
    CheckReport(r, kLeft, 5, -5, 0, __LINE__);

    const uint8_t b[] = {0x1F, 0x80, 0x7F, 0xFF};
    CHECK(mouse::ParseBootReport(b, sizeof(b), &r), "4-byte boot report");
    CheckReport(r, kLeft | kRight | kMiddle | kBack | kForward, -128, 127, -1, __LINE__);

    const uint8_t c[] = {0xE4, 0x00, 0x00, 0x02, 0x55, 0x66, 0x77, 0x88};
    CHECK(mouse::ParseBootReport(c, sizeof(c), &r), "8-byte boot report");
    CheckReport(r, kMiddle, 0, 0, 2, __LINE__);

    const uint8_t d[] = {0x01, 0x05};
    CHECK(!mouse::ParseBootReport(d, sizeof(d), &r), "2-byte boot report accepted");

    // Each button bit maps to its own contract bit.
    const struct {
        uint8_t raw;
        uint32_t expect;
    } table[] = {{0x01, kLeft}, {0x02, kRight}, {0x04, kMiddle}, {0x08, kBack}, {0x10, kForward}};
    for (const auto& t : table) {
        const uint8_t rep[] = {t.raw, 0, 0};
        CHECK(mouse::ParseBootReport(rep, 3, &r), "button report");
        CHECK(r.buttons == t.expect, "button raw %02x -> %08" PRIx32 " != %08" PRIx32, t.raw, r.buttons, t.expect);
    }
}

void TestBootMouseDescriptor()
{
    mouse::ReportLayout l;
    CHECK(mouse::ParseReportDescriptor(kBootMouseDesc, sizeof(kBootMouseDesc), &l), "boot mouse descriptor");
    CHECK(!l.has_report_id, "boot mouse report id");
    CheckField(l.buttons[0], true, 0, 1, false, "button1");
    CheckField(l.buttons[1], true, 1, 1, false, "button2");
    CheckField(l.buttons[2], true, 2, 1, false, "button3");
    CheckField(l.buttons[3], false, 0, 0, false, "button4");
    CheckField(l.buttons[4], false, 0, 0, false, "button5");
    CheckField(l.x, true, 8, 8, true, "x");
    CheckField(l.y, true, 16, 8, true, "y");
    CheckField(l.wheel, false, 0, 0, false, "wheel");

    mouse::Report r;
    const uint8_t rep[] = {0x05, 0x10, 0xF0};
    CHECK(mouse::ParseReport(l, rep, sizeof(rep), &r), "boot mouse report");
    CheckReport(r, kLeft | kMiddle, 16, -16, 0, __LINE__);
    CHECK(!mouse::ParseReport(l, rep, 2, &r), "short report accepted");
}

void TestMouse16Descriptor()
{
    mouse::ReportLayout l;
    CHECK(mouse::ParseReportDescriptor(kMouse16Desc, sizeof(kMouse16Desc), &l), "16-bit mouse descriptor");
    CHECK(l.has_report_id && l.report_id == 2, "16-bit report id %d/%u", l.has_report_id, l.report_id);
    for (unsigned i = 0; i < 5; i++) {
        CheckField(l.buttons[i], true, i, 1, false, "button");
    }
    CheckField(l.x, true, 8, 16, true, "x");
    CheckField(l.y, true, 24, 16, true, "y");
    CheckField(l.wheel, true, 40, 8, true, "wheel");

    mouse::Report r;
    const uint8_t rep[] = {0x02, 0x05, 0x34, 0x12, 0xCC, 0xED, 0xFF, 0x00};
    CHECK(mouse::ParseReport(l, rep, sizeof(rep), &r), "16-bit report");
    CheckReport(r, kLeft | kMiddle, 0x1234, -0x1234, -1, __LINE__);

    const uint8_t extremes[] = {0x02, 0x1A, 0x00, 0x80, 0xFF, 0x7F, 0x80, 0x00};
    CHECK(mouse::ParseReport(l, extremes, sizeof(extremes), &r), "16-bit extremes");
    CheckReport(r, kRight | kBack | kForward, -32768, 32767, -128, __LINE__);

    // Reports of the consumer control collection are ignored.
    const uint8_t other[] = {0x03, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(!mouse::ParseReport(l, other, sizeof(other), &r), "other report id accepted");
    // Missing wheel byte: rejected rather than misread.
    CHECK(!mouse::ParseReport(l, rep, 6, &r), "truncated 16-bit report accepted");
    // AC Pan byte missing is fine.
    CHECK(mouse::ParseReport(l, rep, 7, &r), "report without AC pan rejected");
    CheckReport(r, kLeft | kMiddle, 0x1234, -0x1234, -1, __LINE__);
}

void TestMouse12Descriptor()
{
    mouse::ReportLayout l;
    CHECK(mouse::ParseReportDescriptor(kMouse12Desc, sizeof(kMouse12Desc), &l), "12-bit mouse descriptor");
    CHECK(l.has_report_id && l.report_id == 26, "12-bit report id");
    CheckField(l.x, true, 8, 12, true, "x");
    CheckField(l.y, true, 20, 12, true, "y");
    CheckField(l.wheel, true, 32, 8, true, "wheel");

    mouse::Report r;
    // x = 0xFFF (-1), y = 0x005: bytes LSB first across the byte boundary.
    const uint8_t rep[] = {0x1A, 0x01, 0xFF, 0x5F, 0x00, 0x02};
    CHECK(mouse::ParseReport(l, rep, sizeof(rep), &r), "12-bit report");
    CheckReport(r, kLeft, -1, 5, 2, __LINE__);
    // x = -2047 (0x801), y = 2047 (0x7FF)
    const uint8_t rep2[] = {0x1A, 0x00, 0x01, 0xF8, 0x7F, 0x00};
    CHECK(mouse::ParseReport(l, rep2, sizeof(rep2), &r), "12-bit report 2");
    CheckReport(r, 0, -2047, 2047, 0, __LINE__);
}

void TestPushPopDescriptor()
{
    mouse::ReportLayout l;
    CHECK(mouse::ParseReportDescriptor(kMousePushPopDesc, sizeof(kMousePushPopDesc), &l), "push/pop descriptor");
    CHECK(l.has_report_id && l.report_id == 1, "push/pop report id");
    for (unsigned i = 0; i < 5; i++) {
        CheckField(l.buttons[i], true, i, 1, false, "button");
    }
    CheckField(l.x, true, 8, 8, true, "x");
    CheckField(l.y, true, 16, 8, true, "y");
    CheckField(l.wheel, true, 24, 8, true, "wheel");

    mouse::Report r;
    const uint8_t rep[] = {0x01, 0x12, 0x7F, 0x81, 0xFE};
    CHECK(mouse::ParseReport(l, rep, sizeof(rep), &r), "push/pop report");
    CheckReport(r, kRight | kForward, 127, -127, -2, __LINE__);
}

void TestNonMouseDescriptors()
{
    mouse::ReportLayout l;
    uint32_t usages[8];

    CHECK(!mouse::ParseReportDescriptor(kKeyboardDesc, sizeof(kKeyboardDesc), &l), "keyboard parsed as mouse");
    size_t n = mouse::ApplicationUsages(kKeyboardDesc, sizeof(kKeyboardDesc), usages, 8);
    CHECK(n == 1 && usages[0] == 0x00010006u, "keyboard usages n=%zu", n);

    CHECK(!mouse::ParseReportDescriptor(kGamepadDesc, sizeof(kGamepadDesc), &l), "gamepad parsed as mouse");
    n = mouse::ApplicationUsages(kGamepadDesc, sizeof(kGamepadDesc), usages, 8);
    CHECK(n == 1 && usages[0] == 0x00010005u, "gamepad usages n=%zu", n);

    // Absolute axes are positions, not movements: not a mouse, whether the
    // collection's usage is Pointer or Mouse (a KVM's "absolute mouse").
    CHECK(!mouse::ParseReportDescriptor(kAbsolutePointerDesc, sizeof(kAbsolutePointerDesc), &l),
          "absolute pointer parsed as mouse");
    n = mouse::ApplicationUsages(kAbsolutePointerDesc, sizeof(kAbsolutePointerDesc), usages, 8);
    CHECK(n == 1 && usages[0] == 0x00010001u, "absolute pointer usages n=%zu", n);
    {
        uint8_t absolute_mouse[sizeof(kAbsolutePointerDesc)];
        memcpy(absolute_mouse, kAbsolutePointerDesc, sizeof(absolute_mouse));
        absolute_mouse[3] = 0x02;  // Usage (Mouse)
        CHECK(!mouse::ParseReportDescriptor(absolute_mouse, sizeof(absolute_mouse), &l), "absolute mouse parsed as mouse");
        // The same axes relative: a 16-bit mouse.
        absolute_mouse[sizeof(absolute_mouse) - 2] = 0x06;  // Input (Data, Variable, Relative)
        CHECK(mouse::ParseReportDescriptor(absolute_mouse, sizeof(absolute_mouse), &l), "relative mouse not parsed");
        CheckField(l.x, true, 8, 16, false, "relative mouse x");
        CheckField(l.y, true, 24, 16, false, "relative mouse y");
    }

    n = mouse::ApplicationUsages(kMouse16Desc, sizeof(kMouse16Desc), usages, 8);
    CHECK(n == 2 && usages[0] == 0x00010002u && usages[1] == 0x000C0001u, "16-bit mouse usages n=%zu", n);

    // Truncated and malformed descriptors are rejected without reading past the end.
    for (size_t len = 0; len < sizeof(kMouse16Desc); len++) {
        uint8_t copy[sizeof(kMouse16Desc)];
        memcpy(copy, kMouse16Desc, len);
        bool ok = mouse::ParseReportDescriptor(copy, len, &l);
        // The mouse application collection closes with the byte at offset 74;
        // shorter prefixes cannot succeed.
        CHECK(ok == (len >= 75), "truncated descriptor len=%zu ok=%d", len, ok);
    }
    const uint8_t pop_without_push[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0xB4, 0xC0};
    CHECK(!mouse::ParseReportDescriptor(pop_without_push, sizeof(pop_without_push), &l), "pop without push");
    CHECK(!mouse::ParseReportDescriptor(nullptr, 0, &l), "empty descriptor");
    CHECK(mouse::ApplicationUsages(nullptr, 0, usages, 8) == 0, "empty descriptor usages");
}

// Serialise the way rust/src/handheld.rs does (slot LE, then GamepadData) and
// check the wire bytes the handheld sees.
void CheckWire(const GamepadData& g, int line)
{
    uint8_t buf[20];
    uint32_t id = 9;
    memcpy(buf, &id, 4);
    memcpy(buf + 4, &g, sizeof(GamepadData));
    CHECK((buf[7] & 0x40) != 0, "line %d wire bit30 missing", line);
    CHECK((buf[7] & 0x80) == 0, "line %d wire bit31 set", line);
    CHECK((buf[4] & 0x1F) == (g.buttons & 0x1F), "line %d wire buttons", line);
    CHECK(static_cast<int16_t>(buf[8] | (buf[9] << 8)) == g.lx, "line %d wire lx", line);
    CHECK(static_cast<int16_t>(buf[10] | (buf[11] << 8)) == g.ly, "line %d wire ly", line);
    CHECK(static_cast<uint16_t>(buf[12] | (buf[13] << 8)) == g.lz, "line %d wire lz", line);
    for (int i = 14; i < 20; i++) {
        CHECK(buf[i] == 0, "line %d wire byte %d not zero", line, i);
    }
}

void TestAccumulator()
{
    mouse::Accumulator acc;
    // The first packet is due immediately and only carries the marker.
    CHECK(acc.Due(0, 4000), "initial packet not due");
    GamepadData g = acc.Take(0);
    CheckData(g, 0, 0, 0, 0, __LINE__);
    CheckWire(g, __LINE__);
    CHECK(!acc.Due(100, 4000), "due after initial packet");

    // Movement sums across reports; buttons are the latest state.
    acc.Add({kLeft, 10, -3, 1});
    acc.Add({kLeft, 7, 4, 0});
    CHECK(acc.HasMovement(), "no movement");
    CHECK(acc.Due(100, 4000), "button change not due");  // left was pressed
    g = acc.Take(100);
    CheckData(g, kLeft, 17, 1, 1, __LINE__);
    CheckWire(g, __LINE__);
    CHECK(!acc.HasMovement() && !acc.Due(100, 4000), "movement not cleared by Take");

    // Button mapping, one bit at a time, and only the five contract bits.
    const uint32_t bits[] = {kLeft, kRight, kMiddle, kBack, kForward};
    for (uint32_t b : bits) {
        acc.Add({b, 0, 0, 0});
        g = acc.Take(200);
        CheckData(g, b, 0, 0, 0, __LINE__);
    }
    acc.Add({0xFFFFFFFFu, 0, 0, 0});
    g = acc.Take(300);
    CheckData(g, kLeft | kRight | kMiddle | kBack | kForward, 0, 0, 0, __LINE__);
    acc.Add({0, 0, 0, 0});
    g = acc.Take(400);
    CheckData(g, 0, 0, 0, 0, __LINE__);

    // Saturation at the i16 range, wheel included.
    for (int i = 0; i < 3; i++) {
        acc.Add({0, 30000, -30000, 30000});
    }
    g = acc.Take(10000);
    CheckData(g, 0, 32767, -32768, 32767, __LINE__);
    for (int i = 0; i < 3; i++) {
        acc.Add({0, -30000, 30000, -30000});
    }
    g = acc.Take(20000);
    CheckData(g, 0, -32768, 32767, -32768, __LINE__);
    CHECK(g.lz == 0x8000, "wheel two's complement %04x", g.lz);
    acc.Add({0, 0, 0, -1});
    g = acc.Take(30000);
    CHECK(g.lz == 0xFFFF, "wheel -1 %04x", g.lz);
    acc.Add({0, 0, 0, 200});
    g = acc.Take(31000);
    CHECK(g.lz == 200, "wheel 200 %u", g.lz);
}

void TestRestore()
{
    mouse::Accumulator acc;
    acc.Take(0);
    acc.Add({kRight, 5, 6, 7});
    GamepadData g = acc.Take(1000);
    CheckData(g, kRight, 5, 6, 7, __LINE__);
    CHECK(!acc.Due(1000, 4000), "due after take");

    // The packet could not be posted: it goes back and is due again at once.
    acc.Restore(g);
    CHECK(acc.Due(1000, 4000), "restored packet not due");
    acc.Add({kRight, 1, 1, 1});
    g = acc.Take(1001);
    CheckData(g, kRight, 6, 7, 8, __LINE__);

    // Restoring on top of movement that arrived meanwhile adds up, saturating.
    acc.Add({kRight, 32000, 0, 0});
    acc.Restore(g);
    g = acc.Take(2000);
    CheckData(g, kRight, 32006, 7, 8, __LINE__);
    acc.Add({kRight, 32000, 0, 0});
    acc.Restore(g);
    g = acc.Take(3000);
    CheckData(g, kRight, 32767, 7, 8, __LINE__);
}

void TestPacing()
{
    constexpr uint32_t kInterval = 4000;
    mouse::Accumulator acc;
    acc.Take(0);

    // Movement-only reports every millisecond: not sent before the interval.
    acc.Add({0, 1, 0, 0});
    CHECK(!acc.Due(1000, kInterval), "sent before interval");
    acc.Add({0, 1, 0, 0});
    CHECK(!acc.Due(2000, kInterval), "sent before interval");
    acc.Add({0, 1, 0, 0});
    CHECK(!acc.Due(3999, kInterval), "sent before interval");
    CHECK(acc.Due(4000, kInterval), "not sent at interval");
    GamepadData g = acc.Take(4000);
    CheckData(g, 0, 3, 0, 0, __LINE__);

    // A button change goes out immediately.
    acc.Add({kLeft, 1, 0, 0});
    CHECK(acc.Due(4100, kInterval), "button press delayed");
    g = acc.Take(4100);
    CheckData(g, kLeft, 1, 0, 0, __LINE__);
    acc.Add({0, 0, 0, 0});
    CHECK(acc.Due(4200, kInterval), "button release delayed");
    g = acc.Take(4200);
    CheckData(g, 0, 0, 0, 0, __LINE__);

    // Movement left over after the last report is flushed by a later poll.
    acc.Add({0, 0, 2, 0});
    CHECK(!acc.Due(5000, kInterval), "sent before interval");
    CHECK(acc.Due(8200, kInterval), "remainder not flushed");
    g = acc.Take(8200);
    CheckData(g, 0, 0, 2, 0, __LINE__);
    CHECK(!acc.Due(100000, kInterval), "due with nothing pending");

    // Timer wrap-around.
    acc.Add({0, 1, 0, 0});
    CHECK(!acc.Due(8200 + 1000, kInterval), "sent before interval");
    mouse::Accumulator late;
    late.Take(0xFFFFF000u);
    late.Add({0, 1, 0, 0});
    CHECK(!late.Due(0xFFFFF800u, kInterval), "sent before interval near wrap");
    CHECK(late.Due(0x00000100u, kInterval), "not sent after wrap");
}

// Two mice: one accumulator per mounted interface, each with its own id.
void TestTwoMice()
{
    struct Mouse {
        uint32_t id;
        mouse::Accumulator acc;
    } mice[2] = {{1, {}}, {2, {}}};

    for (auto& m : mice) {
        GamepadData g = m.acc.Take(0);
        CheckData(g, 0, 0, 0, 0, __LINE__);
    }
    // Interleaved reports.
    mice[0].acc.Add({kLeft, 10, 0, 0});
    mice[1].acc.Add({0, -10, 1, 0});
    mice[0].acc.Add({kLeft, 5, 0, 0});
    mice[1].acc.Add({kRight, -5, 1, -1});
    mice[0].acc.Add({0, 0, 0, 0});
    GamepadData a = mice[0].acc.Take(5000);
    GamepadData b = mice[1].acc.Take(5000);
    CheckData(a, 0, 15, 0, 0, __LINE__);
    CheckData(b, kRight, -15, 2, -1, __LINE__);
    CHECK(mice[0].id != mice[1].id, "ids");

    // One mouse's packet being put back does not touch the other.
    mice[1].acc.Restore(b);
    CHECK(!mice[0].acc.Due(5000, 4000), "mouse 1 due");
    CHECK(mice[1].acc.Due(5000, 4000), "mouse 2 not due");
    b = mice[1].acc.Take(5001);
    CheckData(b, kRight, -15, 2, -1, __LINE__);
}

}  // namespace

int main()
{
    TestConstants();
    TestBootReports();
    TestBootMouseDescriptor();
    TestMouse16Descriptor();
    TestMouse12Descriptor();
    TestPushPopDescriptor();
    TestNonMouseDescriptors();
    TestAccumulator();
    TestRestore();
    TestPacing();
    TestTwoMice();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
