use alloc::collections::vec_deque::VecDeque;
use freertos_rust::{Duration, Mutex};

use crate::{
    gamepad::{Gamepad, GamepadData, GamepadId},
    led::{self, LedSession},
    sys, usb_host,
    util::InitCell,
};

const REQ_GET_INFO: u8 = 0;
const REQ_DOCK_BEGIN: u8 = 3;
const REQ_GAMEPAD_CONNECT: u8 = 4;
const REQ_GAMEPAD_DISCONNECT: u8 = 5;
const REQ_GAMEPAD_DATA: u8 = 6;

pub(crate) static TX_PACKET_QUEUE: InitCell<PacketQueue> = InitCell::new();

/// Docked handheld state manager
pub struct Handheld {
    state: HandheldState,
    docked_led: Option<LedSession>,
}

#[derive(Copy, Clone, PartialEq)]
enum HandheldState {
    /// Handheld is not connected.
    Idle,
    /// Sent info request, waiting for response.
    WaitForGetInfo,
    /// Sent dock begin, waiting for response.
    WaitForDockBegin,
    /// Docking is active.
    Active,
    /// Handheld is connected in an error state.
    Error,
}

impl Handheld {
    pub fn new() -> Self {
        TX_PACKET_QUEUE.get().reset();

        Handheld {
            state: HandheldState::Idle,
            docked_led: None,
        }
    }

    pub fn active(&self) -> bool {
        self.state == HandheldState::Active
    }

    pub fn handle_mount(&mut self) {
        assert!(self.state == HandheldState::Idle);
        self.state = HandheldState::WaitForGetInfo;
        usb_host::handheld_control_in(REQ_GET_INFO, 0, 0, 16);
    }

    pub fn handle_unmount(&mut self) {
        TX_PACKET_QUEUE.get().reset();
        unsafe { sys::GpioSetHdmiActive(false) };
    }

    pub fn handle_gamepad_connected(&mut self, gamepad: &Gamepad) {
        // 4 byte: slot
        // 4 byte: reserved
        // 8 byte: gamepad device ID
        // 32 byte: model name (\0 terminated)
        let mut buf = [0u8; 48];
        buf[0..4].copy_from_slice(&gamepad.id.0.to_le_bytes());
        buf[8..16].copy_from_slice(&gamepad.device_id);
        // TODO gamepad model name

        let packet = Packet::new(REQ_GAMEPAD_CONNECT, &buf);
        TX_PACKET_QUEUE.get().push_back(packet);
    }

    pub fn handle_gamepad_disconnected(&mut self, id: GamepadId) {
        let mut buf = [0u8; 4];
        buf[0..4].copy_from_slice(&id.0.to_le_bytes());

        let packet = Packet::new(REQ_GAMEPAD_DISCONNECT, &buf);
        TX_PACKET_QUEUE.get().push_back(packet);
    }

    pub fn handle_gamepad_data(&mut self, id: GamepadId, data: &GamepadData) {
        TX_PACKET_QUEUE
            .get()
            .push_back(Packet::gamepad_data(id, data));
    }

