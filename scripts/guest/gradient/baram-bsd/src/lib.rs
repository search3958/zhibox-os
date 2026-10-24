//! Linux host shim for the parts of `baram-bsd` used by the BaramOS GUI.
//!
//! The upstream crate is UEFI/ESP32 only.  This shim keeps the same public
//! surface while mapping the BaramOS VFS onto an ordinary directory tree.

extern crate alloc;

pub mod app;
pub mod config;
pub mod security;
pub mod setup;
pub mod shift_key;
pub mod uri;
pub mod vfs;
