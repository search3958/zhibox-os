# scripts/ の構成

ホストで動くもの・ゲスト (Linux) で動くもの・OS 依存のものを分離しています。
**新しいスクリプトを追加するときは、下の表に従って置き場所を決めてください。**

```
scripts/
├── build-and-boot.sh      # エントリ: ビルド → initramfs → 起動確認
├── build-initramfs.sh     # エントリ: initramfs 生成
├── build-rootfs.sh        # エントリ: ゲスト WebKitGTK 用 rootfs.img 生成 (Linux/WSL / macOS 分岐)
├── build-rootfs-guest.sh  #   macOS: Ubuntu base + QEMU ゲスト内 apt で rootfs.img 生成
├── boot-qemu.sh           # エントリ: シリアル起動
├── boot-gui.sh            # エントリ: GUI 起動
├── lib/
│   └── common.sh          # 共通 bash (OS 判定・パス・ツール検出)
├── guest/                 # ゲスト (Linux) 側で動くもの
│   ├── init               #   PID 1 (busybox sh, rootfs マウント / live renderer 管理)
│   ├── patch-rootfs.sh    #   rootfs.img の /opt/zhibox を差分更新する一時 init
│   ├── fb-gradient.c      #   framebuffer デモ (マウス + 簡易ウィンドウ + hover 入力)
│   ├── zh_font.c/.h       #   TrueType パーサ / ラスタライザ (自前実装)
│   ├── zh_png.c/.h        #   PNG デコーダ / inflate (自前実装)
│   ├── web/index.html     #   ゲスト WebKitGTK で描画する HTML+CSS (JS なし)
│   ├── web/render-html.py #   ゲスト内で WebKitGTK スナップショット → raw RGBA (live hover 対応)
│   ├── web/launch.sh      #   ゲスト内で Xvfb + dbus + WebKitGTK を起動 (live フラグ)
│   ├── HarmonyOS_Sans_SC_Regular.ttf  # UI フォント (initramfs へ同梱)
│   └── wallpaper.png      #   壁紙 (initramfs へ同梱)
├── host/                  # ホスト OS に依存するもの
│   ├── darwin/            #   macOS 専用
│   │   ├── elf.h          #     ホストツール用ヘッダ
│   │   ├── byteswap.h
│   │   ├── endian.h
│   │   └── linux-shim/    #     静的 Linux ELF を作る最小 libc
│   └── wsl/               #   WSL / Windows 専用
│       ├── aux-scan.sh
│       ├── fix-missing-files.sh
│       ├── lfs-scan.sh
│       ├── reextract.sh
│       ├── stage-aux.sh
│       ├── staged-audit.sh
│       └── wsl-case-test.sh
└── tools/                 # ホスト共通のユーティリティ
    ├── k.sh               #   kernel.git 用 git ラッパー
    └── case-collision-report.sh
```

ルートの `build.sh` が差分ビルド + QEMU 起動の入口です。`kernel/`・`scripts/guest/`・
`scripts/guest/web/` の変更を検出し、必要な成果物だけを作り直します。

## 置き場所の判断

| 追加するもの | 置き場所 |
|---|---|
| ビルド・起動の入口（ユーザーが直接叩くコマンド） | `scripts/` 直下 |
| 両 OS で使う bash 関数 | `scripts/lib/` |
| **ゲスト (Linux) で実行される**プログラム・設定 | `scripts/guest/` |
| **macOS ホスト専用**（ヘッダ、クロスビルド補助、Darwin 固有処理） | `scripts/host/darwin/` |
| **WSL / Windows ホスト専用**（NTFS、drvfs、大文字小文字関連） | `scripts/host/wsl/` |
| どちらのホストでも使う補助ツール | `scripts/tools/` |

## ルール

- エントリスクリプトは `$REPO_ROOT` を基準にパスを組み立てる
  （`lib/common.sh` を `.` で読み込む。`zsh` ではなく `bash` で実行する）
- ゲスト側 (`guest/`) にはホストのパスや macOS 専用処理を持ち込まない
- OS 依存の処理は `host/<os>/` に閉じ込め、共通スクリプト側では
  `[ "$OS" = macos ]` の分岐から呼ぶだけにする
- `host/wsl/` の既存スクリプトは移行前の経緯で `/mnt/c/...` を直接参照している
  （必要になったら `lib/common.sh` ベースに統一する）