    pub fn handle_xfer_complete(&mut self, result: usb_host::HandheldXferResult) {
        match (self.state, result.request) {
            (HandheldState::WaitForGetInfo, REQ_GET_INFO) => {
                let data = match result.data() {
                    Ok(data) if data.len() >= 16 => data,
                    _ => {
                        log::error!("GetInfo error");
                        self.state = HandheldState::Error;
                        return;
                    }
                };

                // Read the Handheld device info.
                let serial = u32::from_le_bytes(data[4..8].try_into().unwrap());
                let hw_version = u32::from_le_bytes(data[8..12].try_into().unwrap());
                let fw_version = u32::from_le_bytes(data[12..16].try_into().unwrap());
                log::info!("-> Serial: {:08X}", serial);
                log::info!("-> HW    : {:08X}", hw_version);
                log::info!("-> FW    : {:08X}", fw_version);

                // Send the Dock device info.
                let mut data = [0u8; 32];
                data[4..8].copy_from_slice(&crate::info::SerialNumber::get().0.to_le_bytes());
                data[8..12]
                    .copy_from_slice(&crate::info::HardwareVersion::get().as_u32().to_le_bytes());
                data[12..16]
                    .copy_from_slice(&crate::info::FirmwareVersion::get().as_u32().to_le_bytes());
                // Human-readable firmware version
                let fw_version = env!("CARGO_PKG_VERSION");
                data[16..32][..fw_version.len().min(16)].copy_from_slice(fw_version.as_bytes());

                usb_host::handheld_control_out(REQ_DOCK_BEGIN, 0, 0, &data);
                self.state = HandheldState::WaitForDockBegin;
            }
            (HandheldState::WaitForDockBegin, REQ_DOCK_BEGIN) => {
                if !result.data().is_ok() {
                    log::error!("Dock begin failure");
                    self.state = HandheldState::Error;
                    return;
                }
                log::info!("Dock begin");

                unsafe { sys::GpioSetHdmiActive(true) };
                self.state = HandheldState::Active;
                self.docked_led = Some(led::begin(led::LedState::DockActive));
            }
            _ => {
                if !result.data().is_ok() {
                    log::warn!("Xfer (req={}) failed", result.request);
                }
            }
        }
    }
}

pub struct Packet {
    id: u8,
    payload: [u8; 60],
    payload_len: u8,
}

impl Packet {
    pub fn new(id: u8, payload: &[u8]) -> Packet {
        assert!(payload.len() <= 60);
        let mut buffer = [0u8; 60];
        buffer[..payload.len()].copy_from_slice(payload);
        Packet {
            id,
            payload: buffer,
            payload_len: payload.len() as u8,
        }
    }

    /// REQ_GAMEPAD_DATA: 4 byte slot, then the 16 GamepadData bytes.
    pub fn gamepad_data(id: GamepadId, data: &GamepadData) -> Packet {
        let mut buf = [0u8; 4 + GamepadData::SIZE];
        buf[0..4].copy_from_slice(&id.0.to_le_bytes());
        buf[4..].copy_from_slice(&data.to_le_bytes());
        Packet::new(REQ_GAMEPAD_DATA, &buf)
    }

    /// The slot and data of a REQ_GAMEPAD_DATA packet.
    fn get_gamepad_data(&self) -> Option<(GamepadId, GamepadData)> {
        if self.id != REQ_GAMEPAD_DATA || (self.payload_len as usize) < 4 + GamepadData::SIZE {
            return None;
        }
        let id = u32::from_le_bytes(self.payload[0..4].try_into().unwrap());
        let data = GamepadData::from_le_bytes(&self.payload[4..4 + GamepadData::SIZE])?;
        Some((GamepadId(id), data))
    }

    fn set_gamepad_data(&mut self, data: &GamepadData) {
        self.payload[4..4 + GamepadData::SIZE].copy_from_slice(&data.to_le_bytes());
    }

    pub fn payload(&self) -> &[u8] {
        &self.payload[..self.payload_len as usize]
    }

    pub fn encode_to(&self, buf: &mut [u8]) -> usize {
        assert!(buf.len() >= 64);
        let len = 4 + (self.payload_len as usize);
        buf[0] = self.id;
        buf[1] = 0;
        buf[2] = 0;
        buf[3] = 0;
        buf[4..len].copy_from_slice(self.payload());
        len
    }
}

pub struct PacketQueue {
    packets: Mutex<VecDeque<Packet>>,
}

impl PacketQueue {
    pub fn new() -> Self {
        PacketQueue {
            packets: Mutex::new(VecDeque::new()).unwrap(),
        }
    }

