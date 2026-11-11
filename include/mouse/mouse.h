// Mouse report decoding and packet accumulation, shared by the USB HID mouse
// driver (src/usb_host/hid_drivers/mouse.cpp) and the Bluetooth mouse path
// (src/bluetooth/bluetooth.cpp). This file has no hardware dependencies so that
// tests/usb_mouse can exercise it on the host.
//
// Handheld contract (rust/src/gamepad.rs, buttons::MOUSE_* and kind::MOUSE):
//   GamepadData.buttons: bit 30 set on every mouse packet (bit 31 is the light
//                        gun's), bit 0 left, bit 1 right, bit 2 middle,
//                        bit 3 back, bit 4 forward.
//   lx: sum of dx since the previous transmitted packet (i16, right positive,
//       saturating).
//   ly: sum of dy (i16, down positive, saturating).
//   lz: sum of wheel clicks as u16 two's complement (0 when unavailable).
//   rx = ry = rz = 0.

#pragma once

#include <cstddef>
#include <cstdint>

#include "rust_api.h"

namespace mouse {

/// Gamepad::kind reported for mice (rust/src/gamepad.rs kind::MOUSE).
constexpr uint32_t kGamepadKindMouse = 0x101;

/// GamepadData.buttons bits.
constexpr uint32_t kButtonLeft = 1u << 0;
constexpr uint32_t kButtonRight = 1u << 1;
constexpr uint32_t kButtonMiddle = 1u << 2;
constexpr uint32_t kButtonBack = 1u << 3;
constexpr uint32_t kButtonForward = 1u << 4;
constexpr uint32_t kButtonMask = kButtonLeft | kButtonRight | kButtonMiddle | kButtonBack | kButtonForward;
/// Set on every mouse packet.
constexpr uint32_t kMouseMarker = 1u << 30;

/// One decoded mouse report. `buttons` uses the kButton* bits (no marker).
struct Report {
    uint32_t buttons;
    int32_t dx;     ///< right positive
    int32_t dy;     ///< down positive
    int32_t wheel;  ///< away from the user positive
};

/// Parse a boot-protocol report (HID 1.11 appendix B.2): byte 0 buttons
/// (bits 0-4), byte 1 X, byte 2 Y, and the customary byte 3 wheel. Returns false
/// if the report is shorter than 3 bytes.
bool ParseBootReport(const uint8_t* report, size_t len, Report* out);

/// Position of one field in an input report.
struct Field {
    bool present;
    bool is_signed;
    uint8_t bit_size;
    uint16_t bit_offset;  ///< bits from the start of the report data (after the report ID byte, if any)
};

/// Where the mouse fields live in a report-protocol input report.
struct ReportLayout {
    bool has_report_id;
    uint8_t report_id;
    Field buttons[5];  ///< Button page usages 1..5
    Field x;
    Field y;
    Field wheel;
};

/// Parse a HID report descriptor. Returns true and fills `out` when the
/// descriptor contains a Generic Desktop Mouse (or Pointer) application
/// collection with relative X and Y fields.
bool ParseReportDescriptor(const uint8_t* desc, size_t len, ReportLayout* out);

/// List the usages (usage_page << 16 | usage) of the descriptor's top-level
/// application collections. Returns the number found (at most `max`).
size_t ApplicationUsages(const uint8_t* desc, size_t len, uint32_t* out, size_t max);

/// Decode a report-protocol report with `layout`. Returns false if the report is
/// for another report ID or too short.
bool ParseReport(const ReportLayout& layout, const uint8_t* report, size_t len, Report* out);

/// Sums reports into one GamepadData packet. Movement is only cleared by Take();
/// if the packet could not be handed over, Restore() puts it back so that no
/// movement is lost.
class Accumulator {
   public:
    Accumulator();

    /// Add one report: deltas are summed (saturating at the i16 range), the
    /// button state replaces the previous one.
    void Add(const Report& report);

    /// Any movement since the last Take().
    bool HasMovement() const;

    /// Button state differs from the last packet taken, a packet was put back
    /// with Restore(), or nothing has been taken yet (the first packet tells the
    /// handheld that this gamepad is a mouse).
    bool ButtonsChanged() const;

    /// A packet should be taken now: ButtonsChanged(), or movement is pending
    /// and at least `min_interval_us` has passed since the last Take().
    bool Due(uint32_t now_us, uint32_t min_interval_us) const;

    /// Build the packet and clear the accumulated movement.
    GamepadData Take(uint32_t now_us);

    /// Put back a packet from Take() that was not accepted.
    void Restore(const GamepadData& data);

   private:
    int32_t dx_;
    int32_t dy_;
    int32_t wheel_;
    uint32_t buttons_;
    uint32_t sent_buttons_;
    bool dirty_;
    uint32_t last_take_us_;
};

}  // namespace mouse
