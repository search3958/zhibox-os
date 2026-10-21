#!/usr/bin/env bash
# ビルド用ツリーを git オブジェクトから丸ごと作り直す
# (中断・部分コピー等で汚れた場合に使用。成果物 object は無傷なのでビルドは即再開できる)
set -euo pipefail

WIN_ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
WIN_SRC="$WIN_ROOT/kernel"
if [ -d "$WIN_SRC/.git" ]; then GD="$WIN_SRC/.git"; else GD="$WIN_ROOT/kernel.git"; fi
SRC="$HOME/kernel-src"

echo "cleaning: $SRC"
rm -rf "$SRC"
mkdir -p "$SRC"

echo "extracting from git objects (HEAD)..."
git --git-dir="$GD" archive HEAD | tar -x -C "$SRC"

echo "files=$(find "$SRC" -type f | wc -l)  size=$(du -sh "$SRC" | cut -f1)"

echo "--- 大文字小文字ペアの検証 ---"
for p in \
  include/uapi/linux/netfilter/xt_CONNMARK.h \
  include/uapi/linux/netfilter/xt_connmark.h \
  include/uapi/linux/netfilter/xt_MARK.h \
  include/uapi/linux/netfilter/xt_mark.h \
  include/uapi/linux/netfilter/xt_DSCP.h \
  include/uapi/linux/netfilter/xt_dscp.h \
  net/netfilter/xt_HL.c \
  net/netfilter/xt_hl.c ; do
  if [ -f "$SRC/$p" ]; then echo "  有 $p"; else echo "  欠 $p"; exit 1; fi
done
echo "DONE"
