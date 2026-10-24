//! Pointer processing used by the Linux desktop.
//!
//! evdev already delivers relative counts.  The BaramOS desktop expects the
//! same small quadratic feel as the UEFI build, so keep a fractional
//! accumulator here instead of making the compositor aware of input devices.

#[derive(Clone, Copy, Debug, Default)]
pub struct MouseEvent {
    pub abs_x: u64,
    pub abs_y: u64,
    pub abs_max_x: u64,
    pub abs_max_y: u64,
    pub rel_dx: i32,
    pub rel_dy: i32,
    pub trackpad_dx: i32,
    pub trackpad_dy: i32,
    pub is_absolute: bool,
    pub is_trackpad: bool,
    pub left: bool,
    pub right: bool,
    pub middle: bool,
    pub scroll: i32,
}

pub struct MouseMotionProcessor {
    fraction_x: f32,
    fraction_y: f32,
    filtered_speed: f32,
    buttons: u8,
}

impl MouseMotionProcessor {
    pub fn new() -> Self {
        Self {
            fraction_x: 0.0,
            fraction_y: 0.0,
            filtered_speed: 0.0,
            buttons: 0,
        }
    }

    pub fn process(&mut self, mut event: MouseEvent) -> MouseEvent {
        if event.is_absolute || event.is_trackpad {
            return event;
        }
        let buttons = (event.left as u8) | ((event.right as u8) << 1) | ((event.middle as u8) << 2);
        if buttons != self.buttons {
            self.buttons = buttons;
            self.fraction_x = 0.0;
            self.fraction_y = 0.0;
        }
        if event.rel_dx == 0 && event.rel_dy == 0 {
            self.filtered_speed *= 0.10;
            return event;
        }
        let abs_x = (event.rel_dx as f32).abs();
        let abs_y = (event.rel_dy as f32).abs();
        let (major, minor) = if abs_x >= abs_y {
            (abs_x, abs_y)
        } else {
            (abs_y, abs_x)
        };
        let instant = major + minor * 0.375;
        self.filtered_speed = self.filtered_speed * 0.10 + instant * 0.90;
        let gain = ((0.12 + 0.004 * self.filtered_speed) * 6.0).clamp(1.0, 8.0);
        let scaled_x = event.rel_dx as f32 * gain + self.fraction_x;
        let scaled_y = event.rel_dy as f32 * gain + self.fraction_y;
        event.rel_dx = scaled_x as i32;
        event.rel_dy = scaled_y as i32;
        self.fraction_x = scaled_x - event.rel_dx as f32;
        self.fraction_y = scaled_y - event.rel_dy as f32;
        event
    }
}

impl Default for MouseMotionProcessor {
    fn default() -> Self {
        Self::new()
    }
}

pub fn apply_mouse_event(
    cx: &mut i32,
    cy: &mut i32,
    event: &MouseEvent,
    screen_w: usize,
    screen_h: usize,
    fallback_abs_max: (u64, u64),
) -> (i32, i32) {
    if screen_w == 0 || screen_h == 0 {
        return (*cx, *cy);
    }
    let abs_max_x = if event.abs_max_x != 0 {
        event.abs_max_x
    } else {
        fallback_abs_max.0
    };
    let abs_max_y = if event.abs_max_y != 0 {
        event.abs_max_y
    } else {
        fallback_abs_max.1
    };
    if event.is_absolute && abs_max_x != 0 && abs_max_y != 0 {
        *cx = ((event.abs_x as u128 * (screen_w - 1) as u128) / abs_max_x as u128)
            .min((screen_w - 1) as u128) as i32;
        *cy = ((event.abs_y as u128 * (screen_h - 1) as u128) / abs_max_y as u128)
            .min((screen_h - 1) as u128) as i32;
    } else {
        *cx = cx
            .saturating_add(event.rel_dx)
            .clamp(0, screen_w as i32 - 1);
        *cy = cy
            .saturating_add(event.rel_dy)
            .clamp(0, screen_h as i32 - 1);
    }
    (*cx, *cy)
}

pub fn log_line_str(message: &str) {
    eprintln!("{message}");
}
