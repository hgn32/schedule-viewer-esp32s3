# 実装メモ(構成と制約)

このリポジトリの構成と、**知らずに触ると壊れる実装上の制約**をまとめる。
`.cpp` / `.h` / `tools/` / `sdkconfig.defaults` / `partitions.csv`を編集する前に必ず読む。

製品の仕様(プロトコル、画面レイアウト、表示条件、時刻の扱い)は`docs/spec.md`。
基板のハードウェア制御の詳細と実測値は`docs/waveshare-esp32s3-7b-display.md`。

## プロジェクトの前提

Waveshare ESP32-S3-Touch-LCD-7B(ESP32-S3-WROOM-1-N16R8、7インチ1024x600 IPS、
RGB565パラレル16bit。基板を90度回して縦置き・論理600x1024で使う)に、
Outlook予定表を12時間分のタイムラインとして表示するビューア。予定はWi-Fi(STA)経由で
予定配信サーバのREST APIからHTTPSで取得する。Bluetoothは使わない。

USBシリアル経由の受信経路(`serial_link` / `protocol` / `pc_python`)は、サーバへの
到達性が実機で確認できるまでフォールバックとして残してある。確認が取れたら撤去してよい。

同様に、`main/secrets.h`の`USE_DUMMY_SCHEDULE`を1にすると、HTTP取得を行わず
`main/dummy_schedule.cpp`が組み立てるダミー予定を表示する(サーバへ到達できない環境向けの
一時的な仕組み)。`secrets.h`はgit追跡外なので、この切り替え自体は差分に現れない。
確認が取れたら`main/dummy_schedule.cpp/.h`と`main/sntp_time.cpp/.h`ごと撤去してよい。

言語は**C++**、フレームワークは**ESP-IDF v5.5**。ビルドはdevcontainer内でのみ行う。

## ディレクトリ構成

| パス | 役割 |
|---|---|
| `main/main.cpp` | `app_main()`。IO拡張→LCD→シリアル→フォント初期化 → ブートメッセージ → バックライト点灯 → Wi-Fi接続 → 定期GET → 再描画 |
| `main/wifi_link.cpp/.h` | Wi-Fi(STA / WPA2-PSK)接続。複数候補から、スキャン結果を見て選んで接続する。タイムアウトと再接続上限つき |
| `main/http_client.cpp/.h` | `esp_http_client`でHTTPS GET。`esp_crt_bundle`で証明書検証。リダイレクトは追わない |
| `main/json_parser.cpp/.h` | cJSONでレスポンスを`Event`へ変換。**実スキーマ未確定のため候補表で複数の形を受ける** |
| `main/dummy_schedule.cpp/.h` | **一時的。** `USE_DUMMY_SCHEDULE=1`のときにHTTP取得の代わりに使うダミー予定のJSON生成。到達性が確認できたら撤去する |
| `main/sntp_time.cpp/.h` | **一時的。** ダミーモードのときだけ使うSNTP時刻同期。到達性が確認できたら撤去する |
| `main/secrets.h` | SSID/パスワード/URL/ポーリング間隔/`USE_DUMMY_SCHEDULE`。**git追跡外**。雛形は`secrets.h.example` |
| `main/serial_link.cpp/.h` | UART0(115200bps)の行単位送受信。Arduinoの`Serial`を置き換えた層(フォールバック) |
| `main/protocol.cpp/.h` | PC↔デバイス間のテキストプロトコル解釈。**トランスポート非依存の純粋な解析**に保つ(フォールバック) |
| `main/schedule.cpp/.h` | 予定データの保持と期間フィルタ。**外部依存の無い純粋なデータ構造**に寄せる |
| `main/display.cpp/.h` | LovyanGFXによるLCD描画(タイムラインUI)。スプライトへ描いてフレームバッファへ転送 |
| `main/lcd_panel.cpp/.h` | `esp_lcd`のRGBパネル初期化と、LovyanGFXの`LGFX_Device`ラッパー |
| `main/io_ext.cpp/.h` | IO拡張チップ(CH32V003、I2C 0x24)。バックライト・各リセット・USB/CAN切替の出力制御。**扱いに癖があるので決定事項15を読むこと** |
| `main/touch.cpp/.h` | タッチIC(GT911)からの読み出しと、タップ/ロングタップ判定。座標は使わず押下の有無だけ見る。**決定事項16を読むこと** |
| `main/font_ttf.cpp/.h` | FreeTypeで`font`パーティション上のTTFを描く層 |
| `fonts/` | `font`パーティションへ書き込むTTF(`MPLUS1-Medium.ttf`)とそのライセンス(`OFL.txt`) |
| `main/text_util.cpp/.h` | 件名・場所の正規化(全角→半角、半角カナ→全角カナ) |
| `main/time_util.h` | UTC⇔JST変換とフォーマット、ISO8601/HTTP Dateのパース。libcのTZがUTCである前提 |
| `main/idf_component.yml` | ESPコンポーネントレジストリ/gitからの依存(LovyanGFX、`espressif/freetype`) |
| `sdkconfig.defaults` | Kconfigの初期値。**恒久的な設定変更はここに書く**(`sdkconfig`は生成物で追跡しない) |
| `partitions.csv` | パーティションテーブル(16MBフラッシュ / factory 6MB / `font`パーティション2MB / OTA無し) |
| `pc_python/scheduler_sender.py` | PC側。Outlook予定取得 → シリアル送信 |
| `.devcontainer/win-agent.bat` | **Windows側の起動口。** `win-agent.ps1`を起動し直し続けるループ |
| `.devcontainer/win-agent.ps1` | Windows側で動く実行スクリプト。`sync` / `ports` / `probe` / `flash` / `monitor`の5操作だけを実行し、**任意のコマンドは実行しない**。起動のたびに自己更新する |
| `tools/flash.sh` | **エージェントが実行する書き込みコマンド。** ビルド → ステージング → デタッチ → `win.sh sync` → `win.sh flash` → `win.sh reset` → `win.sh monitor` |
| `tools/win.sh` | Windows側へ1操作(`sync`/`ports`/`probe`/`reset`/`flash`/`monitor`/`restart`)を依頼し、`logs/win/<ID>.log`の`=== EXIT rc= ===`を待って終了コードを引き継ぐ |
| `tools/win-agent.sh` | FIFO(`.win-request`)を読み、要求をsshの標準出力へ中継する待ち受け側 |
| `.devcontainer/flash.ps1` | Windows側で1回だけ手実行する版(自動化を使わない場合のフォールバック) |
| `tools/win_flash.py` | Windows側で走る書き込み(esptool起動)とシリアル監視。出力を1行ずつsshで`logs/`へ流す |
| `tools/stage-winflash.sh` | Windowsへ渡す一式(esptool・pyserial・intelhex・成果物)を`build/winflash/`へまとめる |
| `tools/screenshot.py` | **一時的。** `logs/monitor.log`の`SHOT`行(`Display::dumpScreenshot()`が出す)からPNGを復元するホスト側スクリプト。ダミーモードのときだけ使う開発用。到達性が確認できたら`Display::dumpScreenshot()`ごと撤去する |
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
   LovyanGFXはTTFを読めないので`espressif/freetype`を`font_ttf.cpp`から使う。
   **TTFが読めないときは起動を止める**(`Display::begin()`がfalse)。内蔵フォントへは
   退避しない。エラー表示にだけ内蔵フォント(`efontJA_24_b`)を使う
   (`Display::showFatalMessage()`)。SDカードは使わない(`sd_card.cpp`は削除済み)。
   `sdkconfig.defaults`の`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`が無いと動かない
   (FreeTypeがスタックを大きく使う。`CONFIG_FATFS_LFN_HEAP`はSDカードを使わないため不要)。
   グリフのラスタライズ結果はPSRAM上に一括確保したアリーナへキャッシュする。
   **このアリーナを小さな`malloc`の集合に置き換えないこと**(ヒープが荒れる)。
   仕組みは`docs/spec.md`。
