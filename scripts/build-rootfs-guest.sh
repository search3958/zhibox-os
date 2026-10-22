#!/usr/bin/env bash
#
# macOS だけで guest WebKitGTK rootfs を作る (Linux コンテナ不要)。
#
#   bash scripts/build-rootfs-guest.sh
#
# 仕組み:
#   1. Ubuntu 24.04 の公式 base rootfs (amd64) をダウンロード
#   2. scripts/guest/web/ と初回起動スクリプトを入れて ext4 イメージを作る
#   3. そのイメージを QEMU に IDE ディスクとして渡して起動
#   4. ゲスト自身が apt で WebKitGTK / Xvfb / Python GI を導入
#   5. 完了したイメージを output/rootfs.img に置く
#
# Linux/WSL では scripts/build-rootfs.sh (debootstrap) を使う。macOS から
# scripts/build-rootfs.sh を呼んだ場合も、このスクリプトへ自動で分岐する。
#
# 環境変数:
#   ZH_ROOTFS_SIZE       イメージサイズ (既定: 3G)
#   ZH_ROOTFS_BASE_URL   Ubuntu base tarball の URL
#   ZH_ROOTFS_BUILD_DIR  作業ディレクトリ (既定: output/build/rootfs-guest)
#   ZH_KEEP_ROOTFS=1     成功後も作業ディレクトリを残す
#   ZH_BUILD_TIMEOUT     ゲストの apt を含む最大秒数 (既定: 3600)
#
set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

[ "$OS" = macos ] || fail "このスクリプトは macOS 専用です。
Linux/WSL では sudo bash scripts/build-rootfs.sh を使ってください。"

QEMU="$(command -v qemu-system-x86_64)" || fail "qemu-system-x86_64 がありません (brew install qemu)"
for tool in curl tar mke2fs; do
  command -v "$tool" >/dev/null 2>&1 || \
    fail "$tool がありません。brew install qemu e2fsprogs curl を確認してください"
done

KERNEL="${ZH_KERNEL:-$OUT/bzImage}"
[ -f "$KERNEL" ] || fail "bzImage がありません: $KERNEL
先に bash scripts/build-and-boot.sh を実行してください。"

BASE_URL="${ZH_ROOTFS_BASE_URL:-https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-amd64.tar.gz}"
BASE_TAR="$OUT/cache/$(basename "$BASE_URL")"
IMAGE_SIZE="${ZH_ROOTFS_SIZE:-3G}"
BUILD_DIR="${ZH_ROOTFS_BUILD_DIR:-$OUT/build/rootfs-guest}"
ROOT="$BUILD_DIR/root"
IMG="$BUILD_DIR/rootfs.img"
LOG="$BUILD_DIR/firstboot.log"

# rm -rf する前に、明らかに危険なパスを拒否する。
case "$BUILD_DIR" in
  ""|"/"|"$HOME"|"$OUT"|"$REPO_ROOT") fail "危険な ZH_ROOTFS_BUILD_DIR です: $BUILD_DIR" ;;
esac

mkdir -p "$OUT/cache" "$OUT/build"

if [ ! -s "$BASE_TAR" ]; then
  say "Ubuntu base rootfs を取得 ($BASE_URL)"
  curl -fL --retry 3 --timeout 600 \
    -o "$BASE_TAR.part" "$BASE_URL"
  mv "$BASE_TAR.part" "$BASE_TAR"
fi
ls -lh "$BASE_TAR"

say "rootfs ツリーを展開"
rm -rf "$BUILD_DIR"
mkdir -p "$ROOT"
tar -xzf "$BASE_TAR" -C "$ROOT"

install -d -m 0755 "$ROOT/opt/zhibox"
install -m 0644 "$REPO_ROOT/scripts/guest/web/index.html" \
  "$ROOT/opt/zhibox/index.html"
install -m 0755 "$REPO_ROOT/scripts/guest/web/render-html.py" \
  "$ROOT/opt/zhibox/render-html.py"
install -m 0755 "$REPO_ROOT/scripts/guest/web/launch.sh" \
  "$ROOT/opt/zhibox/launch.sh"

# base rootfs には ip / udhcpc / poweroff が無いので、initramfs と同じ
# 静的 busybox を入れて初回起動スクリプトから使う。
BUSYBOX_CACHE="$OUT/cache/busybox-x86_64-static"
if [ ! -x "$BUSYBOX_CACHE" ]; then
  say "x86_64 静的 busybox を取得"
  mkdir -p "$(dirname "$BUSYBOX_CACHE")"
  curl -fL --retry 3 -o "$BUSYBOX_CACHE.part" \
    "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
  chmod +x "$BUSYBOX_CACHE.part"
  mv "$BUSYBOX_CACHE.part" "$BUSYBOX_CACHE"
fi
install -m 0755 "$BUSYBOX_CACHE" "$ROOT/bin/busybox"

# 初回起動専用スクリプト。通常起動では scripts/guest/init が
# /opt/zhibox/launch.sh を呼ぶだけで、このスクリプトは実行されない。
cat > "$ROOT/opt/zhibox/firstboot.sh" <<'EOF'
#!/bin/sh
# ゲスト自身で WebKitGTK を apt から導入し、rootfs を完成させる。
# scripts/build-rootfs-guest.sh が QEMU 起動時に init= で一度だけ実行する。
set -e
set -x
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive
export DEBCONF_NONINTERACTIVE_SEEN=true
export TZ=UTC

