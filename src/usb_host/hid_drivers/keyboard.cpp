#include "keyboard.h"

#include "log/log.h"
#include "tusb.h"

void KeyboardHidDriver::Initialize(std::span<const uint8_t> descriptor) {
    static_cast<void>(descriptor);
    uint8_t dev_addr = gamepad_.dev_addr;
    uint8_t instance = gamepad_.instance;
    if (tuh_hid_get_protocol(dev_addr, instance) != HID_PROTOCOL_BOOT &&
        !tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT)) {
        log_warn("USB keyboard: could not select boot protocol (dev_addr=%u instance=%u)", dev_addr, instance);
    }
    log_info("USB keyboard: boot protocol (dev_addr=%u instance=%u)", dev_addr, instance);
    RequestReport();
}

void KeyboardHidDriver::OnReportReceived(std::span<const uint8_t> report) {
    GamepadData decoded;
    if (keyboard::ParseBootReport(report.data(), report.size(), &decoded)) {
        if (sent_ && !keyboard::Same(decoded, state_)) {
            sent_ = false;
        }
        if (!sent_) {
            // (a state not yet sent is replaced by the newer one only when
            // the engine queue was full: see TrySend)
            state_ = decoded;
            TrySend();
        }
    }
    RequestReport();
}

void KeyboardHidDriver::Poll(uint32_t now_us) {
    static_cast<void>(now_us);
    if (!sent_) {
        TrySend();
    }
}

void KeyboardHidDriver::TrySend() {
    // Engine queue full: the state goes out from Poll().
    sent_ = ReportData(state_);
}

uni_controller_type_t KeyboardHidDriver::GetGamepadType() {
    return k_eControllerType_Unknown;
}

uint32_t KeyboardHidDriver::GetGamepadKind() {
    return keyboard::kGamepadKindKeyboard;
}
