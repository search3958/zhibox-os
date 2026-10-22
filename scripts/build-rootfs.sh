#!/usr/bin/env bash
#
# ゲスト Linux 用の rootfs イメージを作る。
#
#   WSL / Linux : sudo bash scripts/build-rootfs.sh
#   macOS       : bash scripts/build-rootfs.sh  (QEMU ゲスト内で apt)
#
# できること:
#   - debootstrap で amd64 の最小 Debian/Ubuntu を作る
#   - WebKitGTK (GTK3) / Python GI / Xvfb / フォントを入れる
#   - scripts/guest/web/ の HTML とレンダラを /opt/zhibox に置く
#   - output/rootfs.img (ext4, パーティションなし) を作る
#
# この rootfs は QEMU の IDE ディスクとしてゲストに渡され、ゲストの init が
# 読み取り専用でマウントして chroot し、Xvfb 上で WebKitGTK を実行する。
# macOS では WebKitGTK を実行できないため、macOS から呼ぶと
# scripts/build-rootfs-guest.sh (QEMU ゲスト内で apt) に自動で切り替わる。
#
# 環境変数:
#   ZH_ROOTFS_DIR     作業ディレクトリ (既定: $HOME/zhibox-rootfs)
#   ZH_ROOTFS_SUITE   Debian/Ubuntu の suite (既定: ホストの VERSION_CODENAME)
#   ZH_ROOTFS_MIRROR  ミラー URL (既定: ホスト種別から自動)
#   ZH_ROOTFS_SIZE    イメージサイズ (既定: 2G)
#   ZH_KEEP_ROOTFS=1  成功後も作業ディレクトリを残す
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

if [ "$OS" = macos ]; then
  # macOS は WebKitGTK をホストで実行できない。Linux コンテナも使わず、
  # QEMU ゲスト自身に apt させる (scripts/build-rootfs-guest.sh)。
  exec bash "$REPO_ROOT/scripts/build-rootfs-guest.sh" "$@"
fi

[ "$(id -u)" -eq 0 ] || fail "root で実行してください: sudo bash scripts/build-rootfs.sh"

case "$(uname -m)" in
  x86_64|amd64) ;;
  *) fail "ゲストは x86_64 カーネルです。x86_64 の Linux/WSL で実行してください (現在: $(uname -m))" ;;
esac

command -v debootstrap >/dev/null 2>&1 || \
  fail "debootstrap がありません: sudo apt install debootstrap e2fsprogs"
command -v mkfs.ext4 >/dev/null 2>&1 || \
  fail "mkfs.ext4 がありません: sudo apt install e2fsprogs"

HOST_ID="debian"
HOST_CODENAME="bookworm"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  HOST_ID="${ID:-debian}"
  HOST_CODENAME="${VERSION_CODENAME:-bookworm}"
fi

SUITE="${ZH_ROOTFS_SUITE:-$HOST_CODENAME}"
case "$HOST_ID" in
  ubuntu)
    MIRROR="${ZH_ROOTFS_MIRROR:-http://archive.ubuntu.com/ubuntu}"
    SECURITY="http://security.ubuntu.com/ubuntu"
    COMPONENTS="main universe"
    ;;
  *)
    MIRROR="${ZH_ROOTFS_MIRROR:-http://deb.debian.org/debian}"
    SECURITY="http://security.debian.org/debian-security"
    COMPONENTS="main"
    ;;
esac

ROOTFS_DIR="${ZH_ROOTFS_DIR:-$HOME/zhibox-rootfs}"
IMAGE="$OUT/rootfs.img"
IMAGE_SIZE="${ZH_ROOTFS_SIZE:-2G}"

# rm -rf する前に、明らかに危険なパスを拒否する。
case "$ROOTFS_DIR" in
  ""|"/"|"$HOME"|"$OUT"|"$REPO_ROOT") fail "危険な ZH_ROOTFS_DIR です: $ROOTFS_DIR" ;;
esac

say "0/5 作業ディレクトリを準備 ($ROOTFS_DIR)"
rm -rf "$ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR"

say "1/5 debootstrap: $SUITE ($MIRROR)"
# minbase + amd64。ゲストのカーネルは x86_64。
debootstrap --variant=minbase --arch=amd64 \
  --components="$(echo "$COMPONENTS" | tr ' ' ',')" \
  "$SUITE" "$ROOTFS_DIR" "$MIRROR"

# apt 用の sources.list を明示する (debootstrap の既定に依存しない)
cat > "$ROOTFS_DIR/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE $COMPONENTS
deb $MIRROR $SUITE-updates $COMPONENTS
deb $SECURITY $SUITE-security $COMPONENTS
EOF

