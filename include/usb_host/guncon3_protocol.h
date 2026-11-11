#pragma once

// Namco GunCon 3 (PS3 light gun, USB 0B9A:0800) report protocol.
//
// Pure functions only (no TinyUSB / FreeRTOS dependencies), so that they can be
// unit tested on the build host. See src/usb_host/guncon3_protocol.cpp.

#include <cstddef>
#include <cstdint>

#include "rust_api.h"

namespace guncon3 {

constexpr uint16_t kVendorId = 0x0B9A;
constexpr uint16_t kProductId = 0x0800;

/// Length of the key packet sent on the interrupt OUT endpoint.
constexpr size_t kKeyLen = 8;
/// Length of an encrypted report received on the interrupt IN endpoint.
constexpr size_t kReportLen = 15;
/// Number of meaningful bytes in a decrypted report.
constexpr size_t kDecodedLen = 13;

/// Fixed key sent to the gun after SET_CONFIGURATION.
extern const uint8_t kKey[kKeyLen];

/// Gamepad "kind" reported to the Rust side for a GunCon 3 (outside the
/// bluepad32 uni_controller_type_t range). Must match gamepad::kind::LIGHT_GUN.
constexpr uint32_t kGamepadKindLightGun = 0x100;

/// GamepadData.buttons bits for a GunCon 3 (contract with the handheld firmware).
namespace buttons {
constexpr uint32_t kTrigger = 1u << 0;
constexpr uint32_t kA1 = 1u << 1;
constexpr uint32_t kA2 = 1u << 2;
constexpr uint32_t kB1 = 1u << 3;
constexpr uint32_t kC2 = 1u << 9;
constexpr uint32_t kC1 = 1u << 10;
constexpr uint32_t kB2 = 1u << 12;
constexpr uint32_t kStickAClick = 1u << 16;
constexpr uint32_t kStickBClick = 1u << 17;
constexpr uint32_t kOffScreen = 1u << 18;
constexpr uint32_t kOneEmitter = 1u << 19;
/// Set on every GunCon 3 report: marks the data as light gun data.
constexpr uint32_t kLightGun = 1u << 31;
}  // namespace buttons

/// Decrypt a 15-byte report with the given 8-byte key.
///
/// Returns false (and leaves `out` unspecified) if the checksum does not match.
/// On success, writes the 13 decrypted bytes to `out`.
bool DecryptReport(const uint8_t report[kReportLen], const uint8_t key[kKeyLen], uint8_t out[kDecodedLen]);

/// Map a decrypted report to the dock's GamepadData.
GamepadData MapReport(const uint8_t decoded[kDecodedLen]);

}  // namespace guncon3
