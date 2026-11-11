#include "usb_host/hid.h"

#include <array>
#include <optional>

#include "hid_drivers/generic.h"
#include "hid_drivers/mouse.h"
#include "log/log.h"
#include "mouse/mouse.h"
#include "pico/time.h"
#include "rust_api.h"
#include "tusb.h"

namespace {
/// One slot per HID interface TinyUSB can mount; several mice and gamepads can
/// be connected at once, each with its own driver instance and gamepad ID.
constexpr size_t kMaxGamepads = CFG_TUH_HID;

constexpr uint16_t kUsagePageGenericDesktop = 0x01;
constexpr uint16_t kUsagePageConsumer = 0x0C;
constexpr uint16_t kUsagePageVendorFirst = 0xFF00;
constexpr uint16_t kUsagePointer = 0x01;
constexpr uint16_t kUsageMouse = 0x02;
constexpr uint16_t kUsageJoystick = 0x04;
constexpr uint16_t kUsageGamepad = 0x05;
constexpr uint16_t kUsageKeyboard = 0x06;
constexpr uint16_t kUsageKeypad = 0x07;
constexpr uint16_t kUsageMultiAxis = 0x08;
constexpr uint16_t kUsageSystemControl = 0x80;
}  // namespace

static std::array<std::optional<UsbHidGamepad>, kMaxGamepads> gamepads;

/// A HID interface whose report descriptor describes a mouse.
static bool IsHidMouse(std::span<const uint8_t> descriptor) {
    mouse::ReportLayout layout;
    return mouse::ParseReportDescriptor(descriptor.data(), descriptor.size(), &layout);
}

/// Interfaces with a joystick, gamepad or multi-axis controller collection are
/// gamepads. Interfaces that only describe a mouse, keyboard, system/consumer
/// control or vendor-defined collections are not. Anything else (including
/// interfaces whose descriptor could not be read) is treated as a gamepad, as
/// every HID interface was before.
bool IsHidGamepad(std::span<const uint8_t> descriptor) {
    if (descriptor.empty()) {
        return true;
    }
    uint32_t usages[8];
    size_t n = mouse::ApplicationUsages(descriptor.data(), descriptor.size(), usages, sizeof(usages) / sizeof(usages[0]));
    if (n == 0) {
        return true;
    }
    bool all_known_other = true;
    for (size_t i = 0; i < n; i++) {
        uint16_t page = static_cast<uint16_t>(usages[i] >> 16);
        uint16_t usage = static_cast<uint16_t>(usages[i] & 0xFFFF);
        if (page == kUsagePageGenericDesktop &&
            (usage == kUsageJoystick || usage == kUsageGamepad || usage == kUsageMultiAxis)) {
            return true;
        }
        bool known_other = (page == kUsagePageGenericDesktop &&
                            (usage == kUsagePointer || usage == kUsageMouse || usage == kUsageKeyboard ||
                             usage == kUsageKeypad || usage == kUsageSystemControl)) ||
                           page == kUsagePageConsumer || page >= kUsagePageVendorFirst;
        if (!known_other) {
            all_known_other = false;
        }
    }
    return !all_known_other;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    log_info("USB HID device mount dev_addr=%u instance=%u", dev_addr, instance);

    // Find an empty slot.
    std::optional<UsbHidGamepad>* slot = nullptr;
    for (auto& gp : gamepads) {
        if (!gp.has_value()) {
            slot = &gp;
            break;
        }
    }
    if (slot == nullptr) {
        log_warn("No USB HID slot available");
        return;
    }

    // Initialize UsbHidGamepad struct.
    UsbHidGamepad* gamepad = &slot->emplace();
    gamepad->dev_addr = dev_addr;
    gamepad->instance = instance;

    // Find a driver.
    std::span<const uint8_t> descriptor(desc_report, static_cast<size_t>(desc_len));
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    std::unique_ptr<UsbHidDriver> driver = nullptr;

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE || IsHidMouse(descriptor)) {
        driver = std::make_unique<MouseHidDriver>(*gamepad);
    } else if (IsHidGamepad(descriptor)) {
        driver = std::make_unique<GenericHidDriver>(*gamepad);
    } else {
        log_info("Not USB HID gamepad");
        slot->reset();
        return;
    }
    gamepad->gamepad_id = rust_gamepad_allocate_id();
    gamepad->driver = std::move(driver);

    // Initialize driver
    gamepad->driver->Initialize(descriptor);

    // Send connection event.
    uint8_t device_id[8];
    memset(device_id, 0, sizeof(device_id));
    rust_event_gamepad_connected(
        gamepad->gamepad_id,
        gamepad->driver->GetGamepadKind(),
        device_id,
        true
    );
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    log_info("USB HID device unmount dev_addr=%u instance=%u", dev_addr, instance);

    for (auto& gamepad : gamepads) {
        if (!gamepad.has_value()) {
            continue;
        }
        if (!(gamepad->dev_addr == dev_addr && gamepad->instance == instance)) {
            continue;
        }

        rust_event_gamepad_disconnected(gamepad->gamepad_id);

        gamepad.reset();
        break;
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report_ptr, uint16_t len) {
    std::span<const uint8_t> report(report_ptr, static_cast<size_t>(len));
    if (report.empty()) {
        return;
    }

    for (auto& gamepad : gamepads) {
        if (!gamepad.has_value()) {
            continue;
        }
        if (!(gamepad->dev_addr == dev_addr && gamepad->instance == instance)) {
            continue;
        }

        gamepad->driver->OnReportReceived(report);
        break;
    }
}

void UsbHidPoll() {
    uint32_t now_us = time_us_32();
    for (auto& gamepad : gamepads) {
        if (gamepad.has_value() && gamepad->driver != nullptr) {
            gamepad->driver->Poll(now_us);
        }
    }
}

void UsbHidDriver::RequestReport() {
    if (!tuh_hid_receive_report(gamepad_.dev_addr, gamepad_.instance)) {
        log_error("tuh_hid_receive_report failed");
    }
}

bool UsbHidDriver::ReportData(GamepadData data) {
    return rust_event_gamepad_data(
        gamepad_.gamepad_id,
        data
    );
}
