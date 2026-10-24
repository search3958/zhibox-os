//! Tiny host-side stand-in for the UEFI services used by the baram GUI crates.
//!
//! The compositor only needs three platform primitives from `uefi`: a wall
//! clock, a millisecond timer wait, and a status value.  Keeping the exact
//! shape here lets the upstream UEFI window-server sources compile unchanged
//! into the Linux desktop.

use core::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status(pub usize);

impl Status {
    pub const SUCCESS: Self = Self(0);
    pub const UNSUPPORTED: Self = Self(3);
    pub const DEVICE_ERROR: Self = Self(7);

    pub const fn is_success(self) -> bool {
        self.0 & 0x8000_0000_0000_0000 == 0
    }
}

/// Opaque timer handle.  The Linux input backend uses a real clock instead of
/// firmware timer events, so the value carries no data.
#[derive(Clone, Copy, Debug, Default)]
pub struct Event;

pub mod prelude {
    pub use crate::Status;
}

pub mod runtime {
    use super::{Event, Status};
    use core::ffi::c_void;

    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
    pub struct Time {
        hour: u8,
        minute: u8,
        second: u8,
        nanosecond: u32,
    }

    impl Time {
        pub const fn invalid() -> Self {
            Self {
                hour: 0,
                minute: 0,
                second: 0,
                nanosecond: 0,
            }
        }

        pub const fn hour(&self) -> u8 {
            self.hour
        }
        pub const fn minute(&self) -> u8 {
            self.minute
        }
        pub const fn second(&self) -> u8 {
            self.second
        }
        pub const fn nanosecond(&self) -> u32 {
            self.nanosecond
        }
    }

    pub fn get_time() -> Result<Time, Status> {
        unsafe {
            let mut now: libc::time_t = 0;
            if libc::time(&mut now) < 0 {
                return Err(Status::DEVICE_ERROR);
            }
            let mut tm: libc::tm = core::mem::zeroed();
            if libc::localtime_r(&now, &mut tm).is_null() {
                return Err(Status::DEVICE_ERROR);
            }
            let mut ts: libc::timespec = core::mem::zeroed();
            let ns = if libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts) == 0 {
                ts.tv_nsec as u32
            } else {
                0
            };
            Ok(Time {
                hour: tm.tm_hour as u8,
                minute: tm.tm_min as u8,
                second: tm.tm_sec as u8,
                nanosecond: ns,
            })
        }
    }

    pub fn reset(_reset_type: u32, _status: Status, _data: Option<*mut c_void>) -> ! {
        std::process::exit(0)
    }
}

pub mod boot {
    use super::{Event, Status};
    use core::time::Duration;

    /// Wait for the 1 ms desktop timer tick.  `Event` is a placeholder, so a
    /// short sleep is the Linux equivalent of firmware event notification.
    pub fn wait_for_event(_events: &mut [Event]) -> Result<(), Status> {
        stall(Duration::from_millis(1));
        Ok(())
    }

    pub fn stall(duration: Duration) {
        std::thread::sleep(duration);
    }
}

pub use boot::stall;