cp /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"

# apt / chroot のために proc, sys, dev を貸す
cleanup_mounts() {
  umount "$ROOTFS_DIR/dev" 2>/dev/null || true
  umount "$ROOTFS_DIR/proc" 2>/dev/null || true
  umount "$ROOTFS_DIR/sys" 2>/dev/null || true
}
trap cleanup_mounts EXIT INT TERM
mkdir -p "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys" "$ROOTFS_DIR/dev"
mount -t proc proc "$ROOTFS_DIR/proc"
mount -t sysfs sysfs "$ROOTFS_DIR/sys"
mount --bind /dev "$ROOTFS_DIR/dev"

say "2/5 WebKitGTK / Xvfb / Python GI を導入"
chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive \
  apt-get update -o Acquire::Retries=3

# Debian 12 は WebKitGTK 4.0、Ubuntu 24.04 以降は 4.1。
WEBKIT_PKGS="gir1.2-webkit2-4.0 libwebkit2gtk-4.0-37"
if chroot "$ROOTFS_DIR" sh -c \
    'apt-cache show gir1.2-webkit2-4.1 2>/dev/null | grep -q "^Package:"'; then
  WEBKIT_PKGS="gir1.2-webkit2-4.1 libwebkit2gtk-4.1-0"
fi

chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive \
  apt-get install -y --no-install-recommends \
    python3 python3-gi python3-gi-cairo \
    gir1.2-gtk-3.0 gir1.2-gdkpixbuf-2.0 \
    xvfb dbus fonts-dejavu-core libgl1-mesa-dri libxtst6 \
    $WEBKIT_PKGS

say "3/5 HTML / レンダラ / ランチャを配置"
install -d -m 0755 "$ROOTFS_DIR/opt/zhibox"
install -m 0644 "$REPO_ROOT/scripts/guest/web/index.html" \
  "$ROOTFS_DIR/opt/zhibox/index.html"
install -m 0755 "$REPO_ROOT/scripts/guest/web/render-html.py" \
  "$ROOTFS_DIR/opt/zhibox/render-html.py"
install -m 0755 "$REPO_ROOT/scripts/guest/web/launch.sh" \
  "$ROOTFS_DIR/opt/zhibox/launch.sh"
# init は rootfs を読み取り専用でマウントするため、マウントポイントは
# イメージ作成時に用意しておく必要がある。
mkdir -p "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys" \
         "$ROOTFS_DIR/dev/pts" "$ROOTFS_DIR/dev/shm" \
         "$ROOTFS_DIR/tmp" "$ROOTFS_DIR/run" "$ROOTFS_DIR/out"
chmod 1777 "$ROOTFS_DIR/tmp"

# dbus / WebKit が起動時に読み込む。読み取り専用 rootfs でも困らないよう固定値。
printf '0123456789abcdef0123456789abcdef\n' > "$ROOTFS_DIR/etc/machine-id"
chmod 0644 "$ROOTFS_DIR/etc/machine-id"

# キャッシュを落としてイメージを小さくする
chroot "$ROOTFS_DIR" apt-get clean
rm -rf "$ROOTFS_DIR/var/lib/apt/lists"/* \
       "$ROOTFS_DIR/var/cache/apt/archives"/* \
       "$ROOTFS_DIR/tmp"/* "$ROOTFS_DIR/var/tmp"/*

say "4/5 ext4 イメージを作成 ($IMAGE, $IMAGE_SIZE)"
# proc/sys/dev をマウントしたままだと mke2fs -d が仮想ファイルまで
# 読みに行くため、必ず先に外す。
cleanup_mounts
trap - EXIT INT TERM

mkdir -p "$OUT"
rm -f "$IMAGE"
truncate -s "$IMAGE_SIZE" "$IMAGE"
mkfs.ext4 -F -L ZHROOT -d "$ROOTFS_DIR" "$IMAGE"

say "5/5 完了"
ls -lh "$IMAGE"
du -sh "$ROOTFS_DIR"
if [ "${ZH_KEEP_ROOTFS:-0}" != 1 ]; then
  rm -rf "$ROOTFS_DIR"
  echo "    作業ディレクトリを削除しました (残す場合: ZH_KEEP_ROOTFS=1)"
fi
echo
echo "次: bash scripts/build-initramfs.sh"
echo "    bash scripts/boot-gui.sh   # rootfs.img を IDE ディスクとして接続"
