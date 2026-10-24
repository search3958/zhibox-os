#!/bin/sh
# Build the native BaramOS desktop for the zhibox-os Linux guest.
#
# The GUI implementation is vendored under scripts/guest/gradient/upstream.
# This script assembles a self-contained Cargo workspace, swaps the UEFI platform crates
# for small Linux shims, patches the framebuffer screen and the a couple of
# UEFI-only boot-runtime lines, then cross-compiles a static x86_64 musl
# binary that owns /dev/fb0 and the evdev devices directly.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GUI_SRC="$ROOT/scripts/guest/gradient/upstream"
OUT=${OUT:-"$ROOT/output"}
BUILD="$OUT/build/desktop-src"
DIST="$OUT/desktop"
TARGET=x86_64-unknown-linux-musl

say() {
	printf 'desktop: %s\n' "$*"
}

if [ ! -d "$GUI_SRC/crates/baram-windowserver" ]; then
	echo "desktop: vendored GUI sources not found at $GUI_SRC" >&2
	exit 1
fi

if ! rustup target list --installed | grep -qx "$TARGET"; then
	echo "desktop: installing Rust target $TARGET" >&2
	rustup target add "$TARGET"
fi

say "assembling $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD/crates/baram-boot/src" "$BUILD/files"
rm -rf "$DIST/apps"
mkdir -p "$DIST/apps"

for crate in baram-core baram-font baram-graphics baram-warp4 baram-windowserver \
	png-decoder stb_truetype_rust wana-kana; do
	cp -R "$GUI_SRC/crates/$crate" "$BUILD/crates/"
done
cp -R "$ROOT/scripts/guest/gradient/baram-bsd" "$BUILD/crates/baram-bsd"
cp -R "$ROOT/scripts/guest/gradient/uefi" "$BUILD/crates/uefi"
cp -R "$ROOT/scripts/guest/gradient/nano-system" "$BUILD/crates/nano-system"
cp -R "$ROOT/scripts/guest/gradient/baram-iokit" "$BUILD/crates/baram-iokit"
cp "$ROOT/scripts/guest/gradient/runner/Cargo.toml" "$BUILD/crates/baram-boot/Cargo.toml"
for source in main.rs ime.rs clock.rs runtime.rs runtime_input.rs runtime_pointer.rs \
	runtime_frame.rs navigation.rs mozc_dictionary.tsv pinyin_dictionary.tsv; do
	cp "$GUI_SRC/crates/baram-boot/src/$source" "$BUILD/crates/baram-boot/src/$source"
done
cp -R "$GUI_SRC/files/." "$BUILD/files/"

cat >"$BUILD/Cargo.toml" <<'CARGO'
[workspace]
resolver = "2"
members = [
    "crates/baram-core",
    "crates/baram-font",
    "crates/baram-graphics",
    "crates/baram-warp4",
    "crates/baram-windowserver",
    "crates/baram-bsd",
    "crates/baram-iokit",
    "crates/nano-system",
    "crates/uefi",
    "crates/png-decoder",
    "crates/stb_truetype_rust",
    "crates/wana-kana",
    "crates/baram-boot",
]

[workspace.dependencies]
uefi = { path = "crates/uefi" }
uefi-raw = { version = "0.15" }
libm = "0.2"
kurbo = { version = "0.13", default-features = false, features = ["libm"] }
esp-hal = { version = "1.2", default-features = false }
embedded-hal = "1"
CARGO

python3 - "$BUILD" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

# ---------------------------------------------------------------- manifests
core_toml = root / "crates/baram-core/Cargo.toml"
text = core_toml.read_text()
text = text.replace('default = ["uefi"]', 'default = ["uefi"]\nlinux = ["dep:libc"]')
if 'libc = { version = "0.2"' not in text:
    text = text.replace('[dependencies]\n',
                        '[dependencies]\nlibc = { version = "0.2", default-features = false, optional = true }\n', 1)
core_toml.write_text(text)

