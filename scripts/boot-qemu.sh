#!/usr/bin/env bash
#
# ビルド済みカーネルを QEMU で対話的に起動する (WSL / macOS 両対応)
#
#   画面付き:   bash scripts/boot-qemu.sh
#   シリアル:   SERIAL=1 bash scripts/boot-qemu.sh
#
#   終了方法: ゲストのシェルで "poweroff -f"
#             画面付きはウィンドウを閉じる / シリアルは Ctrl+A → X
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

INITRD="${1:-$OUT/initramfs.cpio.gz}"
KERNEL="${2:-$OUT/bzImage}"

[ -f "$KERNEL" ] || { echo "bzImage がありません: $KERNEL (scripts/build-and-boot.sh を先に実行)"; exit 1; }
[ -f "$INITRD" ] || { echo "initramfs がありません: $INITRD"; exit 1; }

QEMU="$(command -v qemu-system-x86_64)" || { echo "qemu-system-x86_64 がありません"; exit 1; }

ARGS=(
  -machine pc -m 1G -smp 2
  -kernel "$KERNEL"
  -initrd "$INITRD"
  -vga virtio
)

if [ -n "${SERIAL:-}" ]; then
  # シリアルコンソール (対話)
  echo "QEMU (シリアルコンソール) 起動中 … 終了: Ctrl+A → X"
  exec "$QEMU" "${ARGS[@]}" \
    -append "console=ttyS0 rdinit=/init loglevel=6" \
    -nographic -monitor none
fi

if [ "$OS" = macos ]; then
  # ------------------------------------------------------------- macOS: Cocoa ウィンドウ
  echo "QEMU (GUI) 起動中 … Cocoa ウィンドウが開きます"
  echo "  終了方法: 画面のシェルで poweroff -f ／ ウィンドウを閉じる (Ctrl+C)"
  exec "$QEMU" "${ARGS[@]}" \
    -append "console=tty0 rdinit=/init loglevel=6 zh_gui=1" \
    -display cocoa
fi

# ------------------------------------------------------------- WSL: WSLg 経由で GTK/SDL
export DISPLAY="${DISPLAY:-:0}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"

echo "QEMU (GUI) 起動中 … Windows デスクトップにウィンドウが開きます"
echo "  終了方法: 画面のシェルで poweroff -f ／ ウィンドウを閉じる (Ctrl+C)"

GUI_ARGS=(
  "${ARGS[@]}"
  -append "console=tty0 rdinit=/init loglevel=6 zh_gui=1"
)

if "$QEMU" "${GUI_ARGS[@]}" -display gtk; then
  exit 0
fi
echo "gtk で失敗 → sdl で再試行"
exec "$QEMU" "${GUI_ARGS[@]}" -display sdl
