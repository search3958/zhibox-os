#!/bin/sh
# Build the native Warp 4 runner for the zhibox-os guest.
#
# The Warp 4 engine lives in the sibling baram-os tree.  This script
# assembles a temporary Cargo workspace from those crates, swaps in the small
# Linux `baram-bsd` shim, applies the two Linux-only patches to baram-core,
# and cross-compiles a static x86_64 musl binary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BARAM_OS=${BARAM_OS:-"$ROOT/../baram-os"}
OUT=${OUT:-"$ROOT/output"}
BUILD="$OUT/build/warp4-src"
DIST="$OUT/warp4"
TARGET=x86_64-unknown-linux-musl
APPS=${WARP4_APPS:-calc.w4a}

say() {
	printf 'warp4: %s\n' "$*"
}

if [ ! -d "$BARAM_OS/crates/baram-warp4" ]; then
	echo "warp4: baram-os not found at $BARAM_OS" >&2
	echo "warp4: set BARAM_OS=/path/to/baram-os" >&2
	exit 1
fi

if ! rustup target list --installed | grep -qx "$TARGET"; then
	echo "warp4: installing Rust target $TARGET" >&2
	rustup target add "$TARGET"
fi

say "assembling $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD/crates" "$BUILD/files/data/fonts" "$BUILD/files/data/ui" \
	"$BUILD/apps" "$DIST/apps"

for crate in baram-warp4 baram-core baram-font baram-graphics stb_truetype_rust; do
	cp -R "$BARAM_OS/crates/$crate" "$BUILD/crates/"
done
cp -R "$ROOT/scripts/guest/warp4/baram-bsd" "$BUILD/crates/"
cp -R "$ROOT/scripts/guest/warp4/runner" "$BUILD/"

cp "$BARAM_OS/files/data/fonts/HarmonyOS_Sans_SC_Regular.ttf" "$BUILD/files/data/fonts/"
cp "$BARAM_OS/files/data/fonts/GothicA1-Medium.ttf" "$BUILD/files/data/fonts/"
cp "$BARAM_OS/files/data/fonts/GoogleSansFlex_24pt-Medium.ttf" "$BUILD/files/data/fonts/"
cp "$BARAM_OS/files/data/ui/check-icon.svg" "$BUILD/files/data/ui/"

cat >"$BUILD/Cargo.toml" <<'EOF'
[workspace]
resolver = "2"
members = [
    "crates/baram-core",
    "crates/baram-font",
    "crates/baram-graphics",
    "crates/baram-warp4",
    "crates/baram-bsd",
    "crates/stb_truetype_rust",
    "runner",
]

[workspace.dependencies]
uefi = { version = "0.38", features = ["alloc", "logger", "global_allocator"] }
uefi-raw = { version = "0.15" }
libm = "0.2"
kurbo = { version = "0.13", default-features = false, features = ["libm"] }
esp-hal = { version = "1.2", default-features = false }
embedded-hal = "1"
EOF

python3 - "$BUILD" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

# baram-core is `no_std` and only knows UEFI/ESP32 screens upstream.  Add a
# tiny Linux screen type for the font module and layer helpers to compile.
core_toml = root / "crates/baram-core/Cargo.toml"
text = core_toml.read_text()
if "linux = []" not in text:
    text = text.replace('[features]\ndefault = ["uefi"]',
                        '[features]\ndefault = ["uefi"]\nlinux = []')
    core_toml.write_text(text)

# The upstream manifests use the workspace default feature (UEFI) for
# baram-core.  The host build must not pull UEFI definitions in.
for crate in ("baram-warp4", "baram-graphics"):
    manifest = root / f"crates/{crate}/Cargo.toml"
    text = manifest.read_text()
    text = text.replace('baram-core = { path = "../baram-core" }',
                        'baram-core = { path = "../baram-core", default-features = false }')
    manifest.write_text(text)