for crate in ("baram-warp4", "baram-graphics", "baram-windowserver"):
    manifest = root / f"crates/{crate}/Cargo.toml"
    text = manifest.read_text()
    text = text.replace('baram-core = { path = "../baram-core" }',
                        'baram-core = { path = "../baram-core", default-features = false }')
    manifest.write_text(text)

# The Warp 4 scripting clock uses `uefi::runtime` under the UEFI feature.  The
# Linux desktop enables that feature through the shim, so nothing else needs
# patching here.  Keep libc available for the non-UEFI path as well.
manifest = root / "crates/baram-warp4/Cargo.toml"
text = manifest.read_text()
if 'libc = "0.2"' not in text:
    text = text.replace('[dependencies]\n', '[dependencies]\nlibc = "0.2"\n', 1)
    manifest.write_text(text)

# The Warp 4 scripting clock asks the platform for wall-clock time.  The
# UEFI build gets it from firmware; the Linux desktop uses libc.
script = root / "crates/baram-warp4/src/script.rs"
text = script.read_text()
old = """        let time = runtime::get_time().ok()?;
        let timezone_minutes = config::timezone_offset_minutes();
        let utc_seconds =
            time.hour() as i32 * 3600 + time.minute() as i32 * 60 + time.second() as i32;"""
new = """        #[cfg(feature = "uefi")]
        let (utc_hour, utc_minute, utc_second) = {
            let time = runtime::get_time().ok()?;
            (time.hour() as i32, time.minute() as i32, time.second() as i32)
        };
        #[cfg(not(feature = "uefi"))]
        let (utc_hour, utc_minute, utc_second) = unsafe {
            let mut tm: libc::tm = core::mem::zeroed();
            let now = libc::time(core::ptr::null_mut());
            libc::localtime_r(&now, &mut tm);
            (tm.tm_hour as i32, tm.tm_min as i32, tm.tm_sec as i32)
        };
        let timezone_minutes = config::timezone_offset_minutes();
        let utc_seconds = utc_hour * 3600 + utc_minute * 60 + utc_second;"""
if old in text:
    script.write_text(text.replace(old, new))
elif "let utc_seconds = utc_hour" not in text:
    raise SystemExit("desktop: could not patch Warp 4 clock")

# --------------------------------------------------------- settings GPU tab
# The upstream settings app already owns the theme/pointer/HUD/system tabs.
# Add the two GPU actions here so the URI toggle is reachable from the GUI
# without forking the whole W4A application.
w4u = root / "files/app/settings.w4a/system.w4u"
text = w4u.read_text()
if "gpu_on" not in text:
    text = text.replace(
        '<Button baram:id="@+id/reset"',
        '<TextView baram:layout_width="fill_parent" baram:layout_height="wrap_content" '
        'baram:layout_marginTop="20dip" baram:text="GPU アクセラレーション" />'
        '<PrimaryButton baram:id="@+id/gpu_on" baram:layout_width="fill_parent" '
        'baram:layout_height="48dip" baram:text="GPU を使う (DRM 矩形転送)" />'
        '<Button baram:id="@+id/gpu_off" baram:layout_width="fill_parent" '
        'baram:layout_height="48dip" baram:text="GPU を使わない (ソフトウェア全画面)" />'
        '<Button baram:id="@+id/reset"',
        1,
    )
    w4u.write_text(text)
w4s = root / "files/app/settings.w4a/system.w4s"
text = w4s.read_text()
if "gpu_on" not in text:
    text = text.replace(
        'WarpUI.OnClick reset {',
        'WarpUI.OnClick gpu_on {\n    BaramOS run (os://display/gpu?enabled=1)\n}\n'
        'WarpUI.OnClick gpu_off {\n    BaramOS run (os://display/gpu?enabled=0)\n}\n'
        'WarpUI.OnClick reset {',
        1,
    )
    w4s.write_text(text)

