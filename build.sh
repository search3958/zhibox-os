#!/bin/sh
#
# zhibox-os: 差分ビルド + QEMU 起動 (WSL / Linux / macOS 共通)
#
#   sh build.sh              # 変更されたものだけビルドして GUI 起動
#   sh build.sh --serial     # シリアルコンソールで起動
#   sh build.sh --no-boot    # ビルドだけ
#
# 変更検出:
#   kernel   : kernel/ が output/bzImage より新しいときだけ make (make 自体も差分)
#   initramfs: scripts/guest/ やフォント等のハッシュが変わったときだけ再生成
#   rootfs   : 無ければ作成。web/ だけ変わったら QEMU ゲストで /opt/zhibox を差分更新
#
# 強制再生成:
#   sh build.sh --rebuild-kernel
#   sh build.sh --rebuild-initramfs
#   sh build.sh --rebuild-rootfs
#
# 環境変数:
#   SERIAL=1            シリアル起動 (--serial と同じ)
#   JOBS=N              make の並列数
#   ZH_QEMU_MEM=2G      QEMU のメモリ
#   ZH_SKIP_BOOT_TEST=1 build-and-boot.sh のブート確認を省略
#
set -eu

# `sh build.sh` で呼ばれても bash 前提の共通スクリプトを安全に使う。
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -o pipefail

. "$(cd "$(dirname "$0")" && pwd)/scripts/lib/common.sh"

BOOT=1
FORCE_KERNEL=0
FORCE_INITRAMFS=0
FORCE_ROOTFS=0

usage() {
  sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --serial)          export SERIAL=1 ;;
    --no-boot)         BOOT=0 ;;
    --rebuild-kernel)  FORCE_KERNEL=1 ;;
    --rebuild-initramfs) FORCE_INITRAMFS=1 ;;
    --rebuild-rootfs)  FORCE_ROOTFS=1 ;;
    -h|--help)         usage; exit 0 ;;
    *)                 fail "不明なオプション: $1 (--help を参照)" ;;
  esac
  shift
done

cd "$REPO_ROOT"
mkdir -p "$OUT"

STAMP_DIR="$OUT/.build-stamps"
mkdir -p "$STAMP_DIR"
INITRAMFS_STAMP="$STAMP_DIR/initramfs.sha"
ROOTFS_WEB_STAMP="$STAMP_DIR/rootfs-web.sha"
ROOTFS_RECIPE_STAMP="$STAMP_DIR/rootfs-recipe.sha"

KERNEL_IMG="$OUT/bzImage"
INITRD_IMG="$OUT/initramfs.cpio.gz"
ROOTFS_IMG="${ZH_ROOTFS:-$OUT/rootfs.img}"

zh_sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum
  else
    fail "shasum / sha256sum がありません"
  fi
}

# パス (ファイルまたはディレクトリ) の内容ハッシュ。.DS_Store は無視する。
hash_paths() {
  local p f
  for p in "$@"; do
    if [ -d "$p" ]; then
      find "$p" -type f ! -name '.DS_Store' -print | LC_ALL=C sort | \
        while IFS= read -r f; do
          printf '%s\n' "${f#"$REPO_ROOT"/}"
          zh_sha256 < "$f"
        done
    elif [ -f "$p" ]; then
      printf '%s\n' "${p#"$REPO_ROOT"/}"
      zh_sha256 < "$p"
    fi
  done | zh_sha256 | awk '{print $1}'
}

# $1 より新しい入力があるか (ディレクトリは中身を見る)
any_newer() {
  local ref="$1" p f
  shift
  for p in "$@"; do
    if [ -d "$p" ]; then
      f="$(find "$p" -type f ! -name '.DS_Store' -newer "$ref" -print -quit 2>/dev/null || true)"
      [ -n "$f" ] && return 0
    elif [ -f "$p" ] && [ "$p" -nt "$ref" ]; then
      return 0
    fi
  done
  return 1
}

stamp_matches() {
  local file="$1" value="$2"
  [ -f "$file" ] && [ "$(cat "$file" 2>/dev/null)" = "$value" ]
}

write_stamp() {
  printf '%s\n' "$2" > "$1"
}

# ------------------------------------------------------------------ kernel
KERNEL_INPUTS=(
  "$REPO_ROOT/kernel"
)

kernel_needs_build() {
  [ "$FORCE_KERNEL" = 1 ] && return 0
  [ -f "$KERNEL_IMG" ] || return 0
  any_newer "$KERNEL_IMG" "${KERNEL_INPUTS[@]}" && return 0
  return 1
}

