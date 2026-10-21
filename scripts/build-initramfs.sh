#!/usr/bin/env bash
#
# 最小 initramfs (busybox) を作成する (WSL / macOS 両対応)
#   成果物: $HOME/initramfs.cpio.gz
#
#   WSL : busybox-static パッケージの /bin/busybox を使用
#   macOS: 公式の x86_64 静的 busybox バイナリをダウンロードしキャッシュ
#   gradient (fb-gradient.c) は両方で同梱する (macOS は linux-shim でクロスビルド)
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

BUSYBOX_CACHE="$OUT/cache/busybox-x86_64-static"

rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/{bin,sbin,proc,sys,dev,tmp,root,usr/bin,usr/sbin,usr/local/bin}
mkdir -p "$INITRAMFS/usr/share/zhibox"

# GUI 用アセット (TrueType フォント / PNG 壁紙)
cp "$REPO_ROOT/scripts/guest/HarmonyOS_Sans_SC_Regular.ttf" \
   "$INITRAMFS/usr/share/zhibox/font.ttf"
cp "$REPO_ROOT/scripts/guest/wallpaper.png" \
   "$INITRAMFS/usr/share/zhibox/wallpaper.png"

# HTML+CSS (WebKit) を raw RGBA にレンダリングして同梱する。
#   ZH_HTML_RENDER=0 でスキップ、ZH_HTML_RAW=path で既存ファイルを使う。
#   レンダリングに失敗しても framebuffer デモ自体は動くよう、警告のみで続行する。
if [ -n "${ZH_HTML_RAW:-}" ] && [ -f "${ZH_HTML_RAW}" ]; then
  cp "$ZH_HTML_RAW" "$INITRAMFS/usr/share/zhibox/html.raw"
  say "HTML raw を同梱 (${ZH_HTML_RAW})"
elif [ "${ZH_HTML_RENDER:-1}" != 0 ]; then
  if bash "$REPO_ROOT/scripts/render-html.sh" "$OUT/html.raw"; then
    cp "$OUT/html.raw" "$INITRAMFS/usr/share/zhibox/html.raw"
  else
    echo "    [warn] HTML レンダリングをスキップ (WEBKIT ウィンドウはプレースホルダー)"
  fi
fi

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

  # gradient (フレームバッファ描画デモ): macOS には Linux 用の静的 libc が無いので、
  # syscall を直接発行する freestanding 版として x86_64-elf-gcc でクロスビルドする
  # (scripts/host/darwin/linux-shim/ が最小限の libc 代替。glibc には依存しない)
  XGCC="${CROSS_COMPILE:-x86_64-elf-}gcc"
  if command -v "$XGCC" >/dev/null 2>&1; then
    say "gradient をクロスビルド ($($XGCC -dumpversion))"
    SHIM="$REPO_ROOT/scripts/host/darwin/linux-shim"
    BUILD_GRADIENT="$OUT/build/gradient"
    rm -rf "$BUILD_GRADIENT"
    mkdir -p "$BUILD_GRADIENT"
    GRADIENT_CFLAGS=(-nostdinc -isystem "$("$XGCC" -print-file-name=include)"
      -I "$SHIM/include" -ffreestanding -fno-builtin -fno-stack-protector
      -fno-pic -O2 -Wall -Wextra -Werror)
    "$XGCC" "${GRADIENT_CFLAGS[@]}" -c "$SHIM/mini.c" \
      -o "$BUILD_GRADIENT/mini.o"
    "$XGCC" "${GRADIENT_CFLAGS[@]}" -c "$SHIM/start.S" \
      -o "$BUILD_GRADIENT/start.o"
    "$XGCC" "${GRADIENT_CFLAGS[@]}" -c "$REPO_ROOT/scripts/guest/fb-gradient.c" \
      -o "$BUILD_GRADIENT/fb-gradient.o"
    "$XGCC" "${GRADIENT_CFLAGS[@]}" -c "$REPO_ROOT/scripts/guest/zh_font.c" \
      -o "$BUILD_GRADIENT/zh_font.o"
    "$XGCC" "${GRADIENT_CFLAGS[@]}" -c "$REPO_ROOT/scripts/guest/zh_png.c" \
      -o "$BUILD_GRADIENT/zh_png.o"
    "$XGCC" -nostdlib -static -no-pie -Wl,-e,_start \
      -o "$INITRAMFS/bin/gradient" "$BUILD_GRADIENT"/*.o
    chmod 0755 "$INITRAMFS/bin/gradient"
  else
    echo "    [warn] $XGCC が無いため gradient は同梱しません"
  fi
else
  # ------------------------------------------------------------- WSL / Linux
  [ -x /bin/busybox ] || fail "/bin/busybox がありません: apt install busybox-static"
  cp /bin/busybox "$INITRAMFS/bin/busybox"
  gcc -static -O2 -Wall -Wextra -Werror \
    -o "$INITRAMFS/bin/gradient" \
    "$REPO_ROOT/scripts/guest/fb-gradient.c" \
    "$REPO_ROOT/scripts/guest/zh_font.c" \
    "$REPO_ROOT/scripts/guest/zh_png.c"
fi

cp "$REPO_ROOT/scripts/guest/init" "$INITRAMFS/init"
chmod 0755 "$INITRAMFS/init"
( cd "$INITRAMFS" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$INITRD"
ls -lh "$INITRD"