# ------------------------------------------------------------- parallel/misc
parallel = root / "crates/baram-core/src/parallel.rs"
parallel.write_text('''\
pub fn init() -> usize {
    0
}

pub fn worker_count() -> usize {
    0
}

pub fn for_each<T: Sync>(count: usize, data: &T, job: fn(&T, usize)) {
    for index in 0..count {
        job(data, index);
    }
}
''')

# --------------------------------------------------------------- Linux screen
screen = root / "crates/baram-core/src/screen.rs"
screen.write_text(screen.read_text() + r'''

// ---------------------------------------------------------------- Linux host
// The desktop owns the guest framebuffer directly.  The presentation path
// intentionally has two modes:
//   * GPU on  - copy only damaged rows and notify DRM with a per-frame clip.
//   * GPU off - notify DRM for the full frame (the CPU compositor is still the
//               renderer; this is the software-presentation fallback).
const FBIOGET_VSCREENINFO: libc::c_int = 0x4600;
const FBIOGET_FSCREENINFO: libc::c_int = 0x4602;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FbBitfield {
    offset: u32,
    length: u32,
    msb_right: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FbVarScreeninfo {
    xres: u32,
    yres: u32,
    xres_virtual: u32,
    yres_virtual: u32,
    xoffset: u32,
    yoffset: u32,
    bits_per_pixel: u32,
    grayscale: u32,
    red: FbBitfield,
    green: FbBitfield,
    blue: FbBitfield,
    transp: FbBitfield,
    nonstd: u32,
    activate: u32,
    height: u32,
    width: u32,
    accel_flags: u32,
    pixclock: u32,
    left_margin: u32,
    right_margin: u32,
    upper_margin: u32,
    lower_margin: u32,
    hsync_len: u32,
    vsync_len: u32,
    sync: u32,
    vmode: u32,
    rotate: u32,
    colorspace: u32,
    reserved: [u32; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct FbFixScreeninfo {
    id: [u8; 16],
    smem_start: libc::c_ulong,
    smem_len: u32,
    type_: u32,
    type_aux: u32,
    visual: u32,
    xpanstep: u16,
    ywrapstep: u16,
    line_length: u32,
    mmio_start: libc::c_ulong,
    mmio_len: u32,
    accel: u32,
    capabilities: u16,
    reserved: [u16; 2],
}

impl Default for FbFixScreeninfo {
    fn default() -> Self {
        Self {
            id: [0; 16],
            smem_start: 0,
            smem_len: 0,
            type_: 0,
            type_aux: 0,
            visual: 0,
            xpanstep: 0,
            ywrapstep: 0,
            line_length: 0,
            mmio_start: 0,
            mmio_len: 0,
            accel: 0,
            capabilities: 0,
            reserved: [0; 2],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DrmModeModeinfo {
    clock: u32,
    hdisplay: u16,
    hsync_start: u16,
    hsync_end: u16,
    htotal: u16,
    hskew: u16,
    vdisplay: u16,
    vsync_start: u16,
    vsync_end: u16,
    vtotal: u16,
    vscan: u16,
    vrefresh: u32,
    flags: u32,
    type_: u32,
    name: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DrmModeCardRes {
    fb_id_ptr: u64,
    crtc_id_ptr: u64,
    connector_id_ptr: u64,
    encoder_id_ptr: u64,
    count_fbs: u32,
    count_crtcs: u32,
    count_connectors: u32,
    count_encoders: u32,
    min_width: u32,
    max_width: u32,
    min_height: u32,
    max_height: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DrmModeCrtc {
    set_connectors_ptr: u64,
    count_connectors: u32,
    crtc_id: u32,
    fb_id: u32,
    x: u32,
    y: u32,
    gamma_size: u32,
    mode_valid: u32,
    mode: DrmModeModeinfo,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DrmClipRect {
    x1: u16,
    y1: u16,
    x2: u16,
    y2: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DrmModeFbDirtyCmd {
    fb_id: u32,
    flags: u32,
    color: u32,
    num_clips: u32,
    clips_ptr: u64,
}

const DRM_IOCTL_MODE_GETRESOURCES: libc::c_int = 0xC04064A0u32 as libc::c_int;
const DRM_IOCTL_MODE_GETCRTC: libc::c_int = 0xC06864A1u32 as libc::c_int;
const DRM_IOCTL_MODE_DIRTYFB: libc::c_int = 0xC01864B1u32 as libc::c_int;

fn linux_log(message: &str) {
    unsafe {
        libc::write(2, message.as_ptr() as *const libc::c_void, message.len());
        libc::write(2, b"\n".as_ptr() as *const libc::c_void, 1);
    }
}

macro_rules! eprintln {
    ($($arg:tt)*) => {{
        let message = alloc::format!($($arg)*);
        linux_log(&message);
    }};
}

use core::sync::atomic::{AtomicBool, Ordering};

static LINUX_GPU_ENABLED: AtomicBool = AtomicBool::new(true);

pub fn set_gpu_enabled(enabled: bool) {
    LINUX_GPU_ENABLED.store(enabled, Ordering::Relaxed);
    eprintln!(
        "desktop: gpu presentation {}",
        if enabled { "on (DRM dirty rectangles)" } else { "off (CPU full-frame)" }
    );
}

pub fn gpu_enabled() -> bool {
    LINUX_GPU_ENABLED.load(Ordering::Relaxed)
}

#[cfg(feature = "linux")]
pub struct Screen {
    info: FramebufferInfo,
    layer_buffer: Option<Vec<u32>>,
    fb_ptr: *mut u8,
    fb_len: usize,
    drm_fd: i32,
    drm_fb_id: u32,
    dirty: Option<(usize, usize, usize, usize)>,
}

#[cfg(feature = "linux")]
unsafe impl Send for Screen {}
#[cfg(feature = "linux")]
unsafe impl Sync for Screen {}

#[cfg(feature = "linux")]
impl Screen {
    pub fn take() -> Result<Screen, ()> {
        let fd = unsafe { libc::open(b"/dev/fb0\0".as_ptr() as *const libc::c_char, libc::O_RDWR) };
        if fd < 0 {
            eprintln!("desktop: open /dev/fb0 failed");
            return Err(());
        }
        let mut var = FbVarScreeninfo::default();
        let mut fix = FbFixScreeninfo::default();
        if unsafe { libc::ioctl(fd, FBIOGET_VSCREENINFO, &mut var) } < 0
            || unsafe { libc::ioctl(fd, FBIOGET_FSCREENINFO, &mut fix) } < 0
        {
            eprintln!("desktop: framebuffer ioctl failed");
            unsafe { libc::close(fd) };
            return Err(());
        }
        if var.bits_per_pixel != 32 {
            eprintln!(
                "desktop: unsupported framebuffer depth {} bpp",
                var.bits_per_pixel
            );
            unsafe { libc::close(fd) };
            return Err(());
        }
        let width = var.xres as usize;
        let height = var.yres as usize;
        let stride = if (fix.line_length as usize) >= width * 4 {
            (fix.line_length as usize) / 4
        } else {
            width
        };
        let fb_len = if fix.smem_len as usize >= stride * height * 4 {
            fix.smem_len as usize
        } else {
            stride * height * 4
        };
        let mapping = unsafe {
            libc::mmap(
                core::ptr::null_mut(),
                fb_len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        if mapping == libc::MAP_FAILED {
            eprintln!("desktop: mmap /dev/fb0 failed");
            unsafe { libc::close(fd) };
            return Err(());
        }
        let pixel_format = if var.red.offset == 16 && var.blue.offset == 0 {
            PixelFormat::Bgr
        } else {
            PixelFormat::Rgb
        };
        let (drm_fd, drm_fb_id) = Self::open_drm();
        eprintln!(
            "desktop: framebuffer {}x{} stride={} {}bpp fix_line={} smem={} drm_fd={} fb={}",
            width,
            height,
            stride,
            var.bits_per_pixel,
            fix.line_length,
            fix.smem_len,
            drm_fd,
            drm_fb_id
        );
        Ok(Screen {
            info: FramebufferInfo {
                base: mapping as usize,
                size: fb_len,
                width,
                height,
                stride,
                pixel_format,
            },
            layer_buffer: None,
            fb_ptr: mapping as *mut u8,
            fb_len,
            drm_fd,
            drm_fb_id,
            dirty: None,
        })
    }

    pub fn take_with_target(_target_w: usize, _target_h: usize) -> Result<Screen, ()> {
        Self::take()
    }

    fn open_drm() -> (i32, u32) {
        let fd = unsafe {
            libc::open(
                b"/dev/dri/card0\0".as_ptr() as *const libc::c_char,
                libc::O_RDWR | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            return (-1, 0);
        }
        let mut crtc_ids = [0u32; 16];
        let mut fb_ids = [0u32; 16];
        let mut res = DrmModeCardRes::default();
        res.fb_id_ptr = fb_ids.as_mut_ptr() as u64;
        res.count_fbs = fb_ids.len() as u32;
        res.crtc_id_ptr = crtc_ids.as_mut_ptr() as u64;
        res.count_crtcs = crtc_ids.len() as u32;
        if unsafe { libc::ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &mut res) } < 0 {
            unsafe { libc::close(fd) };
            return (-1, 0);
        }
        let crtc_count = (res.count_crtcs as usize).min(crtc_ids.len());
        for crtc_id in crtc_ids.into_iter().take(crtc_count) {
            if crtc_id == 0 {
                continue;
            }
            let mut crtc = DrmModeCrtc::default();
            crtc.crtc_id = crtc_id;
            if unsafe { libc::ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &mut crtc) } == 0
                && crtc.fb_id != 0
                && crtc.mode_valid != 0
            {
                return (fd, crtc.fb_id);
            }
        }
        unsafe { libc::close(fd) };
        (-1, 0)
    }

    fn mark_dirty(&mut self, x: usize, y: usize, w: usize, h: usize) {
        if w == 0 || h == 0 {
            return;
        }
        if !gpu_enabled() {
            self.dirty = Some((0, 0, self.info.width, self.info.height));
            return;
        }
        let x1 = (x + w).min(self.info.width);
        let y1 = (y + h).min(self.info.height);
        self.dirty = Some(match self.dirty {
            Some((x0, y0, ox1, oy1)) => (x.min(x0), y.min(y0), x1.max(ox1), y1.max(oy1)),
            None => (x, y, x1, y1),
        });
    }

    pub fn present(&mut self) {
        let Some((x0, y0, x1, y1)) = self.dirty.take() else {
            return;
        };
        if self.drm_fd < 0 || self.drm_fb_id == 0 {
            return;
        }
        if x1 <= x0 || y1 <= y0 {
            return;
        }
        let clip = DrmClipRect {
            x1: x0.min(u16::MAX as usize) as u16,
            y1: y0.min(u16::MAX as usize) as u16,
            x2: x1.min(u16::MAX as usize) as u16,
            y2: y1.min(u16::MAX as usize) as u16,
        };
        let mut cmd = DrmModeFbDirtyCmd {
            fb_id: self.drm_fb_id,
            flags: 0,
            color: 0,
            num_clips: 1,
            clips_ptr: &clip as *const DrmClipRect as u64,
        };
        if unsafe { libc::ioctl(self.drm_fd, DRM_IOCTL_MODE_DIRTYFB, &mut cmd) } < 0 {
            eprintln!("desktop: DRM DIRTYFB failed; display may not refresh");
        }
    }

    pub fn flush_layer_row(&mut self, y: usize, row: &[u32]) {
        self.flush_layer_row_range(y, 0, row);
    }

    pub fn flush_layer_row_range(&mut self, y: usize, x_offset: usize, row: &[u32]) {
        if y >= self.info.height || x_offset >= self.info.width {
            return;
        }
        let n = row.len().min(self.info.width - x_offset);
        if n == 0 {
            return;
        }
        let stride = self.info.stride;
        let base = self.fb_ptr as *mut u32;
        let offset = y * stride + x_offset;
        match self.info.pixel_format {
            PixelFormat::Rgb => unsafe {
                for index in 0..n {
                    let c = row[index];
                    let swapped = ((c & 0xFF) << 16) | (c & 0x00FF00) | ((c >> 16) & 0xFF);
                    base.add(offset + index).write(swapped);
                }
            },
            _ => unsafe {
                core::ptr::copy_nonoverlapping(row.as_ptr(), base.add(offset), n);
            },
        }
        self.mark_dirty(x_offset, y, n, 1);
    }
}

#[cfg(feature = "linux")]
impl Drop for Screen {
    fn drop(&mut self) {
        unsafe {
            if !self.fb_ptr.is_null() {
                libc::munmap(self.fb_ptr as *mut libc::c_void, self.fb_len);
            }
            if self.drm_fd >= 0 {
                libc::close(self.drm_fd);
            }
        }
    }
}
''')