# --------------------------------------------------------------- initramfs
INITRAMFS_INPUTS=(
  "$REPO_ROOT/scripts/build-initramfs.sh"
  "$REPO_ROOT/scripts/lib/common.sh"
  "$REPO_ROOT/scripts/guest/init"
  "$REPO_ROOT/scripts/guest/fb-gradient.c"
  "$REPO_ROOT/scripts/guest/zh_font.c"
  "$REPO_ROOT/scripts/guest/zh_font.h"
  "$REPO_ROOT/scripts/guest/zh_png.c"
  "$REPO_ROOT/scripts/guest/zh_png.h"
  "$REPO_ROOT/scripts/guest/HarmonyOS_Sans_SC_Regular.ttf"
  "$REPO_ROOT/scripts/guest/wallpaper.png"
  "$REPO_ROOT/scripts/host/darwin/linux-shim"
  "$REPO_ROOT/scripts/build-warp4.sh"
  "$REPO_ROOT/scripts/guest/warp4"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/crates/baram-warp4"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/crates/baram-core"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/crates/baram-font"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/crates/baram-graphics"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/crates/stb_truetype_rust"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/files/data/fonts"
  "${BARAM_OS:-$REPO_ROOT/../baram-os}/files/data/ui/check-icon.svg"
)

# ------------------------------------------------------------------ rootfs
ROOTFS_WEB_INPUTS=(
  "$REPO_ROOT/scripts/guest/web/index.html"
  "$REPO_ROOT/scripts/guest/web/render-html.py"
  "$REPO_ROOT/scripts/guest/web/launch.sh"
)
ROOTFS_RECIPE_INPUTS=(
  "$REPO_ROOT/scripts/build-rootfs.sh"
  "$REPO_ROOT/scripts/build-rootfs-guest.sh"
  "$REPO_ROOT/scripts/lib/common.sh"
)

qemu_bin() {
  command -v qemu-system-x86_64 >/dev/null 2>&1 || \
    fail "qemu-system-x86_64 がありません (macOS: brew install qemu / WSL: sudo apt install qemu-system-x86)"
  command -v qemu-system-x86_64
}

timeout_run() {
  local secs="$1"
  shift
  if command -v gtimeout >/dev/null 2>&1; then
    gtimeout -k 5 "$secs" "$@"
  elif command -v timeout >/dev/null 2>&1; then
    timeout -k 5 "$secs" "$@"
  else
    "$@"
  fi
}

run_qemu_image_write() {
  local qemu="$1"
  shift
  if [ -w "$ROOTFS_IMG" ] || [ "$(id -u)" -eq 0 ]; then
    timeout_run 300 "$qemu" "$@"
  elif command -v sudo >/dev/null 2>&1; then
    echo "    rootfs.img が書き込み不可のため sudo で QEMU を実行します"
    timeout_run 300 sudo "$qemu" "$@"
  else
    fail "rootfs.img に書き込めません: $ROOTFS_IMG (sudo で実行してください)"
  fi
}

