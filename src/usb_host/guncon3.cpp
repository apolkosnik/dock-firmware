// TinyUSB host class driver for the Namco GunCon 3 (PS3 light gun, USB 0B9A:0800).
//
// The gun exposes one vendor-specific interface (class 0xFF/0x00/0x00) with an
// interrupt OUT endpoint (8-byte key) and an interrupt IN endpoint (15-byte
// encrypted report). After SET_CONFIGURATION the host sends the key, then polls
// the IN endpoint. Report decryption lives in guncon3_protocol.cpp (ported from
// beardypig's GPL-2.0-or-later Linux driver).
//
// The gun contains a 1-port hub (0C12:8801), so it sits one hub level below the
// dock's own hub; tusb_config.h must allow at least two hubs.

#include "usb_host/guncon3.h"

#include <array>
#include <cstring>

#include "log/log.h"
#include "rust_api.h"
#include "tusb.h"
#include "usb_host/guncon3_protocol.h"

namespace {
constexpr size_t kMaxGuns = CFG_TUH_GUNCON3;

/// Re-send the key after this many consecutive checksum failures.
constexpr uint32_t kMaxChecksumFailures = 16;

/// Largest IN transfer we request (the report is 15 bytes).
constexpr uint16_t kInBufLen = 64;

struct Guncon3 {
    bool used;
    bool mounted;
    bool polling;
    bool key_busy;

    uint8_t dev_addr;
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t ep_in_size;

    /// Gamepad slot ID
    uint32_t gamepad_id;

    uint32_t checksum_failures;

    bool have_last;
    GamepadData last;
};

struct Guncon3Buffers {
    TUH_EPBUF_DEF(in, kInBufLen);
    TUH_EPBUF_DEF(out, guncon3::kKeyLen);
};

std::array<Guncon3, kMaxGuns> guns;
CFG_TUH_MEM_SECTION Guncon3Buffers gun_bufs[kMaxGuns];

Guncon3* FindByAddress(uint8_t dev_addr)
{
    for (auto& gun : guns) {
        if (gun.used && gun.dev_addr == dev_addr) {
            return &gun;
        }
    }
    return nullptr;
}

size_t IndexOf(const Guncon3* gun)
{
    return static_cast<size_t>(gun - guns.data());
}

bool SendKey(Guncon3* gun)
{
    if (gun->key_busy) {
        return false;
    }
    uint8_t* buf = gun_bufs[IndexOf(gun)].out;
    memcpy(buf, guncon3::kKey, guncon3::kKeyLen);
    gun->key_busy = true;
    if (!usbh_edpt_xfer(gun->dev_addr, gun->ep_out, buf, guncon3::kKeyLen)) {
        gun->key_busy = false;
        log_warn("GunCon3: failed to queue key (dev_addr=%u)", gun->dev_addr);
        return false;
    }
    return true;
}

bool QueueReport(Guncon3* gun)
{
    uint8_t* buf = gun_bufs[IndexOf(gun)].in;
    if (!usbh_edpt_xfer(gun->dev_addr, gun->ep_in, buf, gun->ep_in_size)) {
        gun->polling = false;
        log_warn("GunCon3: failed to queue IN transfer (dev_addr=%u)", gun->dev_addr);
        return false;
    }
    gun->polling = true;
    return true;
}

void HandleReport(Guncon3* gun, const uint8_t* report, uint32_t len)
{
    if (len < guncon3::kReportLen) {
        return;
    }

    uint8_t decoded[guncon3::kDecodedLen];
    if (!guncon3::DecryptReport(report, guncon3::kKey, decoded)) {
        if (++gun->checksum_failures >= kMaxChecksumFailures) {
            log_warn("GunCon3: %u checksum failures, re-sending key", (unsigned)gun->checksum_failures);
            gun->checksum_failures = 0;
            SendKey(gun);
        }
        return;
    }
    gun->checksum_failures = 0;

    GamepadData data = guncon3::MapReport(decoded);
    if (gun->have_last && memcmp(&data, &gun->last, sizeof(data)) == 0) {
        return;
    }
    gun->last = data;
    gun->have_last = true;

    if (gun->mounted) {
        rust_event_gamepad_data(gun->gamepad_id, data);
    }
}

bool Guncon3Init(void)
{
    for (auto& gun : guns) {
        gun = Guncon3 { };
    }
    return true;
}

bool Guncon3Deinit(void)
{
    return Guncon3Init();
}

uint16_t Guncon3Open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_interface_t* desc_itf, uint16_t max_len)
{
    static_cast<void>(rhport);

    uint16_t vid = 0;
    uint16_t pid = 0;
    if (!tuh_vid_pid_get(dev_addr, &vid, &pid) || vid != guncon3::kVendorId || pid != guncon3::kProductId) {
        return 0;
    }
    if (desc_itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || desc_itf->bNumEndpoints < 2) {
        return 0;
    }
    if (FindByAddress(dev_addr) != nullptr) {
        // Only the first matching interface is used.
        return 0;
    }

    Guncon3* gun = nullptr;
    for (auto& g : guns) {
        if (!g.used) {
            gun = &g;
            break;
        }
    }
    if (gun == nullptr) {
        log_warn("No GunCon3 slot available");
        return 0;
    }

    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint16_t ep_in_size = 0;

    // Walk this interface's descriptors (up to the next interface).
    const uint8_t* p_desc = reinterpret_cast<const uint8_t*>(desc_itf);
    const uint8_t* p_end = p_desc + max_len;
    p_desc = tu_desc_next(p_desc);
    while (p_desc < p_end && tu_desc_len(p_desc) > 0 && tu_desc_type(p_desc) != TUSB_DESC_INTERFACE) {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            const auto* desc_ep = reinterpret_cast<const tusb_desc_endpoint_t*>(p_desc);
            if (desc_ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                uint8_t addr = desc_ep->bEndpointAddress;
                if (tu_edpt_dir(addr) == TUSB_DIR_IN && ep_in == 0) {
                    TU_ASSERT(tuh_edpt_open(dev_addr, desc_ep), 0);
                    ep_in = addr;
                    ep_in_size = tu_edpt_packet_size(desc_ep);
                } else if (tu_edpt_dir(addr) == TUSB_DIR_OUT && ep_out == 0) {
                    TU_ASSERT(tuh_edpt_open(dev_addr, desc_ep), 0);
                    ep_out = addr;
                }
            }
        }
        p_desc = tu_desc_next(p_desc);
    }

    if (ep_in == 0 || ep_out == 0) {
        log_warn("GunCon3: interrupt endpoints not found (dev_addr=%u)", dev_addr);
        return 0;
    }

    *gun = Guncon3 { };
    gun->used = true;
    gun->dev_addr = dev_addr;
    gun->itf_num = desc_itf->bInterfaceNumber;
    gun->ep_in = ep_in;
    gun->ep_out = ep_out;
    gun->ep_in_size = (ep_in_size == 0 || ep_in_size > kInBufLen) ? kInBufLen : ep_in_size;

    log_info("GunCon3 opened: dev_addr=%u itf=%u ep_in=%02x(%u) ep_out=%02x", dev_addr, gun->itf_num, ep_in,
             gun->ep_in_size, ep_out);
    return static_cast<uint16_t>(p_desc - reinterpret_cast<const uint8_t*>(desc_itf));
}