# LayerSystem owns the only place that knows a complete damage update was
# copied.  Ask the screen to present exactly once per flush.
shapes = root / "crates/baram-core/src/layer_shapes.rs"
text = shapes.read_text()
text = text.replace('''        }\n        self.frame_count += 1;\n    }\n\n    pub fn flush_rect''',
'''        }\n        #[cfg(feature = "linux")]\n        screen.present();\n        self.frame_count += 1;\n    }\n\n    pub fn flush_rect''')
text = text.replace('''        let _ = self.take_dirty();\n        self.frame_count += 1;''',
'''        let _ = self.take_dirty();\n        #[cfg(feature = "linux")]\n        screen.present();\n        self.frame_count += 1;''')
shapes.write_text(text)

# baram-font keeps the BDF path linked even when the TTF path is selected.
bdf = root / "crates/baram-font/src/bdf_font.rs"
text = bdf.read_text()
if "LINUX_HOST_BDF_STUBS" not in text:
    text += r'''

// LINUX_HOST_BDF_STUBS: the desktop initialises the TTF font, never the BDF
// source, so these readers are unreachable on Linux.
#[cfg(not(any(feature = "uefi", feature = "esp32s3")))]
fn with_embedded_glyph<F>(_data: &'static [u8], _ch: char, _draw: F) -> bool
where
    F: FnMut(&[u8], i32, i32, i32, i32) -> bool,
{
    false
}

#[cfg(not(any(feature = "uefi", feature = "esp32s3")))]
fn with_file_glyph<F>(_path: &str, _ch: char, _draw: F) -> bool
where
    F: FnMut(&[u8], i32, i32, i32, i32) -> bool,
{
    false
}
'''
    bdf.write_text(text)