# The script runtime asks the platform for wall-clock time.  UEFI provides
# that through `uefi::runtime`; the Linux host uses libc's localtime.
manifest = root / "crates/baram-warp4/Cargo.toml"
text = manifest.read_text()
if 'libc = "0.2"' not in text:
    text = text.replace("[dependencies]\n", "[dependencies]\nlibc = \"0.2\"\n", 1)
    manifest.write_text(text)

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
    raise SystemExit("warp4: could not patch script runtime clock")

# The MP Services dispatcher is UEFI-only.  The Linux build only needs the
# sequential fallback used by the graphics blur.
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

screen = root / "crates/baram-core/src/screen.rs"
text = screen.read_text()
if "LINUX_HOST_SCREEN" not in text:
    text += r'''

// LINUX_HOST_SCREEN: the zhibox host runner does not own a framebuffer.  The
// common Screen methods below still compile against this storage; they are
// simply never called by the raw-surface runner.
#[cfg(feature = "linux")]
pub struct Screen {
    info: FramebufferInfo,
    layer_buffer: Option<Vec<u32>>,
    fb_ptr: *mut u8,
}

#[cfg(feature = "linux")]
impl Screen {
    pub fn new(width: usize, height: usize) -> Self {
        Self {
            info: FramebufferInfo {
                base: 0,
                size: 0,
                width,
                height,
                stride: width,
                pixel_format: PixelFormat::Bgr,
            },
            layer_buffer: None,
            fb_ptr: core::ptr::null_mut(),
        }
    }

    pub fn flush_layer_row(&mut self, y: usize, row: &[u32]) {
        self.flush_layer_row_range(y, 0, row);
    }

    pub fn flush_layer_row_range(&mut self, y: usize, x_offset: usize, row: &[u32]) {
        if y >= self.info.height || x_offset >= self.info.width {
            return;
        }
        let width = self.info.width;
        let len = width.saturating_mul(self.info.height);
        let buffer = self
            .layer_buffer
            .get_or_insert_with(|| alloc::vec![Color::BG.0; len]);
        let n = row.len().min(width - x_offset);
        let start = y * width + x_offset;
        buffer[start..start + n].copy_from_slice(&row[..n]);
    }
}
'''
    screen.write_text(text)

# baram-font keeps the BDF path linked even when the TTF path is selected.
# Its glyph readers are UEFI-only, so provide inert stubs for the host build.
bdf = root / "crates/baram-font/src/bdf_font.rs"
text = bdf.read_text()
if "LINUX_HOST_BDF_STUBS" not in text:
    text += r'''

// LINUX_HOST_BDF_STUBS: the host runner initialises the TTF font, never the
// BDF source, so these readers are unreachable on Linux.
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
PY

say "building $TARGET"
(
	cd "$BUILD"
	RUSTFLAGS='-C linker=rust-lld -C link-self-contained=yes' \
		cargo build --release --target "$TARGET" -p zhibox-warp4
)

mkdir -p "$DIST"
cp "$BUILD/target/$TARGET/release/zhibox-warp4" "$DIST/zhibox-warp4"
chmod 755 "$DIST/zhibox-warp4"

python3 - "$BARAM_OS" "$DIST/apps" $APPS <<'PY'
from pathlib import Path
import shutil
import sys
import tarfile

apps_root = Path(sys.argv[1]) / "files/app"
dest = Path(sys.argv[2])
dest.mkdir(parents=True, exist_ok=True)
for name in sys.argv[3:]:
    src = apps_root / name
    if not src.is_dir():
        print(f"warp4: skipping missing app {name}", file=sys.stderr)
        continue
    out = dest / name
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(src.rglob("*")):
            if path.is_file():
                tar.add(path, arcname=str(path.relative_to(src)))
    print(f"warp4: packaged {out} ({out.stat().st_size} bytes)")
PY

say "runner: $DIST/zhibox-warp4"
file "$DIST/zhibox-warp4" 2>/dev/null || true
