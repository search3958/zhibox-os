# zhibox-os — openEuler カーネル (OLK-5.10) ブータブル環境

`https://gitee.com/openeuler/kernel.git` の **OLK-5.10** ブランチを取得し、
**QEMU 上で実際にブートできる**状態まで構築したワークスペース。

## 構成

| パス | 内容 |
|---|---|
| `kernel/` | カーネルの git クローン（shallow, `OLK-5.10` @ `1cd35e303`） |
| `scripts/` | ビルド・起動・検証用スクリプト |
| `output/` | ビルド成果物（`bzImage`, `config`, `System.map`, `initramfs.cpio.gz`, `boot.log`） |
| `build.log` | 最後に実行したビルドの全ログ |

## 必要な環境（すべて導入済み）

- **WSL: Ubuntu 22.04** — vhdx は `D:\wsl\Ubuntu-22.04`（C: の空きを守るため意図的に D: に配置）
  - gcc 11.4 / make / flex / bison / libelf / libssl / bc / cpio / dwarves(pahole) / rsync
  - **qemu-system-x86_64**, **busybox-static**, **git**
- ビルドツリーは WSL の ext4 上（`/root/kernel-src`）に展開して使用
  （Windows の 9p マウント経由だと遅く、かつ大文字小文字の制約を受けるため）

## ビルド + 起動確認（1コマンド）

```powershell
wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/build-and-boot.sh"
```

実行内容:
1. git オブジェクトから完全なツリーを WSL ext4 へ展開（差分があれば rsync でオーバーレイ）
2. `make defconfig`
3. `make -j$(nproc) bzImage`
4. busybox の最小 initramfs を生成
5. **QEMU でブート確認**（シリアルコンソールに `ZHIBOX_BOOT_OK` が出るか判定）
6. `output/` へ成果物を書き出し

途中経過は別窓から追える:

```powershell
Get-Content build.log -Tail 30 -Wait
```

## ビルド済みカーネルを起動する

```powershell
wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/boot-qemu.sh"
```

- シェルが `/` に着地し `uname -a` で `5.10.x-openEuler...` と表示される
- 終了: シェルで `poweroff -f`、または `Ctrl+A` → `X`

## 運用上の注意

### 1. git の取得元について

gitee へ直結してのクローンは、この回線で**パック転送が必ず途中切断**する
（`early EOF` / `SSL_ERROR_SYSCALL` を3回連続で確認）ため、
openEuler 公式ミラー **`github.com/openeuler-mirror/kernel`** から同一ブランチを取得した。

```
origin  = https://gitee.com/openeuler/kernel.git   # ご要望どおり gitee
mirror  = https://github.com/openeuler-mirror/kernel.git  # 実際に取得した公式ミラー
```

- 以降の同期は gitee が安定するまではミラー推奨:
  `git -C kernel fetch --depth 1 mirror OLK-5.10`
- `git ls-remote` の時点の tip: gitee `1588d318a` / ミラー `1cd35e303`

### 2. NTFS の大文字小文字非区別による13ファイルの欠落

カーネルソースには、**大文字小文字だけが違う同名ファイルの組が13組**ある
（例: `xt_CONNMARK.h` と `xt_connmark.h`）。
Windows の NTFS は大文字小文字非区別のため、**1ディレクトリに両方は存在できない**。

対処（このワークスペースでは実施済み）:

- 衝突する5ディレクトリに `fsutil file setCaseSensitiveInfo ... enable` を適用
  ※ この環境では Win32 層・WSL(drvfs) 層ともにフラグを無視し、実効はしなかった
- そのため欠落する13パスを `git update-index --skip-worktree` に設定
  → `git status` は常にクリーン、`git pull` も妨げない
- **ビルドは WSL の ext4（ケースセンシティブ）上のツリーで行う**ため、
  26ファイルすべてが正しい内容で存在し、ビルド結果には影響しない

### 3. 自分でソースを変更する場合

- 編集は **WSL 側のツリー**（`/root/kernel-src`）で行うのが最も確実
- Windows 側 `kernel/` で編集した場合は、再実行時に rsync でオーバーレイ同期される
- ツリーを作り直す場合: WSL 内で `rm -rf /root/kernel-src` のうえ再実行

## スクリプト一覧

| ファイル | 用途 |
|---|---|
| `scripts/build-and-boot.sh` | ビルド → initramfs 生成 → QEMU ブート確認 → 成果物書き出し |
| `scripts/boot-qemu.sh` | ビルド済みカーネルの対話的起動 |
| `scripts/case-collision-report.sh` | 大文字小文字衝突ファイルの列挙 |
| `scripts/fix-missing-files.sh` | 欠落ファイルの復元（git から blob を書き戻す） |
| `scripts/wsl-case-test.sh` | NTFS 区別フラグの動作検証 |