4. **ログはUSB-Serial/JTAG(ネイティブUSB)へ出す。UART0はプロトコル専用。**
   `sdkconfig.defaults`の`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`。
   **副コンソール(`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`)へ戻さないこと。**
   USB-Serial/JTAGはホストが接続してからしか初期化されず、書き込み後に監視を開いても
   無音になり、実機のログが一切取れなくなる。
5. **描画はPSRAM上のスプライトへ行い、完成後にフレームバッファへ転送する。**
   **スプライトはパネルと同じ生の向き(1024x600)で確保し、回転はスプライト側に持たせる。
   `LGFX_Device`は回転なし(0)のまま使う。** 向きが食い違うと`pushSprite()`が
   90度回転を伴う転送になり極端に遅くなる。描画コードは論理座標(600x1024、
   `SCR_W`/`SCR_H`)のままでよい。部分更新は`Display::pushRect()`が論理座標を
   生座標へ直してから`setClipRect()`を掛ける。呼び出し側は常に論理座標を渡す。
   色深度は**`rgb565_nonswapped`**(`rgb565_2Byte`はバイトスワップ済みで色が壊れる)。
   **`bounce_buffer_size_px`を0にしないこと**(画面が真っ黒になる)。
   CPUが直接書いてもキャッシュ同期は不要。
   根拠・変換式・実測値は`docs/waveshare-esp32s3-7b-display.md`。
6. **前回と画面内容が完全一致する場合は何も描かない。**
   `Display::renderTimeline()`が描画対象からFNV-1aハッシュを作り(`contentSignature()`)、
   前回と一致すれば何も描かず`false`を返す。`showBootMessage()`は画面を丸ごと
   上書きするので、この署名の状態を捨てる。
   **更新の優先順位は時計→タイムライン再描画→明滅**(`main.cpp`のメインループ)。
   **重い全画面再描画を毎分00秒に走らせないこと**(時計が止まって見える)。
   各更新のタイミングは`docs/spec.md`。
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
11. **中止済み・終日・空き(`showAs=free`)の予定は表示しない。**
    終日予定は24時間枠として返るため、タイムラインを丸ごと潰す。
    **除外は解析失敗と区別してカウントする**(`json_parser.cpp`の`filtered`)。
    全件が除外されただけの日を「取得失敗」と誤判定させないため。
    表示条件の詳細は`docs/spec.md`。
