use std::fs;
use std::path::{Component, Path, PathBuf};

fn root() -> PathBuf {
    std::env::var_os("BARAM_ROOT")
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| PathBuf::from("/usr/share/zhibox"))
}

fn safe_relative(path: &str) -> Option<PathBuf> {
    let path = path.trim_start_matches('/').trim_end_matches('/');
    if path.is_empty() {
        return Some(PathBuf::new());
    }
    let mut out = PathBuf::new();
    for component in Path::new(path).components() {
        match component {
            Component::Normal(part) => out.push(part),
            _ => return None,
        }
    }
    Some(out)
}

/// Map the BaramOS VFS namespace onto the directory used by this runner.
pub fn map_path(path: &str) -> Option<PathBuf> {
    let path = path.trim_start_matches('/').trim_end_matches('/');
    let rel = if path == "EFI/BOOT/config.xml" {
        PathBuf::from("config.xml")
    } else if let Some(rest) = path.strip_prefix("apps/") {
        PathBuf::from("apps").join(safe_relative(rest)?)
    } else if path == "apps" {
        PathBuf::from("apps")
    } else if let Some(rest) = path.strip_prefix("files/") {
        PathBuf::from("files").join(safe_relative(rest)?)
    } else if path == "files" {
        PathBuf::from("files")
    } else if let Some(rest) = path
        .strip_prefix("app/")
        .or_else(|| path.strip_prefix("data/"))
    {
        PathBuf::from("files").join(safe_relative(rest)?)
    } else {
        safe_relative(path)?
    };
    Some(root().join(rel))
}

pub fn read_file(path: &str) -> Vec<u8> {
    let Some(path) = map_path(path) else {
        return Vec::new();
    };
    fs::read(path).unwrap_or_default()
}

pub fn read_file_candidates(paths: &[&str]) -> Vec<u8> {
    for path in paths {
        let data = read_file(path);
        if !data.is_empty() {
            return data;
        }
    }
    Vec::new()
}

pub fn read_file_str(path: &str) -> String {
    String::from_utf8(read_file(path)).unwrap_or_default()
}

/// Convert a `files://` URI into the VFS namespace used by BaramOS.
pub fn parse_files_uri(uri: &str) -> Option<String> {
    let path = uri.trim().strip_prefix("files://")?.trim_start_matches('/');
    let path = path.trim_end_matches('/');
    if path.is_empty() {
        return Some("files/".into());
    }
    let mut depth = 0i32;
    for part in path.split('/') {
        if part.is_empty() || part == "." {
            return None;
        }
        if part == ".." {
            depth -= 1;
            if depth < 0 {
                return None;
            }
        } else {
            depth += 1;
        }
    }
    Some(format!("files/{path}"))
}

pub fn write_file(path: &str, data: &[u8]) -> bool {
    let Some(path) = map_path(path) else {
        return false;
    };
    if let Some(parent) = path.parent() {
        if fs::create_dir_all(parent).is_err() {
            return false;
        }
    }
    fs::write(path, data).is_ok()
}
