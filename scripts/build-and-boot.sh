#!/usr/bin/env bash
#
# openEuler kernel (OLK-5.10) のビルド + QEMU ブート確認 一括スクリプト
# 実行場所: WSL (Ubuntu 22.04) の中
#
#   (Get-Content scripts/build-and-boot.sh -Raw) -replace "`r","" | wsl -d Ubuntu-22.04 -u root bash
#
# 補足:
#   Windows 側 (NTFS) は大文字小文字非区別のため、カーネルソース中の
#   "xt_CONNMARK.h / xt_connmark.h" のような13組の衝突ファイルを保持できない。
#   そこでビルド用ツリーは git のオブジェクトから直接展開して完全な状態を保つ。
#
set -euo pipefail

WIN_ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
WIN_SRC="$WIN_ROOT/kernel"     # Windows 側のクローン (git リポジトリ)
WIN_OUT="$WIN_ROOT/output"     # 成果物の出力先
SRC="$HOME/kernel-src"         # WSL ext4 (D:) 上のビルド用ツリー (完全・ケースセンシティブ)
INITRAMFS="$HOME/initramfs"
INITRD="$HOME/initramfs.cpio.gz"
LOG="$HOME/boot.log"
JOBS="$(nproc)"

say() { echo; echo "==> $*"; }

# 進捗ログを Windows 側にも書き出す (別窓で tail できるように)
if [ -t 1 ]; then :; else exec > >(tee "$WIN_ROOT/build.log") 2>&1; fi

# ------------------------------------------------------- 1. ツリー展開 + 同期
if [ -f "$SRC/Makefile" ]; then
  if [ "${SYNC:-0}" = "1" ]; then
    say "1/6 Windows 側の変更をオーバーレイ同期 (SYNC=1, size比較のみで高速)"
    rsync -a --size-only --exclude='.git' "$WIN_SRC/" "$SRC/"
  else
    say "1/6 ビルド用ツリーを利用 (Windows 側との同期は SYNC=1 で有効)"
  fi
else
  say "1/6 git オブジェクトから完全なツリーを展開 (大文字小文字衝突ファイルも含む)"
  mkdir -p "$SRC"
  git --git-dir="$WIN_SRC/.git" archive HEAD | tar -x -C "$SRC"
fi
cd "$SRC"
echo "    ツリー: $(du -sh "$SRC" 2>/dev/null | cut -f1)  /  jobs=$JOBS  /  $(gcc --version | head -1)"

# ---------------------------------------------------------------- 2. 設定
say "2/6 make defconfig"
if [ ! -f .config ]; then
  make defconfig
else
  echo "    .config あり (削除すると再生成)"
fi

# ---------------------------------------------------------------- 3. ビルド
say "3/6 make -j$JOBS bzImage"
make -j"$JOBS" bzImage
ls -lh arch/x86/boot/bzImage

# ------------------------------------------------------------- 4. initramfs
say "4/6 最小 initramfs (busybox) を作成"
rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/{bin,sbin,proc,sys,dev,tmp,root,usr/bin,usr/sbin,usr/local/bin}
cp /bin/busybox "$INITRAMFS/bin/busybox"
cat > "$INITRAMFS/init" <<'EOF'
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
echo "=============================================="
# 起動確認モード (cmdline に "boottest" を含む) では自動的に電源オフして QEMU を終了する
if grep -q boottest /proc/cmdline 2>/dev/null; then
  echo "(boot test mode -> poweroff)"
  poweroff -f
  sleep 5
  halt -f
fi
exec /bin/sh
EOF
chmod 0755 "$INITRAMFS/init"
( cd "$INITRAMFS" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$INITRD"
ls -lh "$INITRD"

# ----------------------------------------------------------- 5. QEMU ブートテスト
say "5/6 QEMU でブート確認 (最大90秒)"
set +e
timeout -k 5 90 qemu-system-x86_64 \
  -machine pc -m 512M -smp 2 -no-reboot \
  -kernel "$SRC/arch/x86/boot/bzImage" \
  -initrd "$INITRD" \
  -append "console=ttyS0 rdinit=/init panic=-1 loglevel=4 boottest" \
  -nographic -monitor none \
  > "$LOG" 2>&1
set -e

echo "---- シリアルコンソール出力 (先頭40行) ----"
head -40 "$LOG"
echo "-------------------------------------------"

if grep -q "ZHIBOX_BOOT_OK" "$LOG"; then
  echo "[OK] カーネルは QEMU 上で正常にブートしました。"
else
  echo "[NG] ブート確認に失敗しました。以下が全出力です:"
  cat "$LOG"
  exit 1
fi

# ------------------------------------------------------------ 6. 成果物の書き出し
say "6/6 成果物を Windows 側へコピー"
mkdir -p "$WIN_OUT"
cp -f "$SRC/arch/x86/boot/bzImage" "$WIN_OUT/bzImage"
cp -f "$SRC/.config"               "$WIN_OUT/config"
cp -f "$SRC/System.map"            "$WIN_OUT/System.map"
cp -f "$INITRD"                    "$WIN_OUT/initramfs.cpio.gz"
cp -f "$LOG"                       "$WIN_OUT/boot.log"
if [ -f "$WIN_ROOT/scripts/boot-qemu.sh" ]; then
  cp -f "$WIN_ROOT/scripts/boot-qemu.sh" "$WIN_OUT/boot-qemu.sh"
fi
ls -lh "$WIN_OUT"

echo
echo "===== 完了 ====="
echo "bzImage : $WIN_OUT/bzImage"
echo "起動確認: WSL 内で bash scripts/boot-qemu.sh"
