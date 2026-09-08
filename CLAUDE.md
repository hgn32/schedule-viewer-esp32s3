# schedule-viewer-esp32s3プロジェクトルール

Waveshare ESP32-S3-Touch-LCD-7B(ESP32-S3-WROOM-1-N16R8、7インチ1024x600 IPS、
RGB565パラレル16bit。基板を90度回して縦置き・論理600x1024で使う)に、
Outlook予定表を6時間分のタイムラインとして表示するビューア。予定はWi-Fi(STA)経由で
予定配信サーバのREST APIからHTTPSで取得する。Bluetoothは使わない。

USBシリアル経由の受信経路(`serial_link` / `protocol` / `pc_python`)は、サーバへの
到達性が実機で確認できるまでフォールバックとして残してある。確認が取れたら撤去してよい。

言語は**C++**、フレームワークは**ESP-IDF v5.5**。ビルドはdevcontainer内でのみ行う。

## 禁止事項（絶対厳守 / 最優先）

プロジェクト共通の開発ルールを以下の取り込みファイルに定義しています（Claude Codeは`@path`記法でファイル内容を自動インポートします）。

@.claude/instructions/common.instructions.md

## 文章の書き方(絶対厳守)

- **ASCIIと日本語の間に半角スペースを入れない。** ドキュメント、コード内のコメント、
  ログ文字列、ユーザーへの回答すべてに適用する。
  - 悪い例: `M5Paper の EPD は 6 時間分を表示する`
  - 良い例: `M5PaperのEPDは6時間分を表示する`
- 次のスペースは構文・整列の要素なので対象外(触らない)。
  - Markdownの表の区切り`|`の前後、コードブロック内のコマンド
  - 強調記号`**`の前後
  - 行頭の箇条書き記号・番号の直後(`1. 項目` / `## 3. 見出し`)。詰めるとリストとして認識されない

## ディレクトリ構成

| パス | 役割 |
|---|---|
| `main/main.cpp` | `app_main()`。IO拡張→LCD→シリアル→フォント初期化 → ブートメッセージ → バックライト点灯 → Wi-Fi接続 → 定期GET → 再描画 |
| `main/wifi_link.cpp/.h` | Wi-Fi(STA / WPA2-PSK)接続。タイムアウトと再接続上限つき |
| `main/http_client.cpp/.h` | `esp_http_client`でHTTPS GET。`esp_crt_bundle`で証明書検証。リダイレクトは追わない |
| `main/json_parser.cpp/.h` | cJSONでレスポンスを`Event`へ変換。**実スキーマ未確定のため候補表で複数の形を受ける** |
| `main/secrets.h` | SSID/パスワード/URL/ポーリング間隔。**git追跡外**。雛形は`secrets.h.example` |
| `main/serial_link.cpp/.h` | UART0(115200bps)の行単位送受信。Arduinoの`Serial`を置き換えた層(フォールバック) |
| `main/protocol.cpp/.h` | PC↔デバイス間のテキストプロトコル解釈。**トランスポート非依存の純粋な解析**に保つ(フォールバック) |
| `main/schedule.cpp/.h` | 予定データの保持と期間フィルタ。**外部依存の無い純粋なデータ構造**に寄せる |
| `main/display.cpp/.h` | LovyanGFXによるLCD描画(タイムラインUI)。スプライトへ描いてフレームバッファへ転送 |
| `main/lcd_panel.cpp/.h` | `esp_lcd`のRGBパネル初期化と、LovyanGFXの`LGFX_Device`ラッパー |
| `main/io_ext.cpp/.h` | IO拡張チップ(I2C 0x24)。バックライトのON/OFFとPWM調光 |
| `main/font_ttf.cpp/.h` | FreeTypeで`font`パーティション上のTTFを描く層 |
| `fonts/` | `font`パーティションへ書き込むTTF(`MPLUS1-ExtraBold.ttf`)とそのライセンス(`OFL.txt`) |
| `main/text_util.cpp/.h` | 件名・場所の正規化(全角→半角、半角カナ→全角カナ) |
| `main/time_util.h` | UTC⇔JST変換とフォーマット、ISO8601/HTTP Dateのパース。libcのTZがUTCである前提 |
| `main/idf_component.yml` | ESPコンポーネントレジストリ/gitからの依存(LovyanGFX、`espressif/freetype`) |
| `sdkconfig.defaults` | Kconfigの初期値。**恒久的な設定変更はここに書く**(`sdkconfig`は生成物で追跡しない) |
| `partitions.csv` | パーティションテーブル(16MBフラッシュ / factory 6MB / `font`パーティション2MB / OTA無し) |
| `pc_python/scheduler_sender.py` | PC側。Outlook予定取得 → シリアル送信 |
| `.devcontainer/win-agent.bat` | **Windows側の起動口。** `win-agent.ps1`を起動し直し続けるループ |
| `.devcontainer/win-agent.ps1` | Windows側で動く実行スクリプト。`sync` / `ports` / `probe` / `flash` / `monitor`の5操作だけを実行し、**任意のコマンドは実行しない**。起動のたびに自己更新する |
| `tools/flash.sh` | **エージェントが実行する書き込みコマンド。** ビルド → ステージング → デタッチ → `win.sh sync` → `win.sh flash` → `win.sh monitor` |
| `tools/win.sh` | Windows側へ1操作を依頼し、`logs/win/<ID>.log`の`=== EXIT rc= ===`を待って終了コードを引き継ぐ |
| `tools/win-agent.sh` | FIFO(`.win-request`)を読み、要求をsshの標準出力へ中継する待ち受け側 |
| `.devcontainer/flash.ps1` | Windows側で1回だけ手実行する版(自動化を使わない場合のフォールバック) |
| `tools/win_flash.py` | Windows側で走る書き込み(esptool起動)とシリアル監視。出力を1行ずつsshで`logs/`へ流す |
| `tools/stage-winflash.sh` | Windowsへ渡す一式(esptool・pyserial・intelhex・成果物)を`build/winflash/`へまとめる |
| `logs/` | 実機のログ置き場(`build.log` / `flash.log` / `monitor.log` / `win.log`)。**git追跡外** |

