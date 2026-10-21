#!/usr/bin/env bash
# WSL(drvfs) から NTFS の「大文字小文字区別」ディレクトリへ、
# 大文字小文字違いの2ファイルを作成できるかの検証
D="/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/kernel/tools/memory-model/litmus-tests"
cd "$D" || { echo "cd failed"; exit 1; }
rm -f zz_b.tmp zz_B.tmp
touch zz_b.tmp zz_B.tmp
echo "created_count=$(ls -1 | grep -c '^zz_' || true)"
ls -1 | grep '^zz_' || true
rm -f zz_b.tmp zz_B.tmp
echo "cleaned"