# ----------------------------------------------------------- boot runtime
runner_main = root / "crates/baram-boot/src/main.rs"
main_text = runner_main.read_text()
main_text = main_text.replace('#![no_std]\n#![no_main]\n\n', '')
# The shim has no firmware entry point.  Run the upstream main loop from a
# normal hosted main function instead.
main_text += '''
fn main() {
    let nano = nano_system::NanoSystem::start();
    let _ = baram_kernel_main(nano);
}
'''
runner_main.write_text(main_text)
runtime = root / "crates/baram-boot/src/runtime.rs"
rt_lines = runtime.read_text().split("\n")
rt_lines = [ln for ln in rt_lines if "baram_kern::" not in ln]
runtime.write_text("\n".join(rt_lines))

runtime_input = root / "crates/baram-boot/src/runtime_input.rs"
ri_text = runtime_input.read_text()
needle = "        match baram_bsd::uri::check_system_commands(&mut display_state) {"
replacement = """        // URI command inbox.  Other guest processes (and the init script's
        // `zh_uri=` kernel parameter) can request a settings change without
        // needing a second compositor connection.
        if let Ok(uri) = std::fs::read_to_string("/tmp/zhibox.uri") {
            let _ = std::fs::remove_file("/tmp/zhibox.uri");
            let uri = uri.trim();
            if uri.starts_with("os://") && baram_bsd::uri::execute(uri, &mut display_state) {
                cached_wallpaper = wallpaper_for_state(
                    &display_state,
                    screen.width(),
                    screen.height(),
                );
                wm.set_all_dirty();
                taskbar_surface.invalidate();
                cached_launcher_layer = None;
                bg_cache = None;
                scene_dirty = true;
                dirty = true;
            }
        }

        match baram_bsd::uri::check_system_commands(&mut display_state) {"""
