# OtonumaASIO

**音沼の住民に向けた、DAC への受け渡しを細かく追い込むための仮想 ASIO ドライバ**

再生ソフトからは普通の ASIO ドライバ 1 本に見え、その裏で DAC への受け渡し方式（ASIO 中継・WASAPI 排他・Kernel Streaming パケット / WaveRT）とバッファ・周期を、**再生しながら切り替えて好みの音を探す**ための仮想 ASIO ドライバです。

A virtual ASIO driver for Windows that lets you switch the output path to your DAC (ASIO relay / WASAPI exclusive / Kernel Streaming packet & WaveRT) and tune buffer sizes and hand-off rates while playing. Bit-perfect, user-mode only (no kernel driver, no driver signing). Japanese UI.

- ビットパーフェクト（音量・ディザ・リサンプルなし。パイプライン内のビット一致を常時検査）
- ユーザーモードだけで完結（カーネルドライバなし・署名不要）
- DSD 対応（ASIO ネイティブ DSD で受け取り、ネイティブ / DoP で出力）
- コントロールパネルで起床間隔のばらつき・アンダーラン・ビット一致を表示
- プリセットとブラインド A/B（ABX、二項検定つき）
- Windows 10 / 11 x64（32bit 再生ソフト用の 32bit 版 DLL も同梱）
- Steinberg ASIO SDK 非依存（IASIO は自前定義）

## ダウンロードとインストール

1. [Releases](../../releases) から `OtonumaASIO-v*-win.zip` をダウンロードして展開します。
2. `install.bat` をダブルクリックします（管理者として自動で実行し直し、`C:\Program Files\OtonumaASIO\` へのコピー・ASIO 登録・スタートメニューのショートカット作成を行います）。
3. 再生ソフトの出力先に **OtonumaASIO** を選び、コントロールパネル（`OtonumaPanel.exe`、タスクトレイに常駐）で設定します。

アンインストールはスタートメニューの「OtonumaASIO アンインストール」または `uninstall.bat` で行います。

詳しくは [マニュアル（docs/manual.html）](docs/manual.html) を見てください。ダウンロードした zip にも同じものが入っています。

## ビルド

- Visual Studio 2026（ツールセット v145）: `OtonumaASIO.sln` または `OtonumaASIO.slnx` を開き、Release の x64 と x86 をビルド
- CMake: `cmake -B build -A x64 && cmake --build build --config Release`

## 構成

```
OtonumaASIO.sln / .slnx   Visual Studio 2026 ソリューション
CMakeLists.txt                CMake（MSVC / mingw-w64）
vs/                           各プロジェクト (.vcxproj)
src/common/                   自前 IASIO 定義、設定（JSON）、共有定義
src/front/                    IASIO 実装、COM 登録
src/engine/                   リングバッファ、ポンプ、計測、操作パイプ
src/backend/                  ASIO 中継 / WASAPI / KS（パケット・WaveRT）/ null
panel/OtonumaPanel.cpp             コントロールパネル
tools/otonumareg.cpp               ASIO 登録・解除
tools/asiohosttest.cpp        簡易 ASIO ホスト（結合試験・ビット一致検査）
scripts/install.bat           インストール（Shift_JIS / CRLF）
scripts/uninstall.bat         アンインストール（Shift_JIS / CRLF）
docs/manual.html              マニュアル
```

## 旧名について

1.0.x までは「BareStream Local」という名前でした。`install.bat` は旧名の登録・常駐・ショートカット・フォルダを自動で片付け、設定とプリセットは初回起動時に `%APPDATA%\OtonumaASIO\settings.json` へ自動で引き継ぎます。

## ライセンス

MIT License © 2026 Imaha486。ASIO は Steinberg Media Technologies GmbH の商標です。
