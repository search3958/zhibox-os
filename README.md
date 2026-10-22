# zhibox-os — openEuler カーネル (OLK-5.10) ブータブル環境

`https://gitee.com/openeuler/kernel.git` の **OLK-5.10** を取得し、
**QEMU 上で実際にブートできる**状態まで構築したワークスペース。

ビルドは **WSL (Ubuntu)** と **macOS (Apple Silicon, ネイティブ)** の両方に対応しています。
スクリプトが `uname` で自動判定して分岐します。Linux コンテナや VM は使いません。

## 構成

| パス | 内容 | サイズ |
|---|---|---|
| `kernel/` | カーネルソース (74,490ファイル) | 1.1GB |
| `kernel.git/` | クローンの git 履歴（shallow, `OLK-5.10` @ `1cd35e303`） | 219MB |
| `scripts/` | ビルド・起動・検証スクリプト | 数KB |
| `output/` | ビルド成果物（`bzImage`, `config`, `System.map`, `initramfs.cpio.gz`, `rootfs.img`, `boot.log`）※`.gitignore` 対象 | 17MB + rootfs |
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
bash scripts/tools/k.sh status
bash scripts/tools/k.sh log --oneline -5
```

## 必要な環境

### WSL: Ubuntu 22.04

- vhdx は `D:\wsl\Ubuntu-22.04`（C: の空きを守るため D: 配置）
- gcc 11.4 / make / flex / bison / libelf / libssl / bc / cpio / dwarves / rsync / git
- **qemu-system-x86_64** + **qemu-system-gui**（画面表示用 GTK/SDL）
- **busybox-static**（initramfs 用 = distro パッケージ）
- ゲスト WebKitGTK 用 rootfs の作成（初回のみ）:
  `debootstrap` / `e2fsprogs`（macOS は `e2fsprogs` と QEMU だけでよい）。
  WebKitGTK 本体は**ゲスト rootfs の中**に入るため、ホスト側に
  `libwebkit2gtk` を入れる必要はない
- ビルドツリーは WSL の ext4 上 `~/kernel-src` に展開して使用
  （Windows の 9p マウントは遅く、かつ大文字小文字の制約を受けるため）

### macOS: Apple Silicon（ネイティブ、コンテナ不要）

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils qemu make gnu-sed gawk coreutils findutils e2fsprogs
```

- カーネルはフリースタンディングなので、`*-elf-gcc`（ベアメタル向け）でクロスコンパイルできる
- APFS は大文字小文字を区別しないため、**ケースセンシティブ APFS のスパースバンドル**
  (`~/zhibox-kernel.sparsebundle` → `/Volumes/zhibox-kernel`) を初回に自動作成し、
  その中の `/Volumes/zhibox-kernel/src` でビルドする（sudo 不要・使用分だけ拡張）
- ホストツール用に `scripts/host/darwin/` の `elf.h` / `byteswap.h` / `endian.h` を
  差し込むほか、新しめの GCC/binutils 向けの小さなソース修正をスクリプト内で自動適用する
  （すべて冪等。詳細は `scripts/build-and-boot.sh` の macOS ブロック）
- initramfs の `gradient`（フレームバッファ描画デモ）も macOS で生成できる。Linux 用の
  静的 libc が無いため、`scripts/host/darwin/linux-shim/`（syscall 直叩きの最小 libc 代替）と
  `x86_64-elf-gcc` で freestanding な静的 ELF としてクロスビルドする（追加依存なし）
- HTML+CSS は **QEMU ゲスト内の Linux で WebKitGTK を実行**して描画する。
  `output/rootfs.img` は WSL/Linux の `debootstrap` でも、macOS の
  `scripts/build-rootfs.sh`（QEMU ゲスト内で apt）でも作れる。
  rootfs が無い場合は `WEBKIT` はプレースホルダーになる
- QEMU は TCG（ソフトウェアエミュレーション）で動くためブートに数分かかる

## ビルド + 起動確認（1コマンド）

```bash
# WSL / macOS 共通 (リポジトリ直下で実行)
bash scripts/build-and-boot.sh
```