if needle in ri_text:
    runtime_input.write_text(ri_text.replace(needle, replacement, 1))
else:
    raise SystemExit("desktop: could not patch URI inbox")

# GPU ON uses DRM dirty rectangles. GPU OFF keeps the wallpaper, skips backdrop
# blur in the vendored compositor, and asks the CPU-rendered framebuffer to
# refresh the whole screen.

# Refresh the cached wallpaper right after a GPU toggle arrives through the
# application URI paths (Warp4/HTML app commands).
import re
gpu_branch_tpl = (
    '{i}} else if parsed.path == "display/gpu" {\n'
    '{i}    cached_wallpaper = wallpaper_for_state(&display_state, screen.width(), screen.height());\n'
    '{i}    wm.set_all_dirty();\n'
    '{i}    taskbar_surface.invalidate();\n'
    '{i}    cached_launcher_layer = None;\n'
    '{i}    bg_cache = None;\n'
    '{i}    scene_dirty = true;\n'
    '{i}    dirty = true;\n'
)

for source, pattern in (
    ("crates/baram-boot/src/runtime_input.rs",
     re.compile(r'(\n)([ ]*)\} else if parsed\n[ ]*\.path\n[ ]*\.starts_with\("display/pointer"\)')),
    ("crates/baram-boot/src/runtime_pointer.rs",
     re.compile(r'(\n)([ ]*)\} else if parsed\.path\.starts_with\("display/pointer"\)')),
):
    path = root / source
    text = path.read_text()
    if 'path == "display/gpu"' in text:
        continue
    def insert(m, tpl=gpu_branch_tpl):
        i = m.group(2)
        return m.group(1) + i + tpl.replace("{i}", i) + m.group(0)[1:]
    new_text, n = pattern.subn(insert, text, count=1)
    if n != 1:
        raise SystemExit("desktop: could not insert GPU refresh branch in " + source)
    path.write_text(new_text)

