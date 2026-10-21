#!/usr/bin/env bash
#
# HTML+CSS を WebKit でレンダリングし、raw RGBA (ZHRAW1) を生成する。
#
#   macOS : WebKitGTK は Linux 専用のため、システムの WKWebView を使う
#   WSL   : WebKitGTK (PyGObject) をオフスクリーンでスナップショット
#
#   bash scripts/render-html.sh [output.raw]
#
# 環境変数:
#   ZH_HTML_WIDTH / ZH_HTML_HEIGHT   レンダリング解像度 (既定 640x400)
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

HTML="$REPO_ROOT/scripts/guest/web/index.html"
RAW="${1:-$OUT/html.raw}"
WIDTH="${ZH_HTML_WIDTH:-640}"
HEIGHT="${ZH_HTML_HEIGHT:-400}"

[ -f "$HTML" ] || fail "HTML がありません: $HTML"
mkdir -p "$(dirname "$RAW")"

if [ "$OS" = macos ]; then
  # --------------------------------------------------------------- macOS
  SWIFTC="$(xcrun --find swiftc 2>/dev/null || command -v swiftc || true)"
  [ -n "$SWIFTC" ] || fail "swiftc がありません (Xcode Command Line Tools を入れてください)"
  if [ ! -x "$OUT/build/render-html" ] ||
     [ "$REPO_ROOT/scripts/host/darwin/render-html.swift" -nt "$OUT/build/render-html" ]; then
    say "WKWebView レンダラをビルド"
    mkdir -p "$OUT/build"
    "$SWIFTC" -O -framework Cocoa -framework WebKit \
      "$REPO_ROOT/scripts/host/darwin/render-html.swift" \
      -o "$OUT/build/render-html"
  fi
  "$OUT/build/render-html" "$HTML" "$RAW" "$WIDTH" "$HEIGHT"
else
  # --------------------------------------------------------------- WSL / Linux
  if ! python3 - <<'PY' >/dev/null 2>&1
import gi
gi.require_version("Gtk", "3.0")
for version in ("4.1", "4.0"):
    try:
        gi.require_version("WebKit2", version)
        break
    except ValueError:
        pass
else:
    raise SystemExit(1)
PY
  then
    fail "WebKitGTK (PyGObject) がありません。例:
  sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-3.0 \\
      gir1.2-webkit2-4.0 libwebkit2gtk-4.0-37 xvfb fonts-dejavu-core
  (Ubuntu 24.04 以降は gir1.2-webkit2-4.1 / libwebkit2gtk-4.1-0)"
  fi

  RUN=()
  if [ -z "${DISPLAY:-}" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
      RUN=(xvfb-run -a -s "-screen 0 1280x800x24")
      echo "    [info] DISPLAY が無いため Xvfb を使用"
    else
      fail "DISPLAY がありません。WSLg で実行するか xvfb を入れてください"
    fi
  fi
  if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-run-session >/dev/null 2>&1; then
    RUN=(dbus-run-session -- "${RUN[@]}")
  fi

  GDK_BACKEND=x11 \
  WEBKIT_DISABLE_COMPOSITING_MODE=1 \
  WEBKIT_DISABLE_DMABUF_RENDERER=1 \
  WEBKIT_FORCE_SANDBOX=0 \
  LIBGL_ALWAYS_SOFTWARE=1 \
    "${RUN[@]}" python3 "$REPO_ROOT/scripts/host/wsl/render-html.py" \
      "$HTML" "$RAW" "$WIDTH" "$HEIGHT"
fi

python3 - "$RAW" "$WIDTH" "$HEIGHT" <<'PY'
import struct
import sys

path, width, height = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = open(path, "rb").read()
if len(data) < 24:
    raise SystemExit("raw が短すぎます: %s" % path)
magic, w, h, fmt, stride = struct.unpack("<8sIIII", data[:24])
if magic != b"ZHRAW1\x00\x00":
    raise SystemExit("magic が違います: %r" % magic)
if (w, h) != (width, height):
    raise SystemExit("サイズが違います: %dx%d (期待 %dx%d)" % (w, h, width, height))
if fmt != 0 or stride != w * 4:
    raise SystemExit("未対応の形式です: fmt=%d stride=%d" % (fmt, stride))
if len(data) < 24 + stride * h:
    raise SystemExit("ピクセルデータが不足しています")
print("    [ok] %s: %dx%d RGBA (%.1f MiB)" %
      (path, w, h, len(data) / (1024.0 * 1024.0)))
PY