```powershell
# Windows からは WSL 経由 (パスは環境に合わせて)
wsl -d Ubuntu-22.04 -u root -e bash /mnt/c/.../zhibox-os/scripts/build-and-boot.sh
```

1. git オブジェクトから**完全な**ツリーを展開（macOS はスパースバンドルをマウント）
2. `make defconfig` → 3. `make -j bzImage` → 4. busybox initramfs 生成
5. **QEMU でブート確認**（`ZHIBOX_BOOT_OK` を判定し自動電源オフ）→ 6. `output/` へ書き出し

- 2回目以降はツリーを再利用し差分のみ
- リポジトリ側の編集を取り込む: `SYNC=1 bash scripts/build-and-boot.sh`（`--size-only` 高速同期）
- ツリー作り直し: `bash scripts/host/wsl/reextract.sh`
- 途中経過: 別窓から `tail -f output/build.log`

## 起動方法

**画面付き（GUI ウィンドウ）** — WSL は WSLg 経由、macOS はネイティブ QEMU ウィンドウ:

```bash
bash scripts/boot-gui.sh
```

GUI 起動時はフレームバッファのデモ (`gradient`) が自動で起動します。

- **壁紙**: `wallpaper.png` を画面いっぱい (cover) に縮小表示
- **文字**: `HarmonyOS_Sans_SC_Regular.ttf` を TrueType レンダリングで描画
- **マウス**: QEMU ウィンドウ内をクリックしてポインタをグラブ（PS/2 マウス）
- **ウィンドウ**: タイトルバーをドラッグで移動、クリックで最前面へ
- **HTML+CSS**: 4 番目の `WEBKIT` ウィンドウに表示（ゲスト Linux の WebKitGTK / JavaScript 無効）
- **hover**: `WEBKIT` ウィンドウ内のカード / ピル / ロゴにマウスを重ねると、
  CSS `:hover` の移動・発光・トランジションがリアルタイムに反映される
- **終了**: `q` / `Esc` / `Ctrl+C` でデモを終了し、busybox シェルに戻る

### GPU アクセラレーション (virtio-gpu)

GUI 起動では QEMU の **virtio-gpu** (`-vga virtio`) を使います。

- カーネルは `CONFIG_VIRTIO_PCI` / `CONFIG_DRM_VIRTIO_GPU` を有効化
- `gradient` は `/dev/fb0`（virtio-gpu の GEM バッファ）に描画し、
  DRM の `DIRTYFB` で**変更された矩形だけ**をホストへ転送する
  （bochs の VRAM への MMIO 書き込みより大幅に速い）
- `DIRTYFB` に対応していないドライバでは、自動的に従来の直接描画へフォールバック
- 起動ログに `gradient: drm acceleration on (virtio-gpu)` と出れば加速パスが有効
- なお Homebrew の QEMU (macOS) は virglrenderer 非対応のため、
  OpenGL/3D のハードウェアアクセラレーションは対象外（2D の転送高速化まで）

### 壁紙 (PNG) と TrueType フォント

ゲスト側で自前のデコーダを使うため、libpng / zlib / libfreetype は不要です
（macOS の freestanding クロスビルドでもそのまま動きます）。

| ホスト側のファイル | initramfs 内のパス | 用途 |
|---|---|---|
| `scripts/guest/wallpaper.png` | `/usr/share/zhibox/wallpaper.png` | 壁紙 |
| `scripts/guest/HarmonyOS_Sans_SC_Regular.ttf` | `/usr/share/zhibox/font.ttf` | UI フォント |

- **PNG**: 8bit・非インターレースの color type 0/2/3/4/6 に対応。
  zlib の inflate と PNG フィルタ復元を内蔵し、RGBA に展開後、
  アスペクト比を保って画面を覆うようにバイリニア縮小（約 80% に減光）
- **TrueType**: `head` / `maxp` / `hhea` / `hmtx` / `cmap` (format 4/12) /
  `loca` / `glyf` を読み、2 次ベジェを整数 26.6 固定小数点で
  4 倍垂直スーパーサンプリング + 水平被覆率により描画
