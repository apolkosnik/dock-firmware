#include "mouse.h"

#include "log/log.h"
#include "pico/time.h"
#include "tusb.h"

namespace {
/// Packets that only carry movement are posted at most this often (250 Hz).
/// Button changes are posted immediately. Movement that arrives in between is
/// summed and goes out with the next packet (Poll() flushes the remainder once
/// the interval has passed, so nothing waits for the next report).
constexpr uint32_t kMinSendIntervalUs = 4000;
}  // namespace

void MouseHidDriver::Initialize(std::span<const uint8_t> descriptor) {
    uint8_t dev_addr = gamepad_.dev_addr;
    uint8_t instance = gamepad_.instance;
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    uint8_t protocol_mode = tuh_hid_get_protocol(dev_addr, instance);
    bool have_layout = mouse::ParseReportDescriptor(descriptor.data(), descriptor.size(), &layout_);

    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE && protocol_mode == HID_PROTOCOL_BOOT) {
        boot_ = true;
    } else if (have_layout) {
        boot_ = false;
    } else {
        // A boot interface that refused SET_PROTOCOL and has no parseable
        // descriptor: the boot layout is the best guess.
        boot_ = true;
        log_warn("USB mouse: no usable report descriptor, assuming boot report format (dev_addr=%u instance=%u)",
                 dev_addr, instance);
    }

    if (boot_) {
        log_info("USB mouse: boot protocol (dev_addr=%u instance=%u)", dev_addr, instance);
    } else {
        unsigned buttons = 0;
        for (const auto& b : layout_.buttons) {
            buttons += b.present ? 1 : 0;
        }
        log_info("USB mouse: report protocol (dev_addr=%u instance=%u report_id=%d x=%u+%u y=%u+%u wheel=%d buttons=%u)",
                 dev_addr, instance, layout_.has_report_id ? layout_.report_id : -1, layout_.x.bit_offset,
                 layout_.x.bit_size, layout_.y.bit_offset, layout_.y.bit_size,
                 layout_.wheel.present ? layout_.wheel.bit_size : 0, buttons);
    }
    RequestReport();
}

void MouseHidDriver::OnReportReceived(std::span<const uint8_t> report) {
    mouse::Report decoded;
    bool ok = boot_ ? mouse::ParseBootReport(report.data(), report.size(), &decoded)
                    : mouse::ParseReport(layout_, report.data(), report.size(), &decoded);
    if (ok) {
        accumulator_.Add(decoded);
        MaybeSend(time_us_32());
    }
    RequestReport();
}

void MouseHidDriver::Poll(uint32_t now_us) {
    MaybeSend(now_us);
}

void MouseHidDriver::MaybeSend(uint32_t now_us) {
    if (!accumulator_.Due(now_us, kMinSendIntervalUs)) {
        return;
    }
    GamepadData data = accumulator_.Take(now_us);
    if (!ReportData(data)) {
        // Engine queue full: keep the movement for the next attempt.
        accumulator_.Restore(data);
    }
}

uni_controller_type_t MouseHidDriver::GetGamepadType() {
    return k_eControllerType_Unknown;
}

uint32_t MouseHidDriver::GetGamepadKind() {
    return mouse::kGamepadKindMouse;
}