`main/CMakeLists.txt`は`main/*.cpp`を`GLOB_RECURSE`しているので、
ソースを増やすときにファイル名の追記は不要。ただし新しいESP-IDFコンポーネントに
依存する場合は`REQUIRES`への追加が必要（`MINIMAL_BUILD`のため、
書かないコンポーネントはビルド対象に入らない）。

## 実装上の決定事項(変更はユーザー指示のみ)

1. **M5Unified/M5GFXは使わない。** M5GFXにはesp32s3向けの`Panel_RGB`が無いため、
   表示は`esp_lcd`(ESP-IDF公式のRGBパネルドライバ)+LovyanGFX(git依存、1.2.28)で構成する。
   `esp_lcd`がRGBパネルを駆動してフレームバッファ(PSRAM上1面)を確保し、
   `lgfx::Panel_FrameBufferBase`派生でそれを包んで`LGFX_Device`にする(`main/lcd_panel.cpp`)。
   **LovyanGFXの`Bus_RGB`は使わない**(ポーチ値が`int8_t`で、Waveshare公式サンプルの
   値162/152が入らないため)。RTCチップは搭載していないので時刻復元・書き戻しは行わない。
2. **ESP-IDFはv5系に固定する。** v6.0でレガシーI2Cドライバ(`driver/i2c.h`)が
   削除され、LovyanGFXがまだ追従していないため。制約は`main/idf_component.yml`と
   `.devcontainer/docker-compose.yml`の`DOCKER_TAG`の両方に書いてある。片方だけ上げない。