PY

say "building $TARGET"
(
	cd "$BUILD"
	RUSTFLAGS='-C linker=rust-lld -C link-self-contained=yes' \
		cargo build --release --target "$TARGET" -p zhibox-desktop
)

mkdir -p "$DIST"
cp "$BUILD/target/$TARGET/release/zhibox-desktop" "$DIST/zhibox-desktop"
chmod 755 "$DIST/zhibox-desktop"
cp "$ROOT/scripts/guest/gradient/baram-bsd/config.xml" "$DIST/config.xml"

python3 - "$BUILD" "$DIST" <<'PY'
from pathlib import Path
import shutil
import sys
import tarfile

baram = Path(sys.argv[1])
dist = Path(sys.argv[2])
src = baram / "files/app"
dest = dist / "apps"
dest.mkdir(parents=True, exist_ok=True)

for path in sorted(src.iterdir()):
    target = dest / path.name
    if path.is_dir():
        if path.name == "icon":
            shutil.copytree(path, target)
            print(f"desktop: copied icon set")
            continue
        with tarfile.open(target, "w", format=tarfile.USTAR_FORMAT) as tar:
            for member in sorted(path.rglob("*")):
                if member.is_file():
                    tar.add(member, arcname=str(member.relative_to(path)))
        print(f"desktop: packaged {target.name} ({target.stat().st_size} bytes)")
    elif path.is_file():
        shutil.copy2(path, target)

print(f"desktop: assets in {dest}")
PY

say "binary: $DIST/zhibox-desktop"
file "$DIST/zhibox-desktop" 2>/dev/null || true
