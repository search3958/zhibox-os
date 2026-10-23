//! Small configuration provider for the Linux Warp 4 host.
//!
//! BaramOS stores its settings as nested XML and addresses values with a
//! slash-separated path such as `ui-theme/window/title_bar_h`.  The Warp 4
//! runtime only needs a handful of those values, so this port implements the
//! same path lookup with a compact XML flattener instead of the full UEFI
//! configuration writer.

use std::collections::HashMap;
use std::sync::OnceLock;

use crate::vfs;

pub struct Config {
    values: HashMap<String, String>,
}

impl Config {
    pub fn get(&self, path: &str) -> Option<&str> {
        self.values.get(path).map(String::as_str)
    }
}

static CONFIG: OnceLock<Config> = OnceLock::new();

pub fn init_config() {
    let xml = vfs::read_file_str("EFI/BOOT/config.xml");
    let values = if xml.trim().is_empty() {
        parse_xml(DEFAULT_CONFIG_XML)
    } else {
        parse_xml(&xml)
    };
    let _ = CONFIG.set(Config { values });
}

pub fn get_config() -> &'static Config {
    CONFIG.get_or_init(|| Config {
        values: parse_xml(DEFAULT_CONFIG_XML),
    })
}

pub fn get_usize(path: &str, default: usize) -> usize {
    get_config()
        .get(path)
        .and_then(|value| value.trim().parse().ok())
        .unwrap_or(default)
}

pub fn get_i32(path: &str, default: i32) -> i32 {
    get_config()
        .get(path)
        .and_then(|value| value.trim().parse().ok())
        .unwrap_or(default)
}

pub fn get_bool(path: &str, default: bool) -> bool {
    get_config()
        .get(path)
        .map(|value| {
            let value = value.trim();
            value == "1" || value.eq_ignore_ascii_case("true") || value.eq_ignore_ascii_case("on")
        })
        .unwrap_or(default)
}

pub fn timezone_offset_minutes() -> i32 {
    let Some(value) = get_config().get("timezone") else {
        return 9 * 60;
    };
    let value = value.trim();
    let (sign, rest) = match value.as_bytes().first() {
        Some(b'+') => (1, &value[1..]),
        Some(b'-') => (-1, &value[1..]),
        _ => (1, value),
    };
    let mut parts = rest.split(':');
    let hours: i32 = parts.next().and_then(|v| v.parse().ok()).unwrap_or(9);
    let minutes: i32 = parts.next().and_then(|v| v.parse().ok()).unwrap_or(0);
    sign * (hours * 60 + minutes)
}

fn parse_xml(xml: &str) -> HashMap<String, String> {
    let mut values = HashMap::new();
    let mut stack: Vec<String> = Vec::new();
    let bytes = xml.as_bytes();
    let mut cursor = 0usize;

    while cursor < bytes.len() {
        let Some(open_rel) = xml[cursor..].find('<') else {
            break;
        };
        let open = cursor + open_rel;
        if open > cursor {
            let text = xml[cursor..open].trim();
            if !text.is_empty() && !stack.is_empty() {
                values.insert(stack.join("/"), unescape(text));
            }
        }
        let Some(close_rel) = xml[open..].find('>') else {
            break;
        };
        let close = open + close_rel;
        let inside = xml[open + 1..close].trim();
        cursor = close + 1;

        if inside.starts_with('!') || inside.starts_with('?') {
            continue;
        }
        if let Some(name) = inside.strip_prefix('/') {
            let name = name.trim();
            if let Some(pos) = stack.iter().rposition(|tag| tag == name) {
                stack.truncate(pos);
            }
            continue;
        }
        if inside.is_empty() {
            continue;
        }
        let self_closing = inside.ends_with('/');
        let tag = inside
            .trim_end_matches('/')
            .split_ascii_whitespace()
            .next()
            .unwrap_or("");
        if tag.is_empty() {
            continue;
        }
        if self_closing {
            continue;
        }
        stack.push(String::from(tag));
    }

    values
}

fn unescape(text: &str) -> String {
    text.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
}

const DEFAULT_CONFIG_XML: &str = include_str!("../config.xml");
