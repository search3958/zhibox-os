//! In-memory Warp 4 application archive reader for the Linux host shim.
//!
//! A `.w4a` app is an ordinary TAR archive.  The upstream implementation in
//! `baram-bsd` keeps the whole archive in memory and reads members from it;
//! this port does the same while loading the archive from a regular directory.

use crate::vfs;

pub const FALLBACK_INDEX: &str = "";

pub fn read_index_yaml() -> String {
    let content = vfs::read_file_str("apps/index.yaml");
    if content.is_empty() {
        String::from(FALLBACK_INDEX)
    } else {
        content
    }
}

pub fn load_app_source(name: &str) -> String {
    vfs::read_file_str(&format!("apps/{name}"))
}

/// An in-memory Warp 4 application archive.
pub struct Warp4Archive {
    app_name: String,
    data: Vec<u8>,
    embedded: Option<Vec<(String, String)>>,
}

impl Warp4Archive {
    pub fn open(app_name: &str) -> Self {
        let path = format!("apps/{app_name}");
        let alias = if let Some(stem) = app_name.strip_suffix(".w4a") {
            Some(format!("apps/{stem}.s4a"))
        } else if let Some(stem) = app_name.strip_suffix(".s4a") {
            Some(format!("apps/{stem}.w4a"))
        } else {
            None
        };
        let paths: Vec<&str> = match alias.as_deref() {
            Some(alias) => vec![path.as_str(), alias],
            None => vec![path.as_str()],
        };
        Self {
            app_name: String::from(app_name),
            data: vfs::read_file_candidates(&paths),
            embedded: None,
        }
    }

    pub fn from_embedded(name: &str, sources: &[(&str, &str)]) -> Self {
        Self {
            app_name: String::from(name),
            data: Vec::new(),
            embedded: Some(
                sources
                    .iter()
                    .map(|(name, source)| (String::from(*name), String::from(*source)))
                    .collect(),
            ),
        }
    }

    pub fn app_name(&self) -> &str {
        &self.app_name
    }

    pub fn read_text(&self, member_name: &str) -> String {
        if !is_safe_archive_member(member_name) {
            return String::new();
        }
        let wanted = member_name.trim_start_matches("./");
        if let Some(sources) = &self.embedded {
            return sources
                .iter()
                .find(|(name, _)| name.trim_start_matches("./") == wanted)
                .map(|(_, source)| source.clone())
                .unwrap_or_default();
        }
        let Some(bytes) = self.read(member_name) else {
            return String::new();
        };
        String::from_utf8(bytes.to_vec()).unwrap_or_default()
    }

    fn read(&self, member_name: &str) -> Option<&[u8]> {
        if !is_safe_archive_member(member_name) {
            return None;
        }
        let wanted = member_name.trim_start_matches("./");
        let mut offset = 0usize;
        while offset.checked_add(512)? <= self.data.len() {
            let header = &self.data[offset..offset + 512];
            if header.iter().all(|byte| *byte == 0) {
                return None;
            }
            let name = tar_string(&header[0..100]);
            let prefix = tar_string(&header[345..500]);
            let full_name = if prefix.is_empty() {
                name
            } else {
                format!("{prefix}/{name}")
            };
            let size = tar_octal(&header[124..136])?;
            let data_start = offset + 512;
            let data_end = data_start.checked_add(size)?;
            if data_end > self.data.len() {
                return None;
            }
            if full_name.trim_start_matches("./") == wanted {
                return Some(&self.data[data_start..data_end]);
            }
            let padded = size.checked_add(511)? / 512 * 512;
            offset = data_start.checked_add(padded)?;
        }
        None
    }
}

fn is_safe_archive_member(name: &str) -> bool {
    !name.is_empty()
        && !name.starts_with('/')
        && !name.split('/').any(|part| part == "..")
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b'/'))
}

fn tar_string(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).trim().into()
}

fn tar_octal(bytes: &[u8]) -> Option<usize> {
    let mut value = 0usize;
    let mut found = false;
    for byte in bytes {
        match byte {
            b'0'..=b'7' => {
                value = value.checked_mul(8)?.checked_add((byte - b'0') as usize)?;
                found = true;
            }
            0 | b' ' => {}
            _ => return None,
        }
    }
    found.then_some(value)
}

pub fn parse_app_uri(uri: &str) -> Option<&str> {
    let name = uri.trim().strip_prefix("app://")?;
    let name = name.split(['?', '#']).next().unwrap_or(name);
    if is_safe_app_filename(name)
        && (name.ends_with(".warp")
            || name.ends_with(".html")
            || name.ends_with(".htm")
            || name.ends_with(".w3a")
            || name.ends_with(".w4a")
            || name.ends_with(".s4a")
            || name.ends_with(".u1"))
    {
        Some(name)
    } else {
        None
    }
}

pub fn is_safe_app_filename(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 128
        && !name.starts_with('.')
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
}