- フォントや壁紙が見つからない場合は、従来のグラデーション背景と
  5x7 ビットマップフォントに自動フォールバック
- 起動ログで確認できます:
  ```
  gradient: font loaded (TrueType)
  gradient: wallpaper 1920x1080 (PNG)
  ```
- **差し替え方法**: 上記 2 ファイルを置き換えて initramfs だけ再生成すれば
  反映されます（カーネルの再ビルドは不要）:
  ```bash
  bash scripts/build-initramfs.sh
  cp ~/initramfs.cpio.gz output/initramfs.cpio.gz   # GUI 起動が参照する場所
  bash scripts/boot-gui.sh
  ```

### HTML+CSS ウィンドウ (ゲスト Linux の WebKitGTK / JavaScript 無効)

`scripts/guest/web/index.html` を **QEMU ゲスト内の Linux** で WebKitGTK に
描画させ、PNG を介さず raw RGBA のまま `WEBKIT` ウィンドウに表示します。
ホスト (macOS/WSL) ではレンダリングしません。

```
rootfs.img (Debian/Ubuntu + WebKitGTK + Xvfb)  -- IDE /dev/sda
        |
        +-- guest init: ext4 を ro マウント -> chroot (GUI では常駐)
              +-- launch.sh: Xvfb :99 + dbus-run-session
                    +-- WebKitGTK (JavaScript OFF) -> /out/html.raw
                    +-- /out/html.version を原子的に更新
        |
        +-- /out は initramfs の /tmp に bind
              +-- fb-gradient: バイリニア縮小 -> /dev/fb0 の WEBKIT ウィンドウ
              +-- fb-gradient -> /tmp/html.input (ポインタ座標)
```

- **レンダリング場所**: ゲスト Linux の WebKitGTK (GTK3)。macOS の WKWebView や
  ホスト側のビルド時レンダリングは使いません
- **JavaScript 無効**: `WebKitSettings:enable-javascript=FALSE` と HTML の CSP の
  両方で禁止。ページ内の `<noscript>` バッジで確認できます。hover は CSS だけで
  動くため、JavaScript を有効にする必要はありません
- **Xvfb**: `/dev/fb0` は `fb-gradient` が占有したまま、WebKitGTK は Xvfb 上の
  通常の Gtk ウィンドウに描画します（Xorg を fb0 に載せない）
- **hover の経路**: `fb-gradient` がマウス位置を `/tmp/html.input` に書き、
  ゲスト側の `render-html.py` が `libXtst` の XTEST で Xvfb のポインタを動かします。
  WebKit が `:hover` を再評価して raw を書き直し、`/tmp/html.version` の
  フレーム番号が変わったときだけ `fb-gradient` が raw を読み直します
- **実行タイミング**: GUI 起動ではレンダラを常駐させ、hover のたびに更新します。
  シリアル起動では従来どおり 1 枚だけ描画して終了します。ページを変更したら
  rootfs を作り直してください（カーネルの再ビルドは不要）
- **rootfs が無い場合**: `WEBKIT` ウィンドウは `HTML RENDER UNAVAILABLE` の
  プレースホルダーになります。ホスト側で代わりに描画することはしません

#### 1. ゲスト rootfs を作る (初回のみ)

**WSL / Linux**:

```bash
sudo apt install debootstrap e2fsprogs
sudo bash scripts/build-rootfs.sh
# -> output/rootfs.img (ext4, パーティションなし, 既定 2G)
```

- `debootstrap` で amd64 の最小 Debian/Ubuntu を作り、その中に
  WebKitGTK (4.0/4.1 を自動判定) / Python GI / Xvfb / libXtst / DejaVu フォントを入れます
- ホストの suite を既定で使います。変える場合:
  `ZH_ROOTFS_SUITE=bookworm sudo -E bash scripts/build-rootfs.sh`
- rootfs の作業ディレクトリは既定で `$HOME/zhibox-rootfs`
  （WSL の ext4 上。NTFS の `/mnt/c` では chroot できないため）

**macOS**:

```bash
brew install qemu e2fsprogs
bash scripts/build-rootfs.sh
# -> output/rootfs.img
```

