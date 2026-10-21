#!/usr/bin/env bash
# Windows の git は予約デバイス名 "aux" を open できない (ENOENT) ので、
# 該当 3 ファイルだけ WSL 側の git で索引化する。その後 skip-worktree を付け、
# Windows 側の git status / 今後の git add -A が取り扱わないようにする。
set -euo pipefail

ROOT="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os"
cd "$ROOT"

P1="kernel/drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c"
P2="kernel/drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.h"
P3="kernel/include/soc/arc/aux.h"

for f in "$P1" "$P2" "$P3"; do
  [ -f "$f" ] || { echo "missing on disk: $f"; exit 1; }
done

echo "== git add (WSL git, repo は core.autocrlf=true 済みなので CRLF→LF 正規化される) =="
git add -- "$P1" "$P2" "$P3"

echo "== skip-worktree 設定 =="
git update-index --skip-worktree -- "$P1" "$P2" "$P3"

echo "== 親リポジトリ index の blob id =="
git ls-files -s -- "$P1" "$P2" "$P3"

echo "== 元の kernel.git の blob id (一致すれば改行コード変換も正確) =="
git --git-dir=kernel.git --work-tree=kernel ls-tree HEAD -- \
  drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c \
  drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.h \
  include/soc/arc/aux.h

echo "== フラグ確認 (S なら skip-worktree) =="
git ls-files -v -- "$P1" "$P2" "$P3"

echo "== done =="