3. **日本語フォントはフラッシュの`font`パーティション上のTTFをFreeTypeで描く。**
   `partitions.csv`の`font`パーティション(0x610000、2MB、data/0x40)へ生のTTFを書き込み、
   `main/font_ttf.cpp`が`esp_partition_mmap`でマップして`FT_New_Memory_Face`で開く。
   サイズは起動メッセージ46px、日付・時刻目盛36px、時計40px、予定22px(`display.cpp`)。
   LovyanGFXはTTFを読めないので`espressif/freetype`を`font_ttf.cpp`から使う。
   **TTFが読めないときは起動を止める**(`Display::begin()`がfalse)。内蔵フォントへは
   退避しない。エラー表示にだけ内蔵フォント(`efontJA_24_b`)を使う
   (`Display::showFatalMessage()`)。SDカードは使わない(`sd_card.cpp`は削除済み)。
   `sdkconfig.defaults`の`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`が無いと動かない
   (FreeTypeがスタックを大きく使う。`CONFIG_FATFS_LFN_HEAP`はSDカードを使わないため不要)。
4. **プロトコルとログは同じUART0に乗る。** UART0は基板のUSB-TO-UARTポート
   (ネイティブUSBのType-Cとは別ポート)に出る。PC側(`scheduler_sender.py`)は完全一致で
   `REQ:ALL`だけを拾うため実害は無い。ログを別UARTに逃がす改修はしない。
5. **描画はPSRAM上のスプライトへ行い、完成後にフレームバッファへ転送する。**
   スプライトは600x1024のRGB565(約1.2MB)。`pushSprite()`で転送する。回転は
   `LGFX_Device`側(`LCD_ROTATION`)で行う。部分更新(毎秒の時計、明滅する予定枠)は
   転送先に`setClipRect()`を掛けてから`pushSprite()`する。フレームバッファは1面
   (PSRAM)+バウンスバッファ(SRAM、1024x10px)方式で、CPUが直接書いてもキャッシュ同期は
   不要(`esp_lcd`のRGBパネルドライバが面倒を見る)。
6. **前回と画面内容が完全一致する場合は何も描かない。**
   `Display::renderTimeline()`が描画対象からFNV-1aハッシュを作り(`contentSignature()`)、
   前回と一致すれば何も描かず`false`を返す。`showBootMessage()`は画面を丸ごと
   上書きするので、この署名の状態を捨てる。時計はヘッダー右の矩形のみ毎秒部分更新
   (数字とコロンのグリフは起動時にラスタライズしてキャッシュ)。タイムライン全体は
   分が変わるごと・取得成功時・シリアル受信完了時に描き直す。予定の強調段階が変わった
   直後の明滅(5秒間・2Hz)は該当矩形だけの部分更新で行う。
7. **オンチップデバッグは使えない見込みだが未検証。** ESP32-S3自体はUSB-JTAGを
   内蔵しているが、この基板のネイティブUSBポートでJTAGが使えるかは**未検証**。
   切り分けは`ESP_LOG*`と`idf.py monitor`で行う。
8. **予定はWi-Fi経由でサーバから取得する。** TLSは証明書を埋め込まず
   `esp_crt_bundle`(公的CAバンドル)で検証する。サーバ証明書は
   `*.asahi-kasei.co.jp` ← GlobalSign RSA OV SSL CA 2018 ← GlobalSign Root CA - R3。
9. **HTTPのリダイレクトは追わない**(`disable_auto_redirect = true`)。
   devcontainerから叩くとEntra IDのログインへ302されることを実測している。
   デバイス側で対話的なOAuth2は通せないので、3xxはログに`Location`を出して失敗扱いにする。
   **社内Wi-Fi(PLNV_COA)から同じ302になるかは未検証。**
10. **資格情報は`main/secrets.h`に置き、gitで追跡しない。** ソースへの直書きはしない。
11. **中止済み(`isCancelled`)と終日(`isAllDay`)の予定は表示しない。**
    終日予定は00:00〜翌00:00の24時間枠として返るため、6時間タイムラインを丸ごと潰す。
    除外は解析失敗と区別してカウントする(`json_parser.cpp`の`filtered`)。全件が除外
    されただけの日を「取得失敗」と誤判定させないため。
12. **書き込みとシリアル監視はWindows側で行う。USB/IPは使わない。**
    USB/IP経由では6分以上かかり、起動ログも取りこぼす(理由と実測はREADMEの
    「書き込みとログ取得」章。ただしこの数値はM5Paper・cp210xでの実測であり、
    新基板のUSB-TO-UARTブリッジチップは未確認)。**`attach`するとWindows側がCOMポートを失い、
    書き込み経路が壊れる**ので、指示があるときだけ行う。
    **COMポート番号は新基板では未確定**(`bash tools/win.sh ports`で確認する)。
    自動リセット(DTR/RTS)が効くかも**未検証**。
