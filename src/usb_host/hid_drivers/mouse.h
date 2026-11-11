#pragma once

#include "mouse/mouse.h"
#include "usb_host/hid.h"

/// USB HID mouse. Boot-protocol mice are read in the boot report format (TinyUSB
/// puts boot interfaces into boot protocol during enumeration); other mice are
/// decoded with the field layout taken from their report descriptor. Reports are
/// summed and sent to the handheld as GamepadData with the mouse marker set
/// (see include/mouse/mouse.h for the contract).
class MouseHidDriver : public UsbHidDriver {
   public:
    MouseHidDriver(UsbHidGamepad& gamepad) : UsbHidDriver(gamepad) {}

    ~MouseHidDriver() override {};

    void Initialize(std::span<const uint8_t> descriptor) override;
    void OnReportReceived(std::span<const uint8_t> report) override;
    void Poll(uint32_t now_us) override;

    uni_controller_type_t GetGamepadType() override;
    uint32_t GetGamepadKind() override;

   private:
    void MaybeSend(uint32_t now_us);

    bool boot_ = false;
    mouse::ReportLayout layout_ { };
    mouse::Accumulator accumulator_;
};
