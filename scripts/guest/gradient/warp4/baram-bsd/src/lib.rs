//! Minimal Linux host shim for the parts of `baram-bsd` used by
//! `baram-warp4`.  The upstream crate is UEFI/ESP32 only; this shim keeps the
//! same public surface while reading the installed app archive from an
//! ordinary directory.

pub mod app;
pub mod config;
pub mod vfs;
