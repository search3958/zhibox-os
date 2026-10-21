# scripts/ の構成

ホストで動くもの・ゲスト (Linux) で動くもの・OS 依存のものを分離しています。
**新しいスクリプトを追加するときは、下の表に従って置き場所を決めてください。**

```
scripts/
├── build-and-boot.sh      # エントリ: ビルド → initramfs → 起動確認
├── build-initramfs.sh     # エントリ: initramfs 生成
├── boot-qemu.sh           # エントリ: シリアル起動
├── boot-gui.sh            # エントリ: GUI 起動
├── lib/
│   └── common.sh          # 共通 bash (OS 判定・パス・ツール検出)
├── guest/                 # ゲスト (Linux) 側で動くもの
│   ├── init               #   PID 1 (busybox sh)
│   └── fb-gradient.c      #   framebuffer デモ (マウス + 簡易ウィンドウ)
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
