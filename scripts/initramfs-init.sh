#!/bin/busybox sh
/bin/busybox --install -s
mount -t proc     proc     /proc  2>/dev/null
mount -t sysfs    sysfs    /sys   2>/dev/null
mount -t devtmpfs devtmpfs /dev   2>/dev/null
mount -t tmpfs    tmpfs    /tmp   2>/dev/null
echo
echo "=============================================="
echo "  openEuler kernel boot test"
echo "=============================================="
uname -a
echo
echo "ZHIBOX_BOOT_OK"
echo "Type 'gradient' to switch to the video mode."
echo "=============================================="
if grep -q boottest /proc/cmdline 2>/dev/null; then
  echo "(boot test mode -> poweroff)"
  poweroff -f
  sleep 5
  halt -f
fi
exec /bin/sh