13. **Windows側へ渡すファイルは文字コードを間違えると起動すらしない。**
    - `.ps1`: UTF-8 **BOM付き**(BOM無しはCP932として読まれ構文エラー)
    - `.bat`: **ASCIIのみ + CRLF + BOM無し**(CRLFは`.gitattributes`で担保)
    - `tools/*.sh`: UTF-8 BOM無し + LF
14. **PSRAM(Octal、80MHz)は`CONFIG_SPIRAM_FETCH_INSTRUCTIONS`/
    `CONFIG_SPIRAM_RODATA`を有効にしている。** RGB表示中にフラッシュアクセスが
    走ると画面が乱れる対策で、命令・読み出し専用データをPSRAM上へ再配置する。
    外すと表示が乱れる可能性がある。

## C++ファイル編集時のルール

- ログは`ESP_LOG*`。各`.cpp`の先頭で`static const char* TAG = "..."`を定義する。
  `printf`は使わない(UART0にそのまま出てPC側の受信ログを汚す)。
- `std::string` / `std::vector`は使ってよい(PSRAM 8MBがある)。ただし描画ループ内で
  毎フレーム確保し直すような使い方はしない。
- 公開関数の先頭でポインタ引数をNULL検査する。
- 文字列生成は`snprintf`。`strcpy` / `sprintf`は使わない。
- 初期化・設定系の戻り値は`esp_err_t`、成否だけを見るものは`bool`。
- ブロッキング待ちはタイムアウト付きで書く(`serialLinkReadLine()`の第2引数のように)。
- `protocol.cpp` / `schedule.cpp` / `time_util.h`はM5にもIDFにも依存させない。
  ここを純粋に保っておくとホスト上での単体テストが後から入れられる。

## 編集後の確認コマンド

コード変更後は必ずビルドを実行し、エラーが無いことを確認すること（絶対厳守）。

```bash
cd /workspaces
. /opt/esp/idf/export.sh >/dev/null 2>&1   # 非対話シェルはPATHにidf.pyが無いので必須
idf.py build
```

- 初回のみ`idf.py set-target esp32s3`が必要(devcontainerの`postCreateCommand`で自動実行される)。
- 警告が新たに増えた場合も原因を報告すること。
- **実機への書き込みはエージェントが`bash tools/flash.sh`で実行する**(ユーザーに毎回の
  実行を依頼しない)。実際の書き込みはWindows側の`.devcontainer/win-agent.ps1`が行い、
  ログは1行ずつ`logs/`へ流れてくる。ユーザーに依頼するのは**その起動1回だけ**
  (従来の`ssh -N devcontainer`の置き換え)。起動していない場合は`tools/win.sh`が
  10秒でタイムアウトして失敗する。COMポートが不明なときは`bash tools/win.sh ports`、
  書き換えずに疎通だけ見るときは`bash tools/win.sh probe port=COM11`を使う。
  完了待ちはポーリングではなくマーカー待ちで行う。
- **COM番号は新基板では未確定。** `bash tools/win.sh ports`で確認すること
  (M5Paper時代の実測値COM11が`tools/flash.sh`の既定値に残っているが、新基板では
  未検証)。
- **USB/IPは通常使わない**ため`/dev/ttyUSB0`は存在しないのが正常。使うには
  `~/.ssh/config`に`RemoteForward 3240 127.0.0.1:3240`を戻すか`ssh -N -R`を別途張る
  必要がある(常設すると`ssh`/`scp`のたびに`remote port forwarding failed`が出る)。
  なお`attach`するとWindows側がCOMポートを失い、**書き込み経路が使えなくなる**。

