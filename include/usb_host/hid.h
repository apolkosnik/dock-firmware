#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "controller/uni_controller_type.h"
#include "rust_api.h"

class UsbHidDriver;

class UsbHidGamepad {
   public:
    /// Slot ID
    uint32_t gamepad_id;

    /// USB device address
    uint8_t dev_addr;

    /// USB interface instance
    uint8_t instance;

    /// Driver
    std::unique_ptr<UsbHidDriver> driver = nullptr;
};

class UsbHidDriver {
   public:
    UsbHidDriver(UsbHidGamepad& gamepad) : gamepad_(gamepad) {}

    virtual ~UsbHidDriver() = default;

    virtual void Initialize(std::span<const uint8_t> descriptor) = 0;
    virtual void OnReportReceived(std::span<const uint8_t> report) = 0;

    /// Called from the USB host task loop (about every millisecond).
    virtual void Poll(uint32_t now_us) { static_cast<void>(now_us); }

    virtual uni_controller_type_t GetGamepadType() = 0;

    /// Value passed as `kind` to rust_event_gamepad_connected. Defaults to the
    /// bluepad32 controller type; non-gamepad drivers use rust/src/gamepad.rs kind::*.
    virtual uint32_t GetGamepadKind() { return static_cast<uint32_t>(GetGamepadType()); }

   protected:
    void RequestReport();

    /// Post data to the engine. Returns false if the engine queue is full.
    bool ReportData(GamepadData data);

    UsbHidGamepad& gamepad_;
};

/// Run the drivers' Poll(); call from the USB host task loop.
void UsbHidPoll();
