#!/usr/bin/env bash
#
# 画面付き (GUI) で QEMU 起動
#   WSLg 経由で Linux の GUI を Windows デスクトップのウィンドウとして表示する
#
#   wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/boot-gui.sh"
#
set -uo pipefail

WIN_OUT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/output"
KERNEL="$WIN_OUT/bzImage"
INITRD="$WIN_OUT/initramfs.cpio.gz"

[ -f "$KERNEL" ] || { echo "見つかりません: $KERNEL (scripts/build-and-boot.sh を先に実行)"; exit 1; }
[ -f "$INITRD" ] || { echo "見つかりません: $INITRD"; exit 1; }

# --- WSLg: Linux GUI アプリを Windows 側に表示するための環境変数 ---
export DISPLAY="${DISPLAY:-:0}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"

echo "QEMU (GUI) 起動中 … Windows デスクトップにウィンドウが開きます"
echo "  終了方法: 画面のシェルで poweroff -f  ／ ウィンドウを閉じる (Ctrl+C)"

ARGS=(
  -machine pc -m 1G -smp 2
  -kernel "$KERNEL"
  -initrd "$INITRD"
  -append "console=tty0 rdinit=/init loglevel=6"
  -vga std
)

# GTK ウィンドウを試し、だめなら SDL にフォールバック
if qemu-system-x86_64 "${ARGS[@]}" -display gtk; then
  exit 0
fi
echo "gtk で失敗 → sdl で再試行"
exec qemu-system-x86_64 "${ARGS[@]}" -display sdl
