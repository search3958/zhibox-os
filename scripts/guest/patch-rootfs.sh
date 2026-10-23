#!/bin/busybox sh
#
# rootfs.img の /opt/zhibox だけを差分更新する (ゲスト側 / PID 1)。
#
# build.sh が scripts/guest/web/ のファイルを /patch に同梱した initramfs を
# 作って QEMU を起動する。このスクリプトは rootfs を読み書き可能でマウントし、
# 更新されたファイルだけをコピーして電源を切る。apt をやり直すより遥かに速い。
#
set -e

/bin/busybox --install -s
mount -t proc     proc     /proc 2>/dev/null || true
mount -t sysfs    sysfs    /sys  2>/dev/null || true
mount -t devtmpfs devtmpfs /dev  2>/dev/null || true
mount -t tmpfs    tmpfs    /tmp  2>/dev/null || true

ROOT=/mnt/root
mkdir -p "$ROOT"

# IDE (sda) / virtio (vda) のどちらで渡されても動く。
DISK=""
i=0
while [ "$i" -lt 30 ]; do
	for dev in /dev/sda /dev/vda /dev/sdb /dev/vdb; do
		[ -b "$dev" ] || continue
		DISK="$dev"
		break
	done
	[ -n "$DISK" ] && break
	i=$((i + 1))
	sleep 1
done
[ -n "$DISK" ] || { echo "rootfs-patch: disk not found" >&2; poweroff -f; }

echo "rootfs-patch: $DISK"
mount -t ext4 -o rw "$DISK" "$ROOT"

mkdir -p "$ROOT/opt/zhibox"
for f in index.html render-html.py launch.sh; do
	[ -f "/patch/$f" ] || continue
	cp -f "/patch/$f" "$ROOT/opt/zhibox/$f"
	chmod 0644 "$ROOT/opt/zhibox/$f"
done
chmod 0755 "$ROOT/opt/zhibox/render-html.py" "$ROOT/opt/zhibox/launch.sh"

sync
umount "$ROOT"
echo "ZH_ROOTFS_PATCH_OK"
poweroff -f