    pub fn pop_front(&self) -> Option<Packet> {
        let mut queue = self.packets.lock(Duration::infinite()).unwrap();
        queue.pop_front()
    }

    pub fn push_back(&self, packet: Packet) {
        let mut queue = self.packets.lock(Duration::infinite()).unwrap();
        push_back_locked(&mut queue, packet);
    }

    pub fn reset(&self) {
        let mut queue = self.packets.lock(Duration::infinite()).unwrap();
        queue.clear();
    }
}

/// Queue a packet for the handheld.
///
/// Gamepad data packets carry the full state, so if the newest queued packet is
/// one, it is simply replaced (as before). Mouse data packets (buttons::MOUSE)
/// carry movement deltas, which must not be lost while the link is busy: a new
/// mouse packet is added into the queued packet of the same mouse when the
/// buttons are unchanged, and queued behind it when they differ, so that every
/// button transition reaches the handheld. A queued mouse packet is never
/// replaced by a gamepad packet. The queue stays bounded: at most one
/// movement-only packet per mouse plus one per (human-rate) button change, and
/// one trailing gamepad packet.
fn push_back_locked(queue: &mut VecDeque<Packet>, packet: Packet) {
    if let Some((id, data)) = packet.get_gamepad_data() {
        if data.is_mouse() {
            // Newest queued packet of this mouse, if any.
            for queued in queue.iter_mut().rev() {
                let Some((queued_id, queued_data)) = queued.get_gamepad_data() else {
                    continue;
                };
                if queued_id != id || !queued_data.is_mouse() {
                    continue;
                }
                if queued_data.buttons() == data.buttons() {
                    queued.set_gamepad_data(&queued_data.merge_mouse(&data));
                    return;
                }
                break;
            }
            queue.push_back(packet);
            return;
        }

        // If the last packet pushed is a (non-mouse) REQ_GAMEPAD_DATA packet, just replace it.
        if let Some(back) = queue.back_mut()
            && let Some((_, back_data)) = back.get_gamepad_data()
            && !back_data.is_mouse()
        {
            *back = packet;
            log::warn!("Dropping gamepad data");
            return;
        }
    }

    queue.push_back(packet);
}

