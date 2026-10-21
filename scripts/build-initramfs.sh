#!/usr/bin/env bash
#
# 最小 initramfs (busybox) を作成する (WSL / macOS 両対応)
#   成果物: $HOME/initramfs.cpio.gz
#
#   WSL : busybox-static パッケージの /bin/busybox を使用
#   macOS: 公式の x86_64 静的 busybox バイナリをダウンロードしキャッシュ
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

BUSYBOX_CACHE="$OUT/cache/busybox-x86_64-static"

rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/{bin,sbin,proc,sys,dev,tmp,root,usr/bin,usr/sbin,usr/local/bin}

if [ "$OS" = macos ]; then
  # ------------------------------------------------------------- macOS
  if [ ! -x "$BUSYBOX_CACHE" ]; then
    say "x86_64 静的 busybox を取得 (busybox.net 公式ビルド)"
    mkdir -p "$(dirname "$BUSYBOX_CACHE")"
    curl -fL --retry 3 -o "$BUSYBOX_CACHE.part" \
      "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
    chmod +x "$BUSYBOX_CACHE.part"
    mv "$BUSYBOX_CACHE.part" "$BUSYBOX_CACHE"
  fi
  cp "$BUSYBOX_CACHE" "$INITRAMFS/bin/busybox"
  # gradient (フレームバウンド描画デモ) は静的 Linux バイナリが無いため macOS では同梱しない
else
  # ------------------------------------------------------------- WSL / Linux
  [ -x /bin/busybox ] || fail "/bin/busybox がありません: apt install busybox-static"
  cp /bin/busybox "$INITRAMFS/bin/busybox"
  gcc -static -O2 -Wall -Wextra -Werror \
    -o "$INITRAMFS/bin/gradient" "$REPO_ROOT/scripts/fb-gradient.c"
fi

cp "$REPO_ROOT/scripts/initramfs-init.sh" "$INITRAMFS/init"
chmod 0755 "$INITRAMFS/init"
( cd "$INITRAMFS" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$INITRD"
ls -lh "$INITRD"
