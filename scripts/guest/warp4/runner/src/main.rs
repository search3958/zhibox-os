//! Native Warp 4 host runner for zhibox-os.
//!
//! The runner owns a Warp 4 archive, renders into `baram-core`'s software
//! layer, and publishes the result in the same raw RGBA format used by the
//! WebKit renderer.  `fb-gradient` displays the surface in a window and
//! forwards pointer coordinates through a tiny text file, so the runner does
//! not need to compete with the desktop for `/dev/fb0` or evdev.

use std::fs;
use std::io::Write;
use std::path::Path;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use baram_core::LayerSystem;
use baram_warp4::{set_ui_mode, set_ui_scale_percent, UiMode, Warp4Engine};

const MAGIC: &[u8; 8] = b"ZHRAW1\0\0";
const DEFAULT_RAW: &str = "/tmp/warp4.raw";
const DEFAULT_VERSION: &str = "/tmp/warp4.version";
const DEFAULT_INPUT: &str = "/tmp/warp4.input";

fn env_or(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| String::from(default))
}

fn env_u32(name: &str, default: u32) -> u32 {
    std::env::var(name)
        .ok()
        .and_then(|value| value.trim().parse().ok())
        .unwrap_or(default)
        .clamp(16, 4096)
}

fn now_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos().min(u64::MAX as u128) as u64)
        .unwrap_or(0)
}

fn read_pointer(path: &str) -> Option<(i32, i32, i32)> {
    let text = fs::read_to_string(path).ok()?;
    let line = text.lines().rev().find(|line| !line.trim().is_empty())?;
    let mut fields = line.split_whitespace();
    let x = fields.next()?.parse().ok()?;
    let y = fields.next()?.parse().ok()?;
    let button = fields
        .next()
        .and_then(|value| value.parse().ok())
        .unwrap_or(0);
    Some((x, y, button))
}

fn write_atomic(path: &str, data: &[u8]) -> std::io::Result<()> {
    let path = Path::new(path);
    let tmp = path.with_extension(format!("tmp.{}", std::process::id()));
    {
        let mut file = fs::File::create(&tmp)?;
        file.write_all(data)?;
        file.flush()?;
    }
    fs::rename(&tmp, path)
}

fn write_surface(path: &str, layer: &LayerSystem) -> std::io::Result<()> {
    let width = layer.width() as u32;
    let height = layer.height() as u32;
    let stride = width.saturating_mul(4);
    let mut out = Vec::with_capacity(24 + stride as usize * height as usize);
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&stride.to_le_bytes());
    for &pixel in layer.buf_ref() {
        out.push(((pixel >> 16) & 0xFF) as u8);
        out.push(((pixel >> 8) & 0xFF) as u8);
        out.push((pixel & 0xFF) as u8);
        out.push(((pixel >> 24) & 0xFF) as u8);
    }
    write_atomic(path, &out)
}

fn write_version(path: &str, version: u64) -> std::io::Result<()> {
    // fb-gradient reads the same little-endian u32 frame counter as the
    // WebKit renderer.  Keep the on-disk contract identical.
    write_atomic(path, &(version as u32).to_le_bytes())
}

fn open_app(name: &str) -> Warp4Engine {
    let mut engine = Warp4Engine::new(name);
    // The gradient window already has a title bar.  Expose the app's own
    // content without a second (BaramOS window-manager) title bar.
    engine.set_chrome_visible(false);
    engine
}

fn main() {
    if std::env::var_os("BARAM_ROOT").is_none() {
        std::env::set_var("BARAM_ROOT", "/usr/share/zhibox");
    }
    baram_bsd::config::init_config();
    set_ui_mode(UiMode::Normal);
    set_ui_scale_percent(100);
    baram_font::ttf_font::init();

    let app = std::env::args()
        .nth(1)
        .unwrap_or_else(|| String::from("calc.w4a"));
    let width = env_u32("ZH_WARP4_WIDTH", 560);
    let height = env_u32("ZH_WARP4_HEIGHT", 400);
    let raw_path = env_or("ZH_WARP4_RAW", DEFAULT_RAW);
    let version_path = env_or("ZH_WARP4_VERSION", DEFAULT_VERSION);
    let input_path = env_or("ZH_WARP4_INPUT", DEFAULT_INPUT);
    let once = std::env::var_os("ZH_WARP4_ONCE").is_some();

    let mut engine = open_app(&app);
    let mut layer = LayerSystem::new(width as usize, height as usize);
    let mut version = 0u64;
    let mut prev_input: Option<(i32, i32, i32)> = None;
    let mut needs_render = true;

    eprintln!(
        "warp4: app={} {}x{} root={}",
        app,
        width,
        height,
        std::env::var("BARAM_ROOT").unwrap_or_default()
    );

    loop {
        let mut changed = engine.tick(now_ns());

        if let Some(input) = read_pointer(&input_path) {
            if prev_input != Some(input) {
                let (x, y, button) = input;
                let previous_button = prev_input.map(|(_, _, button)| button).unwrap_or(0);
                if x < 0 || y < 0 {
                    engine.set_hover(-10000, -10000);
                    if previous_button != 0 {
                        engine.release();
                    }
                } else {
                    if engine.set_hover_changed(x, y) {
                        changed = true;
                    }
                    if button != 0 {
                        engine.pointer_move(x, y);
                    }
                    if button == 1 && previous_button == 0 {
                        engine.click(x, y);
                        needs_render = true;
                    } else if button == 0 && previous_button != 0 {
                        engine.release();
                    }
                    if button != previous_button {
                        needs_render = true;
                    }
                }
                prev_input = Some(input);
            }
        }

        if changed {
            needs_render = true;
        }

        if needs_render {
            engine.draw_to_layer(&mut layer, 0, 0);
            if let Err(error) = write_surface(&raw_path, &layer) {
                eprintln!("warp4: cannot publish frame: {error}");
                break;
            }
            version = version.wrapping_add(1);
            if let Err(error) = write_version(&version_path, version) {
                eprintln!("warp4: cannot publish version: {error}");
                break;
            }
            needs_render = false;
        }

        if let Some(command) = engine.take_command() {
            if let Some(name) = baram_bsd::app::parse_app_uri(&command) {
                eprintln!("warp4: opening {name}");
                engine = open_app(name);
                needs_render = true;
            }
        }

        if once || Path::new("/tmp/warp4.stop").exists() {
            break;
        }
        std::thread::sleep(Duration::from_millis(16));
    }
}