- Linux コンテナやホスト側 WebKitGTK は使いません。Ubuntu 24.04 の公式
  base rootfs を ext4 イメージにし、**そのイメージを QEMU で起動して
  ゲスト自身に apt させる**ため、macOS でも同じ rootfs を作れます
- 初回は Ubuntu base の取得と WebKitGTK の導入で 10〜20 分ほどかかります
- 途中経過は `output/build/rootfs-guest/firstboot.log` に出ます
- `sudo` は不要です。`output/bzImage` を先に作ってください

#### 2. initramfs を作り直して起動

```bash
bash scripts/build-initramfs.sh
cp ~/initramfs.cpio.gz output/initramfs.cpio.gz
bash scripts/boot-gui.sh
```

`boot-qemu.sh` は `output/rootfs.img` があれば自動で

- IDE ディスク (`-drive ...,if=ide`) として接続
- メモリを 2G に増やす
- カーネルコマンドラインに `zh_html_root=1` を追加

します。rootfs が無い場合は 1G のまま、プレースホルダーで起動します。

GUI 起動ログに次のように出れば、ゲスト内の WebKitGTK と live hover が
動いています:

```
guest html: rootfs /dev/sda
guest html: live renderer starting
guest-html: Xvfb :99 640x400 live=1
render-html: WebKit2-2.52, JavaScript=False, live=True
render-html: 640x400 RGBA -> /out/html.raw (...) frame=1
guest html: live ready (pid ...)
gradient: html 640x400 (raw RGBA, JS off)
gradient: html 640x400 -> 500x313 (raw RGBA)
gradient: html frame 2        # マウスを重ねたとき
```

シリアル起動 (`zh_gui=1` なし) では `guest html: /tmp/html.raw ready` の後、
1 枚だけ描画してシェルに戻ります。

> 検証済み: Ubuntu 24.04 base + WebKitGTK 2.52.6 + Xvfb。QEMU ゲスト内で
> 640x400 RGBA を生成して `WEBKIT` ウィンドウに表示し、QEMU monitor の
> `mouse_move` でカードに重ねたときに `:hover` の浮き上がりと発光が
> フレームバッファへ反映されるところまで確認。

#### raw 形式 (リトルエンディアン)

| オフセット | 内容 |
|---|---|
| 0 | magic 8 bytes `ZHRAW1\0\0` |
| 8 | u32 width |
| 12 | u32 height |
| 16 | u32 format (0 = RGBA8888) |
| 20 | u32 stride (bytes) |
| 24 | RGBA 画素 (先頭が上端) |

live モードでは raw のほかに次の共有ファイルを使います。

| パス (initramfs / guest) | 内容 |
|---|---|
| `/tmp/html.raw` / `/out/html.raw` | 現在のフレーム (上記形式) |
| `/tmp/html.version` / `/out/html.version` | u32 フレーム番号。raw 更新後に原子的に置換 |
| `/tmp/html.input` / `/out/html.input` | `x y` (640x400 座標)。負値は hover 解除 |
| `/tmp/html.stop` / `/out/html.stop` | GUI 終了時に init が作り、レンダラが終了する |

- 既定の描画サイズは 640x400。変える場合は
  `scripts/guest/web/launch.sh` の既定値を編集して rootfs を作り直します
- `ZH_HTML_RAW=/path/to/raw` を指定して `build-initramfs.sh` を実行すると、
  デバッグ用に initramfs へ raw を埋め込めます（通常の経路ではありません）

### シリアルコンソール（対話・-nographic）

```bash
bash scripts/boot-qemu.sh
```

- シリアル起動ではデモは自動起動しません（`gradient` と入力すると起動、`q` + Enter で終了）
- どちらも `/` に着地し busybox シェルが待機（`Linux (none) 5.10.0 ...`）
- 終了: 画面の `poweroff -f`、または `Ctrl+A` → `X`（nographic 時）
- macOS は TCG のため起動に数分かかる（スクリプトは最大300秒待つ）

## 運用上の注意

### 1. git の取得元