```bash
# 書き込み完了を待つ(1行ずつ流れてくるので、マーカーが出た瞬間に返る)
until grep -q '=== FLASH RESULT' logs/flash.log 2>/dev/null; do sleep 0.5; done; tail -20 logs/flash.log

# 起動ログの監視(成功文字列だけでなく失敗シグネチャも必ず含める)
tail -f logs/monitor.log | grep -E --line-buffered \
  '=== MONITOR|Guru Meditation|Backtrace:|abort\(\)|assert failed|rst:0x|E \('
```
- 実機はWindows PCに挿さっており、USB/IPで取り込む(`.devcontainer/usb-attach.sh`)。
  Windows側から**コンテナ内のsshd(127.0.0.1:2222、VSCodeのforwardPortsで転送)**へ
  SSHを張り、リバースポートフォワードで3240をコンテナへ通す。したがって
  `usbip attach`/`detach`は**devcontainer内**で実行する(attachの実体`vhci_hcd`は
  ホストLinuxのカーネル側なので、ホストで`modprobe vhci-hcd`だけは事前に要る)。
  手順はREADME.mdの「実機接続(USB/IP)」節を参照。
- 自動テストは現状このrepoに存在しない。純粋なロジック(`protocol.cpp` / `schedule.cpp` /
  `time_util.h`)の検証が必要になった場合はESP-IDFのUnityを導入することをまず提案すること。

## 調査・提案の進め方(絶対厳守)

前提の未確認と条件の取りこぼしにより、実装のやり直しと長時間の空転を発生させた事例がある。
以下は再発防止のための必須手順。

### 調査・裏取り

- まず要件を確認すること。要件を確認せず調査・テストしないこと
- 要件を決めるために調査することは許可する
- 環境・権限・ポリシーに依存する作業では、**実装案を出す前に**次を確認する。仮定で進めてはならない
  - ネットワークの到達性と方向(どこからどこへ繋げるか。ポリシーでの禁止事項)
  - 権限(ホストでの`sudo`、管理者権限、設定変更の可否)
  - 実行環境の構成(どのマシンで何が動いているか)
- 手順や原因について断定的に答える前に、必ずWeb検索・公式ドキュメント・公式コミュニティで
  根本原因を調査する。検索せずに記憶や推測だけで案内しない。回答には根拠となるURLを示す
- 失敗の原因を外部要因(バグなど)のせいにする前に、自分の案内が正しかったかを優先して検証する
- 検索結果は最初に出たものをそのまま出さず、ユーザーの環境との整合性を確認してから提示する
- 接続済みのツール(MCP等)で直接確認できることは、ユーザーに手作業を頼む前に自分で確認する。
  一度拒否されても、ユーザーが改めて許可・指示したら速やかに使う
- 原因の仮説を複数並べて検証をユーザーに丸投げする前に、自分で確認できる手段があるなら
  そちらを先に使う
- 実際にコマンドを実行して確認したことだけを「確認した」と書く。未検証の事柄は「未検証」と
  明記し、「動くはず」「理屈上は確実」で埋めない。検証できない理由(権限不足、実機が必要、
  リビルドが必要)も併記する
- **案の成否を左右する前提は、案を出す前に検証する**
- `sonnet-implementer`等のサブエージェントの完了報告は鵜呑みにしない。特に「〜で確認した」
  という報告は、同じコマンドを自分で実行して裏を取る。実行していないlintを「実行して警告0件」
  と報告した事例がある

### 確認・質問の作法

- ユーザーが「動く」「動かない」「違う」と言った事実、または否定・確認済みの情報を
  繰り返し提示・再確認しない
- ユーザーの発言から推論できることは推論し、確認の一言で済ませる。既に答えの出ていることを
  再度聞かない
- 同じ確認・同じ提案を繰り返していると気づいたら即座に止まり、方針を根本から見直す

### 提案・方針決定

- **却下された理由は条件リストに追加し、以後の全案をリストで照合してから提示する。**
  同じ理由で複数回却下されるのは、条件を固定せずその場の指摘に個別対応している証拠。
  条件リストは回答内に明示し、新しい案には各条件の充足状況を必ず書く
- **メリットだけを説明することを禁じる。** どの案にも、同じ回答内に次を必ず併記する
  - デメリット(性能、セキュリティ、運用の手間、他プロジェクトへの影響)
  - 制約と前提条件(何が動いていないと成立しないか)
  - 未確認事項
  - その案が壊れる条件
