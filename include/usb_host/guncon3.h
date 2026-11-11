#pragma once

#include "host/usbh.h"
#include "host/usbh_pvt.h"

/// Maximum number of GunCon 3 light guns handled at the same time.
#define CFG_TUH_GUNCON3 2

/// TinyUSB application class driver for the Namco GunCon 3 (USB 0B9A:0800).
extern const usbh_class_driver_t usbh_guncon3_driver;