# macOS 側で展開した rootfs はホスト UID のままなので、ゲストの root に直す。
# まだ proc/sys/dev をマウントしていないこの時点で実行する。
chown -R 0:0 / 2>/dev/null || true

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t tmpfs tmpfs /run 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /tmp /var/tmp
mount -t devpts devpts /dev/pts 2>/dev/null || true
mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
chmod 1777 /tmp /var/tmp /dev/shm 2>/dev/null || true

# apt の postinst が systemd サービスを起動しようとするのを止める。
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d

# QEMU の user-mode NAT。カーネルの ip=dhcp が失敗しても busybox で拾う。
if ! /bin/busybox ip addr show eth0 2>/dev/null | grep -q 'inet '; then
  /bin/busybox ip link set eth0 up 2>/dev/null || true
  /bin/busybox udhcpc -i eth0 -n -q -t 20 -T 3 2>/dev/null || true
fi
rm -f /etc/resolv.conf
printf 'nameserver 10.0.2.3\nnameserver 1.1.1.1\n' > /etc/resolv.conf
echo 'zh-build: network'
/bin/busybox ip addr show eth0 2>/dev/null || true

APT_OPTS='-o Acquire::Retries=5 -o Acquire::Languages=none'
apt-get update $APT_OPTS
apt-get install -y $APT_OPTS --no-install-recommends \
  python3 python3-gi python3-gi-cairo \
  gir1.2-gtk-3.0 gir1.2-gdkpixbuf-2.0 \
  gir1.2-webkit2-4.1 libwebkit2gtk-4.1-0 \
  xvfb dbus-daemon dbus-bin dbus-session-bus-common \
  fonts-dejavu-core libgl1-mesa-dri libxtst6

apt-get clean
rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/* \
       /tmp/* /var/tmp/* /usr/sbin/policy-rc.d

echo 'zh-build: WebKitGTK installed'
python3 - <<'PY'
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import Gtk, WebKit2
print("zh-build: WebKit2", WebKit2.get_major_version(), WebKit2.get_minor_version())
PY
sync
echo 'zh-build: DONE'
/bin/busybox poweroff -f
EOF
chmod 0755 "$ROOT/opt/zhibox/firstboot.sh"

# 読み取り専用 rootfs でも WebKit / Xvfb が使えるようにマウントポイントを用意。
mkdir -p "$ROOT/proc" "$ROOT/sys" "$ROOT/dev/pts" "$ROOT/dev/shm" \
         "$ROOT/tmp" "$ROOT/run" "$ROOT/out" \
         "$ROOT/var/lib/apt/lists/partial" "$ROOT/var/cache/apt/archives/partial"
chmod 1777 "$ROOT/tmp"
printf '0123456789abcdef0123456789abcdef\n' > "$ROOT/etc/machine-id"
chmod 0644 "$ROOT/etc/machine-id"

say "ext4 イメージを作成 ($IMAGE_SIZE)"
rm -f "$IMG"
truncate -s "$IMAGE_SIZE" "$IMG"
mke2fs -q -t ext4 -F -L ZHROOT -d "$ROOT" -E root_owner=0:0 "$IMG"
ls -lh "$IMG"

say "QEMU ゲストで WebKitGTK を導入 (初回は 10〜20 分)"
TIMEOUT_CMD=timeout
command -v gtimeout >/dev/null && TIMEOUT_CMD=gtimeout
set +e
"$TIMEOUT_CMD" -k 10 "${ZH_BUILD_TIMEOUT:-3600}" "$QEMU" \
  -machine pc -m 2048 -smp 2 -no-reboot \
  -kernel "$KERNEL" \
  -drive "file=$IMG,format=raw,if=ide,index=0" \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -append "console=ttyS0 root=/dev/sda rw rootfstype=ext4 rootwait init=/opt/zhibox/firstboot.sh ip=dhcp net.ifnames=0 panic=-1 loglevel=5" \
  -nographic -monitor none > "$LOG" 2>&1
rc=$?
set -e

if ! grep -q 'zh-build: DONE' "$LOG" || \
   ! grep -q 'zh-build: WebKit2' "$LOG"; then
  echo "---- ゲストのログ (末尾) ----" >&2
  tail -160 "$LOG" >&2
  fail "rootfs の作成に失敗しました (qemu rc=$rc)。ログ: $LOG"
fi

mkdir -p "$OUT"
mv -f "$IMG" "$OUT/rootfs.img"
ls -lh "$OUT/rootfs.img"
du -h "$OUT/rootfs.img"

if [ "${ZH_KEEP_ROOTFS:-0}" != 1 ]; then
  rm -rf "$BUILD_DIR"
else
  echo "作業ディレクトリ: $BUILD_DIR"
fi

echo
echo "===== rootfs 完了 ====="
echo "output/rootfs.img (guest WebKitGTK, JavaScript off)"
echo "次: bash scripts/build-initramfs.sh"
echo "    bash scripts/boot-gui.sh"