12. **書き込みとシリアル監視はWindows側で行う。USB/IPは使わない。**
    **`attach`するとWindows側がCOMポートを失い、書き込み経路が壊れる**ので、
    指示があるときだけ行う。書き込み・監視に使うのは**ネイティブUSBポート**
    (USB-Serial/JTAG、`VID:PID=303A:1001`)。**CH343側(`USB TO UART`ポート)からは
    ROMブートローダに到達できない。** 手順はREADME.mdの「書き込みとログ取得」章。
13. **Windows側へ渡すファイルは文字コードを間違えると起動すらしない。**
    - `.ps1`: UTF-8 **BOM付き**(BOM無しはCP932として読まれ構文エラー)
    - `.bat`: **ASCIIのみ + CRLF + BOM無し**(CRLFは`.gitattributes`で担保)
    - `tools/*.sh`: UTF-8 BOM無し + LF
14. **PSRAM(Octal、80MHz)は`CONFIG_SPIRAM_FETCH_INSTRUCTIONS`/
    `CONFIG_SPIRAM_RODATA`を有効にしている。** RGB表示中にフラッシュアクセスが
    走ると画面が乱れる対策で、命令・読み出し専用データをPSRAM上へ再配置する。
    外すと表示が乱れる可能性がある。
15. **IO拡張チップはCH32V003(MCU)。CH422Gではない。** 7(末尾Bなし)はCH422Gだが、
    7Bでは別チップに置き換わっている。単一アドレス`0x24`へ`{レジスタ番号, 値}`の
    2バイトを書く形式で、レジスタは`0x02`=モード(0xFFで全ピン出力)、`0x03`=IO出力、
    `0x04`=IO入力、`0x05`=バックライトPWM、`0x06`=ADC。
    ビット割り当てはIO1=TP_RST、IO2=バックライト(DISP)、IO3=LCD_RST、IO4=SD_CS、
    IO5=USB(0)/CAN(1)で、出力の基準値は`0x1E`。
    **`bit5`(USB_SEL)は必ずLowに保つ**(HighにするとネイティブUSBがCAN側へ切り替わり、
    書き込みもログ取得もできなくなる)。**ACK検査は切らない**(CH32V003はACKを返す)。
    **`0x38`に応答するデバイスは存在しない。** CH422Gのコマンド体系(アドレスごとに
    1バイト)で書いていた時期があり、バックライトの消灯が一切効かなかった。
    書き込みが効いているかは`0x04`の読み戻しで確認できる(書いた値がそのまま返る)。
    **PWM調光ができる**(`0x05`へ0〜255。上限は公式デモとESPHomeに合わせて247=97%)。

16. **タッチはGT911を`espressif/esp_lcd_touch_gt911`で読む。LovyanGFXのタッチ層
    (`Touch_GT911`)は使わない。** GT911はIO拡張チップ(CH32V003)と同じI2Cバス
    (SDA=GPIO8 / SCL=GPIO9、`io_ext.cpp`が`i2c_new_master_bus()`で確保済み)に
    ぶら下がっており、`ioExtGetBus()`でバスハンドルを共有する
    (`esp_lcd_new_panel_io_i2c()`は`i2c_master_bus_handle_t`を渡すとv2実装が選ばれる)。
    **LovyanGFXのタッチ層はレガシーI2Cドライバ(`driver/i2c.h`)を使うため、
    `io_ext.cpp`が使う新I2Cドライバ(`driver/i2c_master.h`)と同じピンを二重に
    初期化することになり衝突する。** これを避けるため`main/touch.cpp`は
    `esp_lcd_touch_gt911`コンポーネントを直接使う。GT911のI2Cアドレスは
    **0x5D**(実機のI2Cスキャンで確認済み。電源投入時のINTレベルで0x5D/0x14が
    決まる仕様だが、この基板では0x5D固定で読めている)。TP_RSTはIO拡張のIO1で、
    起動時の出力値`0x1E`で既にHigh(リセット解除済み)のため、
    `esp_lcd_touch_config_t.rst_gpio_num`は`GPIO_NUM_NC`にする。INTはGPIO4だが
    ポーリングで読むため`int_gpio_num`も`GPIO_NUM_NC`。
    **`espressif__esp_lcd_touch_gt911`(1.2.1)が提供する
    `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`マクロは使わない。** このマクロは
    指示付き初期化子の並び順が古い`esp_lcd_panel_io_i2c_config_t`の宣言順を
    前提にしており、ESP-IDF v5.5の実際の宣言順(`dev_addr`の次が
    `on_color_trans_done`/`user_ctx`)と食い違うため、C++では指示付き初期化子の
    順序違反としてビルドエラーになる。`main/touch.cpp`ではメンバへ個別代入している。
    座標(`esp_lcd_touch_get_data()`)は取得できるが使わない。タップ/ロングタップの
    判定仕様(1500ms、誤動作対策)は`docs/spec.md`。
