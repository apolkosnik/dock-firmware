use core::sync::atomic::{AtomicU32, Ordering};

static NEXT_ID: AtomicU32 = AtomicU32::new(1);

#[allow(unused)]
pub mod buttons {
    pub const A: u32 = 1 << 0;
    pub const B: u32 = 1 << 1;
    pub const X: u32 = 1 << 2;
    pub const Y: u32 = 1 << 3;
    pub const UP: u32 = 1 << 4;
    pub const DOWN: u32 = 1 << 5;
    pub const RIGHT: u32 = 1 << 6;
    pub const LEFT: u32 = 1 << 7;
    pub const SYSTEM: u32 = 1 << 8;
    pub const SELECT: u32 = 1 << 9;
    pub const START: u32 = 1 << 10;
    pub const CAPTURE: u32 = 1 << 11;
    pub const L1: u32 = 1 << 12;
    pub const R1: u32 = 1 << 13;
    pub const L2: u32 = 1 << 14;
    pub const R2: u32 = 1 << 15;
    pub const L3: u32 = 1 << 16;
    pub const R3: u32 = 1 << 17;

    // Light gun (GunCon 3) reports. The data is passed to the handheld unchanged;
    // these bits must never be masked on the way.
    pub const GUN_TRIGGER: u32 = 1 << 0;
    pub const GUN_A1: u32 = 1 << 1;
    pub const GUN_A2: u32 = 1 << 2;
    pub const GUN_B1: u32 = 1 << 3;
    pub const GUN_C2: u32 = 1 << 9;
    pub const GUN_C1: u32 = 1 << 10;
    pub const GUN_B2: u32 = 1 << 12;
    pub const GUN_STICK_A: u32 = 1 << 16;
    pub const GUN_STICK_B: u32 = 1 << 17;
    pub const GUN_OFFSCREEN: u32 = 1 << 18;
    pub const GUN_ONE_EMITTER: u32 = 1 << 19;
    /// Set on every light gun report: lx/ly are aim X/Y (full i16), lz is Z.
    pub const LIGHT_GUN: u32 = 1 << 31;

    // Mouse reports (USB HID mice: src/usb_host/hid_drivers/mouse.cpp, Bluetooth
    // mice: src/bluetooth/bluetooth.cpp). lx/ly are the sums of dx/dy since the
    // previous transmitted packet (i16, right/down positive, saturating), lz is
    // the wheel sum as u16 two's complement, rx/ry/rz are 0.
    pub const MOUSE_LEFT: u32 = 1 << 0;
    pub const MOUSE_RIGHT: u32 = 1 << 1;
    pub const MOUSE_MIDDLE: u32 = 1 << 2;
    pub const MOUSE_BACK: u32 = 1 << 3;
    pub const MOUSE_FORWARD: u32 = 1 << 4;
    /// Set on every mouse report.
    pub const MOUSE: u32 = 1 << 30;
}

/// Values of `Gamepad::kind` that are not bluepad32 `uni_controller_type_t` values.
#[allow(unused)]
pub mod kind {
    /// Namco GunCon 3 light gun (src/usb_host/guncon3.cpp).
    pub const LIGHT_GUN: u32 = 0x100;
    /// USB HID or Bluetooth mouse (include/mouse/mouse.h).
    pub const MOUSE: u32 = 0x101;
}

#[derive(Debug)]
pub struct Gamepad {
    pub id: GamepadId,
    pub kind: u32,
    pub device_id: [u8; 8],
    pub wired: bool,
}

#[repr(C, packed)]
#[derive(Copy, Clone, Debug)]
pub struct GamepadData {
    buttons: u32,
    lx: i16,
    ly: i16,
    lz: u16,
    rx: i16,
    ry: i16,
    rz: u16,
}

impl GamepadData {
    /// Size on the wire (and in memory: the struct is packed).
    pub const SIZE: usize = 16;

    #[allow(unused)]
    pub const fn new(buttons: u32, lx: i16, ly: i16, lz: u16, rx: i16, ry: i16, rz: u16) -> Self {
        GamepadData {
            buttons,
            lx,
            ly,
            lz,
            rx,
            ry,
            rz,
        }
    }