pub fn init() {
    TX_PACKET_QUEUE.set(PacketQueue::new());
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gamepad::buttons;

    fn mouse(id: u32, buttons: u32, dx: i16, dy: i16, wheel: i16) -> Packet {
        Packet::gamepad_data(
            GamepadId(id),
            &GamepadData::new(buttons::MOUSE | buttons, dx, dy, wheel as u16, 0, 0, 0),
        )
    }

    fn gamepad(id: u32, buttons: u32) -> Packet {
        Packet::gamepad_data(GamepadId(id), &GamepadData::new(buttons, 0, 0, 0, 0, 0, 0))
    }

    fn contents(queue: &VecDeque<Packet>) -> alloc::vec::Vec<(u8, Option<(u32, u32, i16, i16, i16)>)> {
        queue
            .iter()
            .map(|p| {
                let data = p.get_gamepad_data().map(|(id, d)| {
                    (id.0, d.buttons(), d.to_le_bytes()[4..6].try_into().map(i16::from_le_bytes).unwrap(),
                     d.to_le_bytes()[6..8].try_into().map(i16::from_le_bytes).unwrap(),
                     d.to_le_bytes()[8..10].try_into().map(i16::from_le_bytes).unwrap())
                });
                (p.id, data)
            })
            .collect()
    }

    #[test]
    fn mouse_movement_merges_into_queued_packet() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, mouse(7, buttons::MOUSE_LEFT, 3, -2, 1));
        push_back_locked(&mut q, mouse(7, buttons::MOUSE_LEFT, 4, -5, -3));
        push_back_locked(&mut q, mouse(7, buttons::MOUSE_LEFT, -1, 0, 0));
        assert_eq!(
            contents(&q),
            [(REQ_GAMEPAD_DATA, Some((7, buttons::MOUSE | buttons::MOUSE_LEFT, 6, -7, -2)))]
        );
    }

    #[test]
    fn mouse_button_change_is_queued_separately() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, mouse(7, 0, 3, 0, 0));
        push_back_locked(&mut q, mouse(7, buttons::MOUSE_LEFT, 1, 0, 0));
        push_back_locked(&mut q, mouse(7, buttons::MOUSE_LEFT, 1, 0, 0));
        push_back_locked(&mut q, mouse(7, 0, 2, 0, 0));
        assert_eq!(
            contents(&q),
            [
                (REQ_GAMEPAD_DATA, Some((7, buttons::MOUSE, 3, 0, 0))),
                (REQ_GAMEPAD_DATA, Some((7, buttons::MOUSE | buttons::MOUSE_LEFT, 2, 0, 0))),
                (REQ_GAMEPAD_DATA, Some((7, buttons::MOUSE, 2, 0, 0))),
            ]
        );
    }

    #[test]
    fn two_mice_accumulate_independently() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, mouse(1, 0, 10, 0, 0));
        push_back_locked(&mut q, mouse(2, 0, -10, 1, 0));
        push_back_locked(&mut q, mouse(1, 0, 5, 0, 0));
        push_back_locked(&mut q, mouse(2, 0, -5, 1, 0));
        assert_eq!(
            contents(&q),
            [
                (REQ_GAMEPAD_DATA, Some((1, buttons::MOUSE, 15, 0, 0))),
                (REQ_GAMEPAD_DATA, Some((2, buttons::MOUSE, -15, 2, 0))),
            ]
        );
    }

    #[test]
    fn gamepad_packets_do_not_disturb_mouse_packets() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, mouse(1, 0, 10, 0, 0));
        // A gamepad packet behind a mouse packet is queued, not merged into it.
        push_back_locked(&mut q, gamepad(3, buttons::A));
        // A second gamepad packet replaces the first (existing behaviour).
        push_back_locked(&mut q, gamepad(3, buttons::B));
        // More mouse movement still merges into the mouse packet.
        push_back_locked(&mut q, mouse(1, 0, 1, 1, 0));
        // A mouse packet never replaces a gamepad packet.
        push_back_locked(&mut q, mouse(2, 0, 1, 0, 0));
        assert_eq!(
            contents(&q),
            [
                (REQ_GAMEPAD_DATA, Some((1, buttons::MOUSE, 11, 1, 0))),
                (REQ_GAMEPAD_DATA, Some((3, buttons::B, 0, 0, 0))),
                (REQ_GAMEPAD_DATA, Some((2, buttons::MOUSE, 1, 0, 0))),
            ]
        );
    }

    #[test]
    fn merge_saturates() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, mouse(1, 0, 30000, -30000, 30000));
        push_back_locked(&mut q, mouse(1, 0, 30000, -30000, 30000));
        assert_eq!(
            contents(&q),
            [(REQ_GAMEPAD_DATA, Some((1, buttons::MOUSE, i16::MAX, i16::MIN, i16::MAX)))]
        );
    }

    #[test]
    fn other_packets_are_appended() {
        let mut q = VecDeque::new();
        push_back_locked(&mut q, Packet::new(REQ_GAMEPAD_CONNECT, &[1, 0, 0, 0]));
        push_back_locked(&mut q, gamepad(1, buttons::A));
        push_back_locked(&mut q, Packet::new(REQ_GAMEPAD_DISCONNECT, &[1, 0, 0, 0]));
        push_back_locked(&mut q, gamepad(2, buttons::A));
        assert_eq!(q.len(), 4);
        let mut buf = [0u8; 64];
        assert_eq!(q[1].encode_to(&mut buf), 24);
        assert_eq!(buf[0], REQ_GAMEPAD_DATA);
        assert_eq!(&buf[4..8], &[1, 0, 0, 0]);
    }
}
