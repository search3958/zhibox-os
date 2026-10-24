//! Linux input backend for the ported BaramOS desktop.
//!
//! `baram-boot` was written against Nano System, a small UEFI input
//! abstraction.  This crate keeps the same public surface while reading evdev
//! devices directly.  Keyboard events are translated to the printable ASCII
//! and scan codes the desktop already understands; pointers are reported as
//! relative motion with buttons and wheel state.

use std::collections::HashSet;
use std::ffi::CString;
use std::fs::File;
use std::io::Read;
use std::os::unix::io::FromRawFd;
use std::time::{Duration, Instant};

use uefi::Event;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NanoDisplayInfo {
    pub width: usize,
    pub height: usize,
    pub stride: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NanoInputInfo {
    pub keyboard_available: bool,
    pub pointer_available: bool,
    pub absolute_pointer_available: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct NanoInputState {
    pub key_sequence: u64,
    pub pointer_sequence: u64,
    pub last_printable: Option<u8>,
    pub last_scancode: u16,
    pub modifiers: u8,
    pub pointer_dx: i32,
    pub pointer_dy: i32,
    pub pointer_x: u64,
    pub pointer_y: u64,
    pub pointer_max_x: u64,
    pub pointer_max_y: u64,
    pub pointer_is_absolute: bool,
    pub pointer_is_trackpad: bool,
    pub left: bool,
    pub right: bool,
    pub middle: bool,
    pub scroll: i32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct NanoBasicPointerEvent {
    pub dx: i32,
    pub dy: i32,
    pub absolute: Option<(u64, u64, u64, u64)>,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct NanoKeyEvent {
    pub printable: Option<u8>,
    pub scancode: u16,
    pub modifiers: u8,
    pub raw_key: u8,
}

enum Queued {
    Key(NanoKeyEvent),
    Pointer(NanoBasicPointerEvent),
}

struct Device {
    file: File,
}

#[derive(Default)]
struct PendingPointer {
    dx: i32,
    dy: i32,
    scroll: i32,
    changed: bool,
}

pub struct NanoSystem {
    pub display: NanoDisplayInfo,
    pub input: NanoInputInfo,
    pub input_state: NanoInputState,
    timer_event: Option<Event>,
    devices: Vec<Device>,
    queue: Vec<Queued>,
    pressed: HashSet<u16>,
    held_hid: [bool; 256],
    pending: PendingPointer,
    last_open_try: Option<Instant>,
}

#[repr(C)]
struct InputEvent {
    time: libc::timeval,
    type_: u16,
    code: u16,
    value: i32,
}

const EV_SYN: u16 = 0;
const EV_KEY: u16 = 1;
const EV_REL: u16 = 2;

const REL_X: u16 = 0;
const REL_Y: u16 = 1;
const REL_HWHEEL: u16 = 6;
const REL_WHEEL: u16 = 8;

const BTN_LEFT: u16 = 0x110;
const BTN_RIGHT: u16 = 0x111;
const BTN_MIDDLE: u16 = 0x112;

const KEY_ESC: u16 = 1;
const KEY_1: u16 = 2;
const KEY_0: u16 = 11;
const KEY_MINUS: u16 = 12;
const KEY_EQUAL: u16 = 13;
const KEY_BACKSPACE: u16 = 14;
const KEY_TAB: u16 = 15;
const KEY_Q: u16 = 16;
const KEY_A: u16 = 30;
const KEY_Z: u16 = 44;
const KEY_LEFTBRACE: u16 = 26;
const KEY_RIGHTBRACE: u16 = 27;
const KEY_ENTER: u16 = 28;
const KEY_LEFTCTRL: u16 = 29;
const KEY_SEMICOLON: u16 = 39;
const KEY_APOSTROPHE: u16 = 40;
const KEY_GRAVE: u16 = 41;
const KEY_LEFTSHIFT: u16 = 42;
const KEY_BACKSLASH: u16 = 43;
const KEY_COMMA: u16 = 51;
const KEY_DOT: u16 = 52;
const KEY_SLASH: u16 = 53;
const KEY_RIGHTSHIFT: u16 = 54;
const KEY_LEFTALT: u16 = 56;
const KEY_SPACE: u16 = 57;
const KEY_CAPSLOCK: u16 = 58;
const KEY_RIGHTCTRL: u16 = 97;
const KEY_RIGHTALT: u16 = 100;
const KEY_HOME: u16 = 102;
const KEY_UP: u16 = 103;
const KEY_PAGEUP: u16 = 104;
const KEY_LEFT: u16 = 105;
const KEY_RIGHT: u16 = 106;
const KEY_END: u16 = 107;
const KEY_DOWN: u16 = 108;
const KEY_PAGEDOWN: u16 = 109;
const KEY_INSERT: u16 = 110;
const KEY_DELETE: u16 = 111;
const KEY_LEFTMETA: u16 = 125;
const KEY_RIGHTMETA: u16 = 126;
const KEY_KP0: u16 = 82;
const KEY_KP9: u16 = 73;
const KEY_KPENTER: u16 = 96;

impl NanoSystem {
    pub fn start() -> Self {
        let width = std::env::var("ZH_SCREEN_W")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(1280);
        let height = std::env::var("ZH_SCREEN_H")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(720);
        let mut system = Self {
            display: NanoDisplayInfo {
                width,
                height,
                stride: width,
            },
            input: NanoInputInfo {
                keyboard_available: true,
                pointer_available: true,
                absolute_pointer_available: false,
            },
            input_state: NanoInputState::default(),
            timer_event: Some(Event),
            devices: Vec::new(),
            queue: Vec::new(),
            pressed: HashSet::new(),
            held_hid: [false; 256],
            pending: PendingPointer::default(),
            last_open_try: None,
        };
        system.open_devices();
        system
    }

    fn open_devices(&mut self) {
        if !self.devices.is_empty() {
            return;
        }
        let now = Instant::now();
        if self
            .last_open_try
            .is_some_and(|last| now.duration_since(last) < Duration::from_millis(500))
        {
            return;
        }
        self.last_open_try = Some(now);
        for index in 0..32 {
            let path = format!("/dev/input/event{index}");
            let Ok(path) = CString::new(path) else {
                continue;
            };
            let fd = unsafe {
                libc::open(
                    path.as_ptr(),
                    libc::O_RDONLY | libc::O_NONBLOCK | libc::O_CLOEXEC,
                )
            };
            if fd >= 0 {
                self.devices.push(Device {
                    file: unsafe { File::from_raw_fd(fd) },
                });
            }
        }
        if !self.devices.is_empty() {
            Self::serial_log(&format!(
                "desktop: evdev ready ({} device(s))\n",
                self.devices.len()
            ));
        }
    }

    fn pump(&mut self) {
        self.open_devices();
        for index in 0..self.devices.len() {
            loop {
                let mut event = InputEvent {
                    time: libc::timeval {
                        tv_sec: 0,
                        tv_usec: 0,
                    },
                    type_: 0,
                    code: 0,
                    value: 0,
                };
                let fd = std::os::unix::io::AsRawFd::as_raw_fd(&self.devices[index].file);
                let n = unsafe {
                    libc::read(
                        fd,
                        &mut event as *mut InputEvent as *mut libc::c_void,
                        core::mem::size_of::<InputEvent>(),
                    )
                };
                if n != core::mem::size_of::<InputEvent>() as isize {
                    break;
                }
                self.handle_event(&event);
            }
        }
    }

    fn handle_event(&mut self, event: &InputEvent) {
        match event.type_ {
            EV_KEY => self.handle_key(event.code, event.value),
            EV_REL => match event.code {
                REL_X => self.pending.dx = self.pending.dx.saturating_add(event.value),
                REL_Y => self.pending.dy = self.pending.dy.saturating_add(event.value),
                REL_WHEEL => self.pending.scroll = self.pending.scroll.saturating_add(event.value),
                REL_HWHEEL => {}
                _ => {}
            },
            EV_SYN if event.code == 0 => {
                let state = &mut self.input_state;
                if self.pending.changed {
                    state.pointer_sequence = state.pointer_sequence.wrapping_add(1);
                }
                if self.pending.dx != 0
                    || self.pending.dy != 0
                    || self.pending.scroll != 0
                    || self.pending.changed
                {
                    state.pointer_dx = self.pending.dx;
                    state.pointer_dy = self.pending.dy;
                    state.scroll = self.pending.scroll;
                    self.queue.push(Queued::Pointer(NanoBasicPointerEvent {
                        dx: self.pending.dx,
                        dy: self.pending.dy,
                        absolute: None,
                    }));
                    self.pending = PendingPointer::default();
                }
            }
            _ => {}
        }
    }

    fn handle_key(&mut self, code: u16, value: i32) {
        match code {
            BTN_LEFT | BTN_RIGHT | BTN_MIDDLE => {
                let pressed = value != 0;
                match code {
                    BTN_LEFT => self.input_state.left = pressed,
                    BTN_RIGHT => self.input_state.right = pressed,
                    _ => self.input_state.middle = pressed,
                }
                self.pending.changed = true;
                return;
            }
            _ => {}
        }

        if value == 0 {
            self.pressed.remove(&code);
            if let Some(hid) = evdev_to_hid(code) {
                self.held_hid[hid as usize] = false;
            }
        } else if value == 1 {
            self.pressed.insert(code);
        }
        self.refresh_modifiers();

        if value == 0 || is_modifier(code) {
            return;
        }

        let shift = self.shift_held();
        let ctrl = self.ctrl_or_cmd_held();
        let printable = printable_for(code, shift);
        let scancode = scancode_for(code);
        if printable.is_none() && scancode == 0 {
            return;
        }
        let hid = evdev_to_hid(code).unwrap_or(0);
        if value == 1 {
            if let Some(hid) = evdev_to_hid(code) {
                self.held_hid[hid as usize] = true;
            }
        }
        self.input_state.key_sequence = self.input_state.key_sequence.wrapping_add(1);
        self.input_state.last_printable = printable;
        self.input_state.last_scancode = scancode;
        self.input_state.modifiers = self.modifiers();
        let _ = ctrl;
        self.queue.push(Queued::Key(NanoKeyEvent {
            printable,
            scancode,
            modifiers: self.input_state.modifiers,
            raw_key: hid,
        }));
    }

    fn release_hid(&mut self, code: u16) {
        if let Some(hid) = evdev_to_hid(code) {
            if !self.pressed.contains(&code) {
                self.held_hid[hid as usize] = false;
            }
        }
    }

    fn refresh_modifiers(&mut self) {
        for &code in &[KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA] {
            if !self.pressed.contains(&code) {
                self.release_hid(code);
            }
        }
        self.input_state.modifiers = self.modifiers();
    }

    fn modifiers(&self) -> u8 {
        let mut bits = 0u8;
        if self.pressed.contains(&KEY_LEFTSHIFT) {
            bits |= 0x02;
        }
        if self.pressed.contains(&KEY_RIGHTSHIFT) {
            bits |= 0x20;
        }
        if self.pressed.contains(&KEY_LEFTCTRL) {
            bits |= 0x01;
        }
        if self.pressed.contains(&KEY_RIGHTCTRL) {
            bits |= 0x10;
        }
        if self.pressed.contains(&KEY_LEFTALT) || self.pressed.contains(&KEY_RIGHTALT) {
            bits |= 0x04;
        }
        if self.pressed.contains(&KEY_LEFTMETA) || self.pressed.contains(&KEY_RIGHTMETA) {
            bits |= 0x08;
        }
        bits
    }

    pub fn serial_log(message: &str) {
        eprint!("{message}");
    }

    pub fn panic_report(_info: &core::panic::PanicInfo) -> ! {
        std::process::exit(101)
    }

    pub fn paint_failure_screen() {}

    pub fn cold_reset() -> ! {
        std::process::exit(0)
    }

    pub fn take_timer_event(&mut self) -> Option<Event> {
        self.timer_event.take()
    }

    pub fn poll_keyboard(&mut self) -> Option<NanoKeyEvent> {
        self.pump();
        let index = self.queue.iter().position(|event| matches!(event, Queued::Key(_)))?;
        match self.queue.remove(index) {
            Queued::Key(event) => Some(event),
            _ => None,
        }
    }

    pub fn poll_pointer(&mut self) -> Option<NanoBasicPointerEvent> {
        self.pump();
        let index = self
            .queue
            .iter()
            .position(|event| matches!(event, Queued::Pointer(_)))?;
        match self.queue.remove(index) {
            Queued::Pointer(event) => Some(event),
            _ => None,
        }
    }

    pub fn pointer_abs_max(&self) -> (u64, u64) {
        (1, 1)
    }

    pub fn key_is_held(&self, code: u8) -> bool {
        self.held_hid.get(code as usize).copied().unwrap_or(false)
    }

    pub fn shift_held(&self) -> bool {
        self.input_state.modifiers & 0x22 != 0
    }

    pub fn ctrl_or_cmd_held(&self) -> bool {
        self.input_state.modifiers & 0x11 != 0
    }

    pub fn set_shift_key(&mut self, _key: u8) {}
}

fn is_modifier(code: u16) -> bool {
    matches!(
        code,
        KEY_LEFTCTRL
            | KEY_RIGHTCTRL
            | KEY_LEFTSHIFT
            | KEY_RIGHTSHIFT
            | KEY_LEFTALT
            | KEY_RIGHTALT
            | KEY_LEFTMETA
            | KEY_RIGHTMETA
    )
}

fn scancode_for(code: u16) -> u16 {
    match code {
        KEY_UP => 0x01,
        KEY_DOWN => 0x02,
        KEY_LEFT => 0x03,
        KEY_RIGHT => 0x04,
        KEY_PAGEUP => 0x05,
        KEY_PAGEDOWN => 0x06,
        KEY_HOME => 0x07,
        KEY_END => 0x08,
        KEY_INSERT => 0x09,
        KEY_DELETE => 0x0A,
        KEY_BACKSPACE => 0x08,
        _ => 0,
    }
}

fn printable_for(code: u16, shift: bool) -> Option<u8> {
    let lower = |c: u8| if shift { c.to_ascii_uppercase() } else { c };
    match code {
        KEY_A..=KEY_Z => Some(lower(b'a' + (code - KEY_A) as u8)),
        KEY_1..=KEY_0 => {
            const BASE: &[u8; 10] = b"1234567890";
            const SHIFTED: &[u8; 10] = b"!@#$%^&*()";
            let index = (code - KEY_1) as usize;
            Some(if shift { SHIFTED[index] } else { BASE[index] })
        }
        KEY_SPACE => Some(b' '),
        KEY_ENTER | KEY_KPENTER => Some(b'\n'),
        KEY_TAB => Some(b'\t'),
        KEY_ESC => Some(0x1B),
        KEY_MINUS => Some(if shift { b'_' } else { b'-' }),
        KEY_EQUAL => Some(if shift { b'+' } else { b'=' }),
        KEY_LEFTBRACE => Some(if shift { b'{' } else { b'[' }),
        KEY_RIGHTBRACE => Some(if shift { b'}' } else { b']' }),
        KEY_BACKSLASH => Some(if shift { b'|' } else { b'\\' }),
        KEY_SEMICOLON => Some(if shift { b':' } else { b';' }),
        KEY_APOSTROPHE => Some(if shift { b'"' } else { b'\'' }),
        KEY_GRAVE => Some(if shift { b'~' } else { b'`' }),
        KEY_COMMA => Some(if shift { b'<' } else { b',' }),
        KEY_DOT => Some(if shift { b'>' } else { b'.' }),
        KEY_SLASH => Some(if shift { b'?' } else { b'/' }),
        KEY_Q => Some(lower(b'q')),
        _ => None,
    }
}

fn evdev_to_hid(code: u16) -> Option<u8> {
    if (KEY_A..=KEY_Z).contains(&code) {
        return Some(0x04 + (code - KEY_A) as u8);
    }
    if (KEY_1..=KEY_0).contains(&code) {
        return Some(0x1E + (code - KEY_1) as u8);
    }
    match code {
        KEY_ENTER => Some(0x28),
        KEY_ESC => Some(0x29),
        KEY_BACKSPACE => Some(0x2A),
        KEY_TAB => Some(0x2B),
        KEY_SPACE => Some(0x2C),
        KEY_MINUS => Some(0x2D),
        KEY_EQUAL => Some(0x2E),
        KEY_LEFTBRACE => Some(0x2F),
        KEY_RIGHTBRACE => Some(0x30),
        KEY_BACKSLASH => Some(0x31),
        KEY_SEMICOLON => Some(0x33),
        KEY_APOSTROPHE => Some(0x34),
        KEY_GRAVE => Some(0x35),
        KEY_COMMA => Some(0x36),
        KEY_DOT => Some(0x37),
        KEY_SLASH => Some(0x38),
        KEY_CAPSLOCK => Some(0x39),
        _ => None,
    }
}

#[macro_export]
macro_rules! nano_entry {
    ($application:path) => {};
}