bool Guncon3SetConfig(uint8_t dev_addr, uint8_t itf_num)
{
    Guncon3* gun = FindByAddress(dev_addr);
    if (gun == nullptr) {
        usbh_driver_set_config_complete(dev_addr, itf_num);
        return false;
    }

    // Send the key; IN polling starts once the key transfer completes.
    // If the key cannot be queued, poll anyway: checksum failures re-send it.
    if (!SendKey(gun)) {
        QueueReport(gun);
    }

    gun->gamepad_id = rust_gamepad_allocate_id();
    gun->mounted = true;
    log_info("GunCon3 mounted: dev_addr=%u gamepad_id=%u", dev_addr, (unsigned)gun->gamepad_id);

    uint8_t device_id[8];
    memset(device_id, 0, sizeof(device_id));
    rust_event_gamepad_connected(gun->gamepad_id, guncon3::kGamepadKindLightGun, device_id, true);

    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

bool Guncon3XferCb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    Guncon3* gun = FindByAddress(dev_addr);
    if (gun == nullptr) {
        return false;
    }

    if (ep_addr == gun->ep_out) {
        gun->key_busy = false;
        if (result != XFER_RESULT_SUCCESS) {
            log_warn("GunCon3: key transfer failed (result=%d)", (int)result);
        }
        if (!gun->polling) {
            QueueReport(gun);
        }
        return true;
    }

    if (ep_addr == gun->ep_in) {
        gun->polling = false;
        if (result == XFER_RESULT_SUCCESS) {
            HandleReport(gun, gun_bufs[IndexOf(gun)].in, xferred_bytes);
        } else if (result == XFER_RESULT_STALLED) {
            log_warn("GunCon3: IN endpoint stalled, polling stopped (dev_addr=%u)", dev_addr);
            return true;
        }
        QueueReport(gun);
        return true;
    }

    return false;
}

void Guncon3Close(uint8_t dev_addr)
{
    Guncon3* gun = FindByAddress(dev_addr);
    if (gun == nullptr) {
        return;
    }
    log_info("GunCon3 unmounted: dev_addr=%u", dev_addr);
    if (gun->mounted) {
        rust_event_gamepad_disconnected(gun->gamepad_id);
    }
    *gun = Guncon3 { };
}

}  // namespace

const usbh_class_driver_t usbh_guncon3_driver = {
    .name = "GUNCON3",
    .init = Guncon3Init,
    .deinit = Guncon3Deinit,
    .open = Guncon3Open,
    .set_config = Guncon3SetConfig,
    .xfer_cb = Guncon3XferCb,
    .close = Guncon3Close,
};
