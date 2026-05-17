# modore

![demo](docs/clip.gif)

English: [README.en.md](README.en.md)

モードレスな日本語IME。ローマ字をそのまま打って変換ホットキーを押すと、
カーソル位置の単語がMozcのトップ候補に置き換わる。モード切り替えも変換
中の下線UIもなし、必要なときだけ呼び出すスタイル。

## ステータス

| OS      | 状態                                                                  |
| ------- | --------------------------------------------------------------------- |
| macOS   | 動作中。ホットキー設定、候補サイクル、Escで取り消し、候補パネル対応   |
| Linux   | 動作中。X11 grab + AT-SPI2 + Unixソケット IPC + Waylandフォールバック |
| Windows | 未着手                                                                |

機能比較表は [docs/PARITY.md](docs/PARITY.md)。機能の追加・削除と
同じコミットで更新すること。

## ビルドと起動

```sh
make             # 使えるターゲット一覧を表示
make build       # ホストアプリをビルド
make run         # ビルドして起動（Linux + macOS）
make open        # macOSのみ：.appバンドルを開く
```

初回ビルドではMozc + protobuf + abseilのソース（合計 ~150 MB）と
Mozc OSS辞書（~48 MB）をダウンロードする。2回目以降はインクリメンタル
ビルドで数秒。

**macOS**: 初回起動時にアクセシビリティ権限を要求される。フォーカス中の
テキストフィールドを読み書きするために必須なので、*システム設定 →
プライバシーとセキュリティ → アクセシビリティ* で許可してから起動し
直す。起動後はメニューバーに **ﾓﾄﾞﾚ** ラベルが表示され、メニューには
現在のホットキー、配送経路（Carbon / イベントタップ・フォールバック）、
設定の編集と場所表示、終了が並ぶ。他のアプリがSecure Keyboard Entry
（sudoプロンプトやパスワード欄）を保持している間はラベルが赤くなる。
OSがその間キー注入をブロックするため、どのアプリが原因かメニューから
すぐ確認できる。

**Linux**: GUIセッションから起動すること（AT-SPIがセッションD-Busを
使うため）。Waylandコンポジタ、Hyprlandのバインド、`--trigger`ソケット、
Chromium / Electron周りの癖、systemdユーザーユニットは
[docs/linux.md](docs/linux.md) を参照。

`zsh`、`bash`、`fish` の 対応 は
[docs/shell-integration.md](docs/shell-integration.md) を参照。

## 設定

`~/.config/modore/modore.conf`（または `$XDG_CONFIG_HOME/modore/modore.conf`）、
INI形式:

```ini
[conversion]
hotkey = Ctrl+Shift+grave
# [bridge]
# mozc_backend = built-in
```

デフォルトはmacOSが `Cmd+Semicolon`、Linuxが `Super+Semicolon`。ホットキー
の文法、修飾キーのエイリアス、キー名一覧は
[docs/configuration.md](docs/configuration.md) を参照。

`[bridge] mozc_backend` で変換バックエンドを選べる。`built-in`
（デフォルト、同梱Mozcをプロセス内で使用。`oss` も互換エイリアス）または
`google_ime`（システムに入っている Google 日本語入力サービス）を
指定する。この変更はアプリ再起動後に反映される。

## 構成

```
bridge/             Mozc用のC ABIラッパー。クロスプラットフォーム、CMakeビルド。
engine/             Lua スクリプティング + ML分類器（ローマ字/ASCII判定）。
native/macos/       Swiftホスト：イベントタップ + アクセシビリティ + クリップボードフォールバック。
native/linux/       C++ホスト：X11 grab + Unixソケット IPC + AT-SPI2 + クリップボードフォールバック。
tools/              学習スクリプト（train_classifier.py）と学習データ。
third_party/        fcitx5-mozcサブモジュール（Mozcエンジンのビルドを提供）。
```

bridgeは共有ライブラリ（macOSは `libmozc_bridge.dylib`、Linuxは
`libmozc_bridge.so`、~25 MB）で、Mozcエンジン・abseil・protobufを静的に
リンクしている。フロントエンドからは `bridge/include/mozc_bridge.h` の
フラットなC ABIだけ叩けば動く。

## 参考

- [espanso](https://github.com/espanso/espanso) — Carbonホットキーの配送、
合成イベントのマーカー、修飾キーの解放待ち、Unicode注入のチャンキング、
SecureInputウォッチャー。
- [OpenKey](https://github.com/tuyenvm/OpenKey) — 合成キーイベントを
セッションタップに投げる位置（Chromium / Electronまで届く経路）。
- [ibus-hiragana](https://github.com/esrille/ibus-hiragana) — 変換時の
UX周り（候補サイクル、MRU履歴、送り仮名の扱い、変換ごとのカタカナ修飾）。
- [Yukino Ikegami](https://github.com/ikegami-yukino),
  [Setsuo Tsuruta](https://nrid.nii.ac.jp/nrid/1000000366395/),
  [*Hybrid method for modeless Japanese input using N-gram based binary
  classification and dictionary*](https://doi.org/10.1007/s11042-013-1805-1),
  Multimedia Tools and Applications, 2015 —
  ローマ字/ASCII自動判定のMLモデル（SVM + n-gram特徴量 + 非日本語辞書）は
  この論文をベースに実装。

## 必要なもの

- CMake 3.22+
- Python 3（Mozcのビルドスクリプトが `python` を呼ぶ）
- **macOS**: Xcode Command Line Tools
- **Linux**: C++20対応のGCC / Clang、X11 + XTest開発ヘッダ（`libX11`,
`libXtst`）、AT-SPI（`atspi-2`, GLib）、`pkg-config`。クリップボード
補助はX11なら `xclip`、Waylandなら `wl-clipboard`（`wl-paste` /
`wl-copy`）。

## ライセンス

MIT、[LICENSE](LICENSE) 参照。同梱のサードパーティ（Mozc、fcitx5-mozc、
abseil-cpp、protobuf）はBSD-3-Clauseで、詳細は
[bridge/NOTICE.md](bridge/NOTICE.md)。
