#!/usr/bin/env bash
#
# 画面付き (GUI) で QEMU 起動 — boot-qemu.sh のラッパー
#   WSL: WSLg 経由で Windows デスクトップに表示
#   macOS: Cocoa ウィンドウ
#
set -uo pipefail
exec bash "$(cd "$(dirname "$0")" && pwd)/boot-qemu.sh" "$@"