    pub fn from_le_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        let u16_at = |i: usize| u16::from_le_bytes([bytes[i], bytes[i + 1]]);
        Some(GamepadData {
            buttons: u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            lx: u16_at(4) as i16,
            ly: u16_at(6) as i16,
            lz: u16_at(8),
            rx: u16_at(10) as i16,
            ry: u16_at(12) as i16,
            rz: u16_at(14),
        })
    }

    pub fn to_le_bytes(&self) -> [u8; Self::SIZE] {
        let (buttons, lx, ly, lz, rx, ry, rz) =
            (self.buttons, self.lx, self.ly, self.lz, self.rx, self.ry, self.rz);
        let mut out = [0u8; Self::SIZE];
        out[0..4].copy_from_slice(&buttons.to_le_bytes());
        out[4..6].copy_from_slice(&lx.to_le_bytes());
        out[6..8].copy_from_slice(&ly.to_le_bytes());
        out[8..10].copy_from_slice(&lz.to_le_bytes());
        out[10..12].copy_from_slice(&rx.to_le_bytes());
        out[12..14].copy_from_slice(&ry.to_le_bytes());
        out[14..16].copy_from_slice(&rz.to_le_bytes());
        out
    }

    pub fn buttons(&self) -> u32 {
        self.buttons
    }

    /// A mouse packet (buttons::MOUSE set).
    pub fn is_mouse(&self) -> bool {
        self.buttons & buttons::MOUSE != 0
    }

    /// Combine a queued mouse packet (`self`) with a newer one from the same
    /// mouse: the deltas add up (saturating), everything else is the newer
    /// packet's.
    pub fn merge_mouse(&self, newer: &GamepadData) -> GamepadData {
        let (lx, ly, lz) = (self.lx, self.ly, self.lz as i16);
        let (nlx, nly, nlz) = (newer.lx, newer.ly, newer.lz as i16);
        GamepadData {
            buttons: newer.buttons,
            lx: lx.saturating_add(nlx),
            ly: ly.saturating_add(nly),
            lz: lz.saturating_add(nlz) as u16,
            rx: newer.rx,
            ry: newer.ry,
            rz: newer.rz,
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct GamepadId(pub u32);

impl GamepadId {
    pub fn allocate() -> Self {
        Self(NEXT_ID.fetch_add(1, Ordering::SeqCst))
    }

    pub fn as_u32(self) -> u32 {
        self.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_round_trip() {
        let d = GamepadData::new(0x4000_0005, -2, 32767, 0xFFFF, 1, -1, 7);
        let b = d.to_le_bytes();
        assert_eq!(&b[0..4], &[0x05, 0x00, 0x00, 0x40]);
        assert_eq!(&b[4..6], &[0xFE, 0xFF]);
        assert_eq!(&b[6..8], &[0xFF, 0x7F]);
        assert_eq!(&b[8..10], &[0xFF, 0xFF]);
        let back = GamepadData::from_le_bytes(&b).unwrap();
        assert_eq!(back.to_le_bytes(), b);
        assert!(GamepadData::from_le_bytes(&b[..15]).is_none());
    }

    #[test]
    fn mouse_marker() {
        assert!(GamepadData::new(buttons::MOUSE, 0, 0, 0, 0, 0, 0).is_mouse());
        assert!(!GamepadData::new(buttons::LIGHT_GUN | 1, 0, 0, 0, 0, 0, 0).is_mouse());
        assert!(!GamepadData::new(buttons::A, 0, 0, 0, 0, 0, 0).is_mouse());
    }

    #[test]
    fn merge_adds_deltas_and_takes_newer_buttons() {
        let a = GamepadData::new(buttons::MOUSE | buttons::MOUSE_LEFT, 10, -3, 1, 0, 0, 0);
        let b = GamepadData::new(buttons::MOUSE | buttons::MOUSE_RIGHT, -4, -3, (-2i16) as u16, 0, 0, 0);
        let m = a.merge_mouse(&b);
        let (buttons, lx, ly, lz) = (m.buttons, m.lx, m.ly, m.lz);
        assert_eq!(buttons, buttons::MOUSE | buttons::MOUSE_RIGHT);
        assert_eq!(lx, 6);
        assert_eq!(ly, -6);
        assert_eq!(lz as i16, -1);
    }

    #[test]
    fn merge_saturates() {
        let a = GamepadData::new(buttons::MOUSE, 30000, -30000, 30000, 0, 0, 0);
        let b = GamepadData::new(buttons::MOUSE, 30000, -30000, 30000, 0, 0, 0);
        let m = a.merge_mouse(&b);
        let (lx, ly, lz) = (m.lx, m.ly, m.lz);
        assert_eq!(lx, i16::MAX);
        assert_eq!(ly, i16::MIN);
        assert_eq!(lz as i16, i16::MAX);
        let c = GamepadData::new(buttons::MOUSE, 0, 0, (-30000i16) as u16, 0, 0, 0);
        let m = c.merge_mouse(&c);
        assert_eq!(m.lz as i16, i16::MIN);
    }
}