gitee 直結のクローンはこの回線で**パック転送が必ず途中切断**する（`early EOF` を3回確認）ため、
openEuler 公式ミラー **`github.com/openeuler-mirror/kernel`** から同一ブランチを取得した。

```
origin  = https://gitee.com/openeuler/kernel.git        # ご要望の取得元
mirror  = https://github.com/openeuler-mirror/kernel.git # 実際に取得した公式ミラー
```

gitee が安定するまでは `git --git-dir=kernel.git --work-tree=kernel fetch --depth 1 mirror OLK-5.10` 推奨。

### 2. 大文字小文字非区別ファイルシステムによる 13 ファイルの欠落 (NTFS / APFS 共通)

カーネルには `xt_CONNMARK.h` / `xt_connmark.h` のような**大文字小文字だけ違う組が13組（26ファイル）**あり、
Windows の NTFS も macOS の APFS（既定）も非区別のため **`kernel/` には片方しか存在できない**（この環境では区別ディレクトリ化も無効だった）。

- `kernel/` は**参照用**（13ファイル欠落）。`git status` が怒らないよう元のリポジトリ側で `skip-worktree` 設定済み
- **完全な26ファイルは `kernel.git/` のオブジェクトに保存されており**、ビルドはそこから展開するためビルド結果には影響しない
- 完全なツリーの実体: WSL は `~/kernel-src`（ext4）、macOS は `/Volumes/zhibox-kernel/src`
  （ケースセンシティブ APFS スパースバンドル。スクリプトが初回に自動作成・マウント）

### 3. コミット / push について

- 本リポジトリは **ローカルコミットのみ**（push 未実施）。
- `kernel.git/objects/pack/*.pack` は **約208MB の単一ファイル**で、**GitHub の 100MB 上限を超える**。
  push したい場合は、先に `/kernel.git/` を `.gitignore` に追加して除外するか、履歴を持ち込まないこと。
- カーネルソース一式は約1.3GB。push は非推奨（GitHub の推奨は1GB以下・警告は5GB）。

### 4. ソースを変更する場合

- ビルドするならビルド用ツリー側で編集するのが確実
  （WSL: `~/kernel-src`、macOS: `/Volumes/zhibox-kernel/src`）
- リポジトリ側 `kernel/` で編集した場合: `SYNC=1` 付きでビルドスクリプトを再実行すると取り込む

## スクリプト一覧

| パス | 用途 |
|---|---|
| `scripts/build-and-boot.sh` | ビルド → initramfs → QEMU ブート確認 → 成果物書き出し |
| `scripts/build-rootfs.sh` | ゲスト Linux 用 rootfs.img (WebKitGTK + Xvfb) を生成。macOS は QEMU ゲスト内 apt に分岐 |
| `scripts/build-rootfs-guest.sh` | macOS 専用: Ubuntu base + QEMU ゲストで rootfs.img を生成 |
| `scripts/build-initramfs.sh` | busybox + gradient の initramfs を生成 (HTML raw は同梱しない) |
| `scripts/boot-qemu.sh` | QEMU 起動。rootfs.img があれば IDE 接続してゲスト描画を有効化 |
| `scripts/boot-gui.sh` | **画面付き (GUI)** で起動（WSL は WSLg / macOS はネイティブ） |
| `scripts/lib/common.sh` | OS 判定 (WSL/macOS)・パス・ツール検出の共通処理 |
| `scripts/guest/` | **ゲスト (Linux) 側**で動くもの: `init` (PID 1) / `fb-gradient.c` / `zh_font.*` (TrueType) / `zh_png.*` (PNG) / `web/index.html` / `web/render-html.py` / `web/launch.sh` / フォント・壁紙アセット |
| `scripts/host/darwin/` | **macOS 専用**: ホスト用ヘッダ (`elf.h` 等) / `linux-shim/` |
| `scripts/host/wsl/` | **WSL 専用**: NTFS・大文字小文字衝突まわりの補助 |
| `scripts/tools/` | ホスト共通ツール: `k.sh` / `case-collision-report.sh` |

> ディレクトリ構成の詳細と、新しいスクリプトを置く場所は `scripts/README.md` を参照。
