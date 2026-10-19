# zhibox-os — openEuler カーネル (OLK-5.10) ブータブル環境

`https://gitee.com/openeuler/kernel.git` の **OLK-5.10** を取得し、
**QEMU 上で実際にブートできる**状態まで構築したワークスペース。

## 構成

| パス | 内容 | サイズ |
|---|---|---|
| `kernel/` | カーネルソース (74,490ファイル) | 1.1GB |
| `kernel.git/` | クローンの git 履歴（shallow, `OLK-5.10` @ `1cd35e303`） | 219MB |
| `scripts/` | ビルド・起動・検証スクリプト | 数KB |
| `output/` | ビルド成果物（`bzImage`, `config`, `System.map`, `initramfs.cpio.gz`, `boot.log`）※`.gitignore` 対象 | 17MB |
| `README.md` / `.gitignore` / `build.log` | ドキュメント・設定・ビルドログ | — |

> **レイアウトの注意**: `kernel/.git` は `kernel.git/` に分離してあります。
> リポジトリに「ファイルとして」カーネルを含めるため（同居すると git が
> サブモジュール化してファイル本体をコミットしなくなるため）。
> git 操作は必ず `--git-dir` と `--work-tree` を同時に指定します:

```powershell
# Windows (PowerShell / git for Windows)
git --git-dir=kernel.git --work-tree=kernel status
git --git-dir=kernel.git --work-tree=kernel log --oneline -5
git --git-dir=kernel.git --work-tree=kernel diff
```
```bash
# WSL 内ならラッパーで自動付与
bash scripts/k.sh status
bash scripts/k.sh log --oneline -5
```

## 必要な環境（すべて導入済み）

- **WSL: Ubuntu 22.04** — vhdx は `D:\wsl\Ubuntu-22.04`（C: の空きを守るため D: 配置）
  - gcc 11.4 / make / flex / bison / libelf / libssl / bc / cpio / dwarves / rsync / git
  - **qemu-system-x86_64** + **qemu-system-gui**（画面表示用 GTK/SDL）
  - **busybox-static**（initramfs 用）
- ビルドツリーは WSL の ext4 上 `/root/kernel-src` に展開して使用
  （Windows の 9p マウントは遅く、かつ大文字小文字の制約を受けるため）

## ビルド + 起動確認（1コマンド）

```powershell
wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/build-and-boot.sh"
```

1. git オブジェクトから**完全な**ツリーを WSL ext4 へ展開
2. `make defconfig` → 3. `make -j$(nproc) bzImage` → 4. busybox initramfs 生成
5. **QEMU でブート確認**（`ZHIBOX_BOOT_OK` を判定し自動電源オフ）→ 6. `output/` へ書き出し

- 2回目以降はツリーを再利用し差分のみ（数分）
- Windows 側の編集を取り込む: `SYNC=1` を先頭に付けて実行（`--size-only` 高速同期）
- ツリー作り直し: `wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/.../scripts/reextract.sh"`
- 途中経過: 別窓から `Get-Content build.log -Tail 30 -Wait`

## 起動方法

**画面付き（GUI ウィンドウ）** — WSLg 経由で Windows デスクトップに表示:

```powershell
wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/boot-gui.sh"
```

**シリアルコンソール（対話・-nographic）**:

```powershell
wsl -d Ubuntu-22.04 -u root -e bash "/mnt/c/Users/Sentaro Takenaka/Documents/GitHub/zhibox-os/scripts/boot-qemu.sh"
```

- どちらも `/` に着地し busybox シェルが待機（`Linux (none) 5.10.0 ...`）
- 終了: 画面の `poweroff -f`、または `Ctrl+A` → `X`（nographic 時）

## 運用上の注意

### 1. git の取得元

gitee 直結のクローンはこの回線で**パック転送が必ず途中切断**する（`early EOF` を3回確認）ため、
openEuler 公式ミラー **`github.com/openeuler-mirror/kernel`** から同一ブランチを取得した。

```
origin  = https://gitee.com/openeuler/kernel.git        # ご要望の取得元
mirror  = https://github.com/openeuler-mirror/kernel.git # 実際に取得した公式ミラー
```

gitee が安定するまでは `git --git-dir=kernel.git --work-tree=kernel fetch --depth 1 mirror OLK-5.10` 推奨。

### 2. NTFS の大文字小文字非区別による 13 ファイルの欠落

カーネルには `xt_CONNMARK.h` / `xt_connmark.h` のような**大文字小文字だけ違う組が13組（26ファイル）**あり、
Windows の NTFS は非区別のため **`kernel/` には片方しか存在できない**（この環境では区別ディレクトリ化も無効だった）。

- `kernel/` は**参照用**（13ファイル欠落）。`git status` が怒らないよう元のリポジトリ側で `skip-worktree` 設定済み
- **完全な26ファイルは `kernel.git/` のオブジェクトに保存されており**、ビルドはそこから展開するためビルド結果には影響しない
- 完全なツリーの実体: WSL の `/root/kernel-src`（ケースセンシティブな ext4）

### 3. コミット / push について

- 本リポジトリは **ローカルコミットのみ**（push 未実施）。
- `kernel.git/objects/pack/*.pack` は **約208MB の単一ファイル**で、**GitHub の 100MB 上限を超える**。
  push したい場合は、先に `/kernel.git/` を `.gitignore` に追加して除外するか、履歴を持ち込まないこと。
- カーネルソース一式は約1.3GB。push は非推奨（GitHub の推奨は1GB以下・警告は5GB）。

### 4. ソースを変更する場合

- ビルドするなら WSL 側 `/root/kernel-src` で編集するのが確実
- Windows 側 `kernel/` で編集した場合: `SYNC=1` 付きでビルドスクリプトを再実行すると取り込む

## スクリプト一覧

| ファイル | 用途 |
|---|---|
| `scripts/build-and-boot.sh` | ビルド → initramfs → QEMU ブート確認 → 成果物書き出し |
| `scripts/boot-gui.sh` | **画面付き (GUI)** で起動（WSLg） |
| `scripts/boot-qemu.sh` | シリアルコンソールで対話起動 |
| `scripts/reextract.sh` | ビルド用ツリーを git から丸ごと作り直す |
| `scripts/k.sh` | kernel クローン用 git ラッパー（`--git-dir`/`--work-tree` 自動付与） |
| `scripts/case-collision-report.sh` | 大文字小文字衝突ファイルの列挙 |
| `scripts/fix-missing-files.sh` | 欠落ファイルの復元（NTFS 制約が効かない場合の最終手段） |
| `scripts/wsl-case-test.sh` | NTFS 区別フラグの動作検証 |