patch_rootfs_web() {
  local qemu="$1" dir="$OUT/build/rootfs-patch" log="$OUT/build/rootfs-patch.log"
  local busybox=""

  say "rootfs: web の差分を QEMU ゲストで反映 (apt は再実行しません)"
  rm -rf "$dir"
  mkdir -p "$dir/root/bin" "$dir/root/patch" "$dir/root/proc" "$dir/root/sys" \
           "$dir/root/dev" "$dir/root/tmp"

  if [ -x "$OUT/cache/busybox-x86_64-static" ]; then
    busybox="$OUT/cache/busybox-x86_64-static"
  elif [ -x /bin/busybox ]; then
    busybox=/bin/busybox
  else
    fail "静的 busybox がありません。先に scripts/build-initramfs.sh を実行してください"
  fi

  cp "$busybox" "$dir/root/bin/busybox"
  cp "$REPO_ROOT/scripts/guest/patch-rootfs.sh" "$dir/root/init"
  chmod 0755 "$dir/root/bin/busybox" "$dir/root/init"
  cp "$REPO_ROOT/scripts/guest/web/index.html"   "$dir/root/patch/index.html"
  cp "$REPO_ROOT/scripts/guest/web/render-html.py" "$dir/root/patch/render-html.py"
  cp "$REPO_ROOT/scripts/guest/web/launch.sh"    "$dir/root/patch/launch.sh"

  ( cd "$dir/root" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$dir/initramfs.cpio.gz"

  set +e
  run_qemu_image_write "$qemu" \
    -machine pc -m 512M -smp 2 -no-reboot \
    -kernel "$KERNEL_IMG" \
    -initrd "$dir/initramfs.cpio.gz" \
    -drive "file=$ROOTFS_IMG,format=raw,if=ide,index=0" \
    -append "console=ttyS0 rdinit=/init loglevel=4 panic=-1" \
    -nographic -monitor none > "$log" 2>&1
  local rc=$?
  set -e

  if ! grep -q "ZH_ROOTFS_PATCH_OK" "$log"; then
    echo "---- rootfs patch ログ (末尾) ----" >&2
    tail -80 "$log" >&2
    fail "rootfs の差分更新に失敗しました (qemu rc=$rc)。ログ: $log"
  fi

  rm -rf "$dir"
  echo "    /opt/zhibox を更新しました"
}

build_rootfs_full() {
  say "rootfs: イメージを作成 (初回のみ・時間がかかります)"
  if [ "$OS" = macos ] || [ "$(id -u)" -eq 0 ]; then
    bash "$REPO_ROOT/scripts/build-rootfs.sh"
  elif command -v sudo >/dev/null 2>&1; then
    sudo bash "$REPO_ROOT/scripts/build-rootfs.sh"
  else
    fail "rootfs の作成には root 権限が必要です: sudo sh build.sh"
  fi
}

# ============================================================ 1. kernel
say "1/4 kernel: 差分を確認"
if kernel_needs_build; then
  say "kernel: 変更を検出 → 差分ビルド (make は変更分のみ)"
  # SYNC=1: リポジトリ側 kernel/ の編集をビルドツリーへ反映
  ZH_SKIP_BOOT_TEST=1 SYNC=1 bash "$REPO_ROOT/scripts/build-and-boot.sh"
else
  echo "    最新です: $KERNEL_IMG ($(du -h "$KERNEL_IMG" | cut -f1))"
fi

# ========================================================= 2. initramfs
say "2/4 initramfs: 差分を確認"
INITRAMFS_HASH="$(hash_paths "${INITRAMFS_INPUTS[@]}")"
if [ ! -f "$INITRD_IMG" ] || [ "$FORCE_INITRAMFS" = 1 ]; then
  need_initramfs=1
elif stamp_matches "$INITRAMFS_STAMP" "$INITRAMFS_HASH"; then
  need_initramfs=0
elif [ -f "$INITRD_IMG" ] && ! any_newer "$INITRD_IMG" "${INITRAMFS_INPUTS[@]}"; then
  # 初回 (スタンプ導入前) は成果物の方が新しければ最新とみなす
  need_initramfs=0
else
  need_initramfs=1
fi

if [ "$need_initramfs" = 1 ]; then
  say "initramfs: 変更を検出 → 再生成"
  bash "$REPO_ROOT/scripts/build-initramfs.sh"
  cp -f "$INITRD" "$INITRD_IMG"
  write_stamp "$INITRAMFS_STAMP" "$INITRAMFS_HASH"
else
  echo "    最新です: $INITRD_IMG ($(du -h "$INITRD_IMG" | cut -f1))"
  write_stamp "$INITRAMFS_STAMP" "$INITRAMFS_HASH"
fi

# ============================================================ 3. rootfs
say "3/4 rootfs: 差分を確認"
ROOTFS_WEB_HASH="$(hash_paths "${ROOTFS_WEB_INPUTS[@]}")"
ROOTFS_RECIPE_HASH="$(hash_paths "${ROOTFS_RECIPE_INPUTS[@]}")"

if [ "$FORCE_ROOTFS" = 1 ] || [ ! -f "$ROOTFS_IMG" ]; then
  build_rootfs_full
  write_stamp "$ROOTFS_WEB_STAMP" "$ROOTFS_WEB_HASH"
  write_stamp "$ROOTFS_RECIPE_STAMP" "$ROOTFS_RECIPE_HASH"
elif stamp_matches "$ROOTFS_WEB_STAMP" "$ROOTFS_WEB_HASH"; then
  echo "    最新です: $ROOTFS_IMG ($(du -h "$ROOTFS_IMG" | cut -f1))"
else
  if [ ! -f "$ROOTFS_WEB_STAMP" ] && \
     ! any_newer "$ROOTFS_IMG" "${ROOTFS_WEB_INPUTS[@]}"; then
    echo "    最新です: $ROOTFS_IMG (web はイメージ作成後に変更なし)"
  else
    QEMU="$(qemu_bin)"
    patch_rootfs_web "$QEMU"
  fi
  write_stamp "$ROOTFS_WEB_STAMP" "$ROOTFS_WEB_HASH"
fi

# rootfs のパッケージ定義が変わった場合は自動再作成せず警告だけ出す。
if [ -f "$ROOTFS_RECIPE_STAMP" ]; then
  if ! stamp_matches "$ROOTFS_RECIPE_STAMP" "$ROOTFS_RECIPE_HASH"; then
    echo "    [warn] rootfs のパッケージ定義が変わっています。"
    echo "           反映するには: sh build.sh --rebuild-rootfs (apt 再実行・時間がかかります)"
  fi
else
  if any_newer "$ROOTFS_IMG" "${ROOTFS_RECIPE_INPUTS[@]}"; then
    echo "    [warn] rootfs のパッケージ定義がイメージより新しいです。"
    echo "           反映するには: sh build.sh --rebuild-rootfs (apt 再実行・時間がかかります)"
  else
    write_stamp "$ROOTFS_RECIPE_STAMP" "$ROOTFS_RECIPE_HASH"
  fi
fi

# ============================================================== 4. boot
if [ "$BOOT" = 1 ]; then
  say "4/4 QEMU を起動"
  if [ -n "${SERIAL:-}" ]; then
    echo "    シリアルコンソール (終了: Ctrl+A → X)"
  else
    echo "    画面付き (終了: ゲストで poweroff -f / ウィンドウを閉じる)"
  fi
  exec bash "$REPO_ROOT/scripts/boot-qemu.sh"
fi

say "4/4 ビルド完了 (--no-boot)"
echo "起動: bash scripts/boot-qemu.sh"
