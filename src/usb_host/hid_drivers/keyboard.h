#pragma once

#include "keyboard/keyboard.h"
#include "usb_host/hid.h"

/// USB HID keyboard, read in boot protocol (InitUsbHost leaves boot
/// interfaces in report protocol, so it is selected here). Every change of
/// the keys held is sent to the handheld as GamepadData with the keyboard
/// marker set (see include/keyboard/keyboard.h for the contract).
class KeyboardHidDriver : public UsbHidDriver {
   public:
    KeyboardHidDriver(UsbHidGamepad& gamepad) : UsbHidDriver(gamepad) {}

    ~KeyboardHidDriver() override {};

    void Initialize(std::span<const uint8_t> descriptor) override;
    void OnReportReceived(std::span<const uint8_t> report) override;
    void Poll(uint32_t now_us) override;

    uni_controller_type_t GetGamepadType() override;
    uint32_t GetGamepadKind() override;

   private:
    void TrySend();

    /// The state as last received, and whether the handheld has it.
    GamepadData state_ { keyboard::kKeyboardMarker, 0, 0, 0, 0, 0, 0 };
    bool sent_ = false;
};