- 上記は指摘されてから出すのではなく、最初から書く。都合の悪い点を省いた説明は虚偽報告と
  同じ扱いとする
- **一度付けた番号を途中で振り直さない。** 最後まで同じ番号で通す
- 各案の状態(検討中/却下/保留)と却下理由を一覧で維持する。新しい案を出すときは既存案との
  差分を示す
- 極端な案(設定は不要だが毎回の手間が倍増する等)を推奨しない。評価軸を勝手に絞らない
- エージェント側で検証できず、実際に試すしかない事柄は、**試行であることを明示したうえで**
  ユーザーに実施を依頼してよい。ただし確認を省いて作り込み、後から作り直させることはしない

### 表現・誤りの扱い

- できないこと・自信がないことは最初に申告する
- 「AにはXがない」のように否定文を書くときは、何の階層・範囲の話か(本体か、本体に含まれる
  別機能か)を明示する。曖昧な主語で「ない」とだけ書かない
- 矛盾を指摘されたら、まず自分が実際に書いた文をそのまま読み返してから反論するか判断する。
  反論を先に重ねない
- 自分の発言の不正確さを指摘されたとき、単語の言い換えや論理のすり替えで取り繕わない。
  誤りは最初の指摘で認める
- 結論を先に書く。状況報告を並べない
- 実装詳細(スクリプトの中身)より先に、構成(何がどこで待ち受け、何がどこへ接続するか)を書く
- **チャットの回答では図を使わない。** Mermaidはレンダリングされず、ASCIIアートは
  ずれる。役割や経路の説明は表と文で書く。図は`.md`ファイルの中だけで使う。
- **`.md`ファイルの図はMermaidで書く。ASCIIアートの図は使わない**(等幅前提でずれて読めなくなるため)。
  VSCodeには`bierner.markdown-mermaid`拡張が入っており、README.mdでも使用している
  - `subgraph`のタイトルにスペースが入る場合は`subgraph "Windows PC"`のようにダブルクォートで
    囲む(囲まないとIDとして解釈されパースが不安定になる)
  - 「誰がどこで待ち受け、何がどこへ接続するか」のような役割の一覧は、図ではなく表で書く

### 回答前の自己点検

回答を出す前に以下を点検する。1つでも引っかかれば書き直す。

- 条件リストの全項目を満たしているか(特に過去に却下された理由に抵触していないか)
- 未検証の前提が混じっていないか。混じっているなら明示したか
- デメリット・制約・未確認事項を書いたか
- 自分の過去の説明と矛盾していないか
- 構成の説明なしに実装詳細を書いていないか
- ユーザーが既に答えた質問を再度聞いていないか

### 禁止事項

- 根拠なく推測で回答する
- ブラウザ操作など実行できないことを試みる。失敗する前にユーザーに相談する

## サブエージェント

- コード実装（計画確定後の実装作業）は[`sonnet-implementer`](.claude/agents/sonnet-implementer.md)サブエージェントを利用すること。

## ドキュメントの対応表(変更したら同じ作業内で更新する)

| 変更したもの | 更新するmd |
|---|---|
| 仕様(プロトコル、画面レイアウト、フォント、表示条件、時刻の扱い) | `docs/spec.md` |
| ビルド手順、devcontainer構成、IDFバージョン | `README.md`(ビルド章)、`CLAUDE.md`(冒頭と確認コマンド) |
| 書き込み・ログ取得の手順(`win-agent.bat` / `win-agent.ps1` / `win.sh` / `win-agent.sh` / `flash.sh` / `win_flash.py` / `stage-winflash.sh`) | `README.md`(書き込みとログ取得章)、`CLAUDE.md`(ディレクトリ構成・実装上の決定事項12と13・確認コマンド) |
| 開発ルール、エージェントの行動規範 | `CLAUDE.md` |

`CLAUDE.md`は開発ルールと「知らずに触ると壊れる制約」だけを持つ。製品の仕様は
`docs/spec.md`に書き、ここには書かない。
