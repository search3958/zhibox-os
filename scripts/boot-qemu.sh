#!/usr/bin/env bash
#
# ビルド済みカーネルを QEMU で対話的に起動する
#   WSL 内で:  bash boot-qemu.sh
#   終了方法:  シェルで "poweroff -f" 、または Ctrl+A → X
#
set -euo pipefail

WIN_OUT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/output"
SRC="$HOME/kernel-src"
INITRD="${1:-$WIN_OUT/initramfs.cpio.gz}"
KERNEL="${2:-$WIN_OUT/bzImage}"

[ -f "$KERNEL" ] || { echo "bzImage がありません: $KERNEL"; exit 1; }
[ -f "$INITRD" ] || { echo "initramfs がありません: $INITRD"; exit 1; }

exec qemu-system-x86_64 \
  -machine pc -m 1G -smp 2 \
  -kernel "$KERNEL" \
  -initrd "$INITRD" \
  -append "console=ttyS0 rdinit=/init loglevel=6" \
  -nographic
