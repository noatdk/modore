# modore

![demo](docs/clip.gif)

English: [README.en.md](README.en.md)

モードレスな日本語IME。ローマ字をそのまま打ってホットキー（デフォルトは
`Ctrl+/`）を押すと、カーソル位置の単語がMozcのトップ候補に置き換わる。
モード切り替えなし、変換中の下線UIなし、必要なときだけ呼ぶ方式。

## ステータス

| OS      | 状態                                                                 |
| ------- | -------------------------------------------------------------------- |
| macOS   | 動作中。ホットキー設定、候補サイクル、Escでアンドゥ、候補パネル付き    |
| Linux   | 動作中。X11 grab + AT-SPI2 + Unixソケット IPC + Waylandフォールバック |
| Windows | 未着手                                                               |

機能比較は [docs/PARITY.md](docs/PARITY.md)。機能を足したり外したりした
コミットで一緒に更新すること。

## ビルドと起動

```sh
make             # 使えるターゲット一覧を表示
make build       # ホストアプリをビルド
make run         # ビルドして起動（Linux + macOS）
make open        # macOSのみ：.appバンドルを開く
```

初回ビルドだけMozc + protobuf + abseilのソース（合計 ~150 MB）を取ってきて、
Mozc OSS辞書（~48 MB）も落としてくる。2回目以降はインクリメンタルで数秒。

**macOS**: 初回起動でアクセシビリティ権限を聞いてくる。フォーカス中の
テキストフィールドを読み書きするのに必須なので、*システム設定 →
プライバシーとセキュリティ → アクセシビリティ* で許可して起動し直す。
起動後はメニューバーに **ﾓﾄﾞﾚ** ラベルが出る。メニューには現在のホット
キー、配送経路（Carbon / イベントタップ・フォールバック）、設定の編集や
場所表示、終了が並んでいる。他アプリがSecure Keyboard Entry（sudoの
プロンプトやパスワード欄）を握っている間はラベルが赤くなる。OSがその間
キー注入をブロックするので、どのアプリを離せばいいかメニューですぐわかる。

**Linux**: GUIログインから起動する（AT-SPIがセッションD-Busを使うため）。
Waylandコンポジタ、Hyprlandのバインド、`--trigger`ソケット、Chromium /
Electron周りの癖、systemdユーザーユニットは [docs/linux.md](docs/linux.md)
に書いてある。

## 設定

`~/.config/modore/modore.conf`（または `$XDG_CONFIG_HOME/modore/modore.conf`）。
INI形式。今のところ `[conversion]` セクションだけ：

```ini
[conversion]
hotkey = Ctrl+Shift+grave
```

デフォルトは `Ctrl+Slash`。ホットキー文法、修飾キーの別名、キー名は
[docs/configuration.md](docs/configuration.md) に。

## 構成

```
bridge/             Mozcを包むC ABIラッパー。クロスプラットフォーム、CMakeビルド。
native/macos/       Swiftホスト：イベントタップ + アクセシビリティ + クリップボードフォールバック。
native/linux/       C++ホスト：X11 grab + Unixソケット IPC + AT-SPI2 + クリップボードフォールバック。
third_party/        fcitx5-mozc submodule（Mozcエンジンのビルドはここ経由）。
```

bridgeは共有ライブラリ（macOSは `libmozc_bridge.dylib`、Linuxは
`libmozc_bridge.so`、~25 MB）。Mozcエンジン・abseil・protobufは静的に
リンク済み。フロントエンドからは `bridge/include/mozc_bridge.h` の
フラットなC ABIだけ叩けばよい。

## 参考

実装で参考にしたもの：

- [espanso](https://github.com/espanso/espanso) — Carbonホットキー配送、
合成イベントの目印、修飾キー解放待ち、Unicode注入のチャンキング、
SecureInputウォッチャー。
- [OpenKey](https://github.com/tuyenvm/OpenKey) — 合成キーイベントを
セッションタップに流す位置（Chromium / Electronまで届く経路）。
- [ibus-hiragana](https://github.com/esrille/ibus-hiragana) — 変換まわりの
UX（候補サイクル、MRU履歴、送り仮名、変換ごとのカタカナ修飾）。

## 必要なもの

- CMake 3.22+
- Python 3（Mozcのビルドスクリプトが `python` を呼ぶ）
- **macOS**: Xcode Command Line Tools
- **Linux**: C++20が通るGCCかClang、X11 + XTest開発ヘッダ（`libX11`,
`libXtst`）、AT-SPI（`atspi-2`, GLib）、`pkg-config`。クリップボード
補助はX11なら `xclip`、Waylandなら `wl-clipboard`（`wl-paste` /
`wl-copy`）。

## ライセンス

MIT、[LICENSE](LICENSE) 参照。同梱の3rdパーティ（Mozc、fcitx5-mozc、
abseil-cpp、protobuf）はBSD-3-Clause、詳細は [bridge/NOTICE.md](bridge/NOTICE.md)。
