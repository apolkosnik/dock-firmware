#pragma once

// USB HID keyboards, reported to the handheld as GamepadData.
//
// Handheld contract (rust/src/gamepad.rs, buttons::KEYBOARD and
// kind::KEYBOARD; the handheld firmware's `lightgun::keyboard`):
//   buttons  bit 29 set on every keyboard report (kKeyboardMarker), bits 0-7
//            the modifier keys as in the HID boot report's first byte (left
//            Ctrl, Shift, Alt, GUI, right Ctrl, Shift, Alt, GUI)
//   lx       HID usages of the first and second key held (low byte first)
//   ly       of the third and fourth
//   lz       of the fifth and sixth; 0 where no key is
//   rx, ry, rz  0
// Every report is the keyboard's whole state. The keyboard is read in the
// HID boot protocol (8 bytes: modifiers, reserved, six keys), which every
// keyboard has.

#include <cstddef>
#include <cstdint>

#include "rust_api.h"

namespace keyboard {

/// Gamepad::kind reported for keyboards (rust/src/gamepad.rs kind::KEYBOARD).
constexpr uint32_t kGamepadKindKeyboard = 0x102;

/// Set in GamepadData::buttons on every keyboard report.
constexpr uint32_t kKeyboardMarker = 1u << 29;

constexpr size_t kBootReportSize = 8;

/// Decode a boot protocol report. Returns false for a report that is too
/// short, and for the "too many keys" report (keys 1..3 are error codes, not
/// keys): the state before it still holds.
inline bool ParseBootReport(const uint8_t* report, size_t len, GamepadData* out) {
    if (report == nullptr || out == nullptr || len < kBootReportSize) {
        return false;
    }
    for (size_t i = 2; i < kBootReportSize; i++) {
        if (report[i] >= 1 && report[i] <= 3) {
            return false;
        }
    }
    auto pair = [&](size_t i) { return static_cast<uint16_t>(report[i] | (report[i + 1] << 8)); };
    *out = GamepadData { };
    out->buttons = kKeyboardMarker | report[0];
    out->lx = static_cast<int16_t>(pair(2));
    out->ly = static_cast<int16_t>(pair(4));
    out->lz = pair(6);
    return true;
}

/// Whether two keyboard states are the same.
inline bool Same(const GamepadData& a, const GamepadData& b) {
    return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly && a.lz == b.lz;
}

}  // namespace keyboard
