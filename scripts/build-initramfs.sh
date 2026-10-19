#!/usr/bin/env bash
set -euo pipefail

WIN_ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
INITRAMFS="$HOME/initramfs"
INITRD="$HOME/initramfs.cpio.gz"

rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/{bin,sbin,proc,sys,dev,tmp,root,usr/bin,usr/sbin,usr/local/bin}
cp /bin/busybox "$INITRAMFS/bin/busybox"
gcc -static -O2 -Wall -Wextra -Werror \
  -o "$INITRAMFS/bin/gradient" "$WIN_ROOT/scripts/fb-gradient.c"
cp "$WIN_ROOT/scripts/initramfs-init.sh" "$INITRAMFS/init"
chmod 0755 "$INITRAMFS/init"
( cd "$INITRAMFS" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$INITRD"
ls -lh "$INITRD"
