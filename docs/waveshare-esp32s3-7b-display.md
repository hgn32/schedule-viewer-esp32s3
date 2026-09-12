# Waveshare ESP32-S3-Touch-LCD-7B 画面制御メモ

別プロジェクトへ引き継ぐための知見。**すべて2026-09-12に実機で確認した内容**で、
推測は「未検証」と明記してある。対象は`ESP32-S3-Touch-LCD-7B`
(ESP32-S3-WROOM-1-N16R8 / 16MBフラッシュ / 8MB Octal PSRAM / 1024x600 RGB LCD)。

環境はESP-IDF v5.5 + `esp_lcd`(RGBパネル) + LovyanGFX 1.2.28(git依存)。

> 参照した公式デモは**7B用ではなく7(無印)用**。
> `https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-7/ESP32-S3-Touch-LCD-7-Demo.zip`
> の`ESP-IDF/08_lvgl_Porting/main/waveshare_rgb_lcd_port.c`。
> 7B用のzip(`.../ESP32-S3-Touch-LCD-7B/ESP32-S3-Touch-LCD-7B-Demo.zip`)は404だった。
> 画面まわりは7と7Bで同じ値で動いている。

---

## 1. 最初に踏む地雷

この4つは知らないと必ずハマる。

| 症状 | 原因 | 対処 |
|---|---|---|
| 画面が紫がかる/ネガポジに見える | LovyanGFXの`rgb565_2Byte`は**バイトスワップ済み**形式。`esp_lcd`のRGBパネルはネイティブ順を期待する | 色深度を**`rgb565_nonswapped`**にする |
| 全画面転送が異常に遅い(実測832ms) | スプライトとフレームバッファの向きが違うと`pushSprite()`が回転転送になり、61万画素がPSRAMのキャッシュラインをまたぐ | **スプライトをパネルと同じ生の向きで確保**し、回転はスプライト側に持たせる |
| 書き込み中にCOMポートが消える | CH32V003の`USB_SEL`(bit5)がHighだとネイティブUSBがCAN側へ切り替わる | `USB_SEL`を**常にLow**に保つ |
| I2Cが常に`unexpected nack` | **CH32V003はACKを返さない** | `i2c_device_config_t.flags.disable_ack_check = 1` |

---

## 2. esp_lcdのRGBパネル設定

```c
esp_lcd_rgb_panel_config_t cfg = {};
cfg.clk_src              = LCD_CLK_SRC_DEFAULT;
cfg.data_width           = 16;
cfg.bits_per_pixel       = 16;
cfg.num_fbs              = 1;
cfg.bounce_buffer_size_px = 1024 * 10;   // 0にしてはいけない。後述

cfg.timings.pclk_hz           = 30 * 1000 * 1000;
cfg.timings.h_res             = 1024;
cfg.timings.v_res             = 600;
cfg.timings.hsync_pulse_width = 162;
cfg.timings.hsync_back_porch  = 152;
cfg.timings.hsync_front_porch = 48;
cfg.timings.vsync_pulse_width = 45;
cfg.timings.vsync_back_porch  = 13;
cfg.timings.vsync_front_porch = 3;
cfg.timings.flags.pclk_active_neg = 1;

cfg.hsync_gpio_num = 46;
cfg.vsync_gpio_num = 3;
cfg.de_gpio_num    = 5;
cfg.pclk_gpio_num  = 7;
cfg.disp_gpio_num  = -1;   // DISPはCH32V003経由なのでGPIOでは制御しない

// data[0..4]=B3..B7, [5..10]=G2..G7, [11..15]=R3..R7
const int data_pins[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
cfg.flags.fb_in_psram = 1;
```

### バウンスバッファは外せない

`bounce_buffer_size_px = 0`にすると**画面が真っ黒になる**(実機で確認)。
ESP-IDF公式ドキュメントの
"a high pixel clock might cause LCD peripheral starvation, leading to display corruption"
に該当する。

代償は大きい。バウンスバッファ方式は
「**割り込みの中でCPUがPSRAMのフレームバッファからSRAMへコピーする**」実装で、
公式も"a significant increase in CPU usage"と書いている。pclk 30MHzなら
**毎秒60MB分のコピーが常時CPUに乗る**。描画性能の上限はこれで決まる。

### sdkconfigの必須項目

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y   # RGB表示中のフラッシュアクセスで画面が乱れる対策
CONFIG_SPIRAM_RODATA=y               # 同上
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

FreeTypeを使うなら`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`も要る(スタックを大きく使う)。

---

## 3. LovyanGFXとの接続

`M5GFX`にはesp32s3向けの`Panel_RGB`が無い。LovyanGFXの`Bus_RGB`も使えない
(ポーチ値が`int8_t`で、公式の162/152が入らない)。
そこで**`esp_lcd`がフレームバッファを持ち、`lgfx::Panel_FrameBufferBase`派生で包む**。

### 色深度

```cpp
class PanelLcd7b : public lgfx::Panel_FrameBufferBase {
public:
    PanelLcd7b() {
        // rgb565_2Byteはバイトスワップ済み(SPIパネル向け)。RGBパネルのフレーム
        // バッファは16bitのネイティブ順で読まれるので、こちらを使う。
        _write_depth = lgfx::color_depth_t::rgb565_nonswapped;
        _read_depth  = lgfx::color_depth_t::rgb565_nonswapped;
    }
};
```

`lgfx::rgb565_t`が`rgb565_nonswapped`に対応する型
(`lgfx/v1/misc/colortype.hpp`の`static constexpr color_depth_t depth = rgb565_nonswapped;`)。
`pushImage()`へ渡すときはこの型でキャストする。

### 回転はスプライト側に持たせる(重要)

基板を90度回して縦置きで使う場合、**`LGFX_Device`に`setRotation()`をかけてはいけない**。

```cpp
gfx.setRotation(0);                           // デバイスは回転なし(生の1024x600)

_canvas.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
_canvas.createSprite(1024, 600);              // パネルと同じ生の向きで確保
_canvas.setRotation(1);                       // 回転はスプライトが持つ(論理600x1024)
```

理由は`LGFX_Sprite::push_sprite()`が**生バッファの寸法をそのまま**
`dst->pushImage()`へ渡すため(`lgfx/v1/LGFX_Sprite.hpp`)。
向きを揃えると`Panel_FrameBufferBase::writeImage()`の
「回転0 かつ 変換不要」の分岐に入り、**行ごとの`memcpy`**になる。

実測: 全画面転送 **832ms → 160ms**。

### 部分更新のクリップ矩形は生座標へ直す

デバイスが回転なしなので、`setClipRect()`には生座標を渡す必要がある。
変換式は`Panel_FrameBufferBase::_rotate_pixelcopy()`の回転1の扱いに合わせる。

```cpp
// 論理600x1024 → 生1024x600 (回転1)
const int nx = LOGICAL_H - (y + h);   // LOGICAL_H = 1024
const int ny = x;
const int nw = h;
const int nh = w;
_gfx->setClipRect(nx, ny, nw, nh);
_canvas.pushSprite(_gfx, 0, 0);
```

論理(0,0,600,1024)→生(0,0,1024,600)で検算できる。
スプライトの生バッファから1画素読むときは
`index = x * 1024 + (1023 - y)`(論理(x,y)に対応)。

---

## 4. CH32V003(IOエキスパンダ / I2C)

**最大の罠。「アドレス+レジスタ番号」形式のチップではない。**
用途ごとにI2Cアドレスが違い、**各アドレスへ1バイトだけ書く**。

| 用途 | I2Cアドレス(7bit) | 内容 |
|---|---|---|
| モード | `0x24` | `0x02` | `0xFF`でIO0〜IO7を出力にする |
| IO出力 | `0x24` | `0x03` | 出力値8bit |
| IO入力 | `0x24` | `0x04` | 読み出し(書いた値がそのまま読み戻せる) |
| PWM | `0x24` | `0x05` | **バックライト輝度(0〜255)** |
| ADC | `0x24` | `0x06` | 電池電圧 |

SDA=GPIO8 / SCL=GPIO9 / 400kHz。

### 出力ビットの割り当て

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 用途 | DI0 | TP_RST | **LCD_BL(DISP)** | LCD_RST | SD_CS | **USB_SEL** | OD0 | OD1 |

### 公式デモが実際に書いている値

```c
// 初期化(全ピン出力 → 出力値)
write2(0x24, 0x02, 0xFF);
write2(0x24, 0x03, 0x1E);
// バックライト輝度(0=消灯、247=97%が上限)
write2(0x24, 0x05, pwm);
// バックライト消灯(PWMを0にしたうえでDISP=IO2も落とす)
write2(0x24, 0x05, 0x00);
write2(0x24, 0x03, 0x1A);
```

**出力値は`0x1E`を基準にし、そこから動かすのは用途のビットだけにする。**
とくに`bit5`(USB_SEL)を立てるとネイティブUSBがCAN側へ切り替わり、
**書き込み経路もログ取得も失われる**。

### 守るべき3点

1. **モード(`0x02`)は初期化時に一度だけ書けばよい。** 公式デモの`IO_EXTENSION_Init()`も
   `{0x02, 0xFF}`を一度書くだけで、以降は`{0x03, 値}`を書いている。
2. **`USB_SEL`(bit5)は常にLow。** Highにするとネイティブ USBがCAN側へ切り替わり、
   **書き込み経路もログ取得も失われる**。CANを使わないなら上げさせないガードを入れる。
3. **ACK検査は切らない。** CH32V003は`0x24`でACKを返す(I2Cスキャンで確認済み)。
   切ると書き込み失敗が一切ログに出なくなる。

```c
i2c_device_config_t dev_cfg = {};
dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
dev_cfg.device_address  = 0x24;        // 7Bに存在するのはこのアドレスだけ
dev_cfg.scl_speed_hz    = 400000;
```

### PWM調光はできる

レジスタ`0x05`へ0〜255を書くと輝度が変わる。上限はWaveshare公式デモと
ESPHomeの`waveshare_io_ch32v003`がどちらも247(=97%)にしている。

---

## 5. 書き込みとログ

| 項目 | 内容 |
|---|---|
| 書き込みポート | **ネイティブUSBのType-C**。`VID:PID=303A:1001`(USB-Serial/JTAG) |
| 自動リセット | **効く**(esptoolが`Hard resetting via RTS pin`で完了) |
| `USB TO UART`側 | CH343。`VID:PID=1A86:55D3`。**ROMブートローダには到達できなかった**(原因未特定) |
| ダウンロードモード | BOOTを押したままType-Cを挿し直し、通電後に離す |

### `--after hard_reset`だけではアプリが起動しないことがある

書き込み後に明示的なリセットを1回入れると確実。

```
python -m esptool --chip esp32s3 --port COMx run
```

### ログはUSB-Serial/JTAGを主コンソールにする

`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`(副コンソール)では**ログが出ない**。
USB-Serial/JTAGは仕様上**ホストが接続してからしか初期化されない**ため、
リセット→ポートを閉じる→監視を開く、という順序だと初期化されないまま無音になる。

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y   # 主コンソールにする
```

こうすればCOM4でそのまま読める。UART0は別用途に空く。

---

## 6. 性能の実測値と、効いた最適化

600x1024相当の全画面を描き直したときの実測(ESP32-S3 240MHz / Octal PSRAM 80MHz)。

| 段階 | 全体 | 転送 | 備考 |
|---|---|---|---|
| 最初 | 1173ms | 832ms | スプライトとFBの向きが不一致 |
| 向きを揃えた | 585ms | 160ms | 転送が行ごとの`memcpy`になった |
| 文字を一括転送 | — | — | 1画素ずつ`drawPixel()`をやめ、グリフ単位で`pushImage()` |
| グリフキャッシュ | **394ms** | 157ms | FreeTypeの再ラスタライズ **203ms → 0ms** |

### 内訳(最終状態)

```
背景の塗り 105ms / ヘッダ 19ms / 時刻目盛 23ms / 予定枠 86ms / 転送 157ms
```

**背景105ms + 転送157ms = 262msがメモリ帯域で決まる下限。**
1.2MBの塗りと1.2MBのコピーで、バウンスバッファのCPUコピーと帯域を奪い合うため
これ以上は縮まない。さらに削るなら`num_fbs = 2`のダブルバッファで転送を消すしかない
(PSRAMを1.2MB追加、部分更新を2面へ描き分ける必要あり。未検証)。

### 文字描画で効いた2つ

1. **1画素ずつ`drawPixel()`を呼ばない。** LovyanGFXの`drawPixel()`は
   1回ごとにクリップ判定・回転変換・書き込み窓の設定を通るので固定費が大きい。
   グリフの外接矩形ぶんを一時バッファへ合成してから`pushImage()`で1回に送る。
2. **ラスタライズ結果をキャッシュする。** `FT_Load_Char(..., FT_LOAD_RENDER)`を
   毎回呼ぶと、同じ文字でも作り直しになる。実測で**203ms**を占めていた。
   キーは(文字コード, ピクセルサイズ)。カバレッジの実体はPSRAM上の
   固定アリーナ(512KB)に先頭詰めで置き、満杯なら全部捨てて作り直す。
   小さな確保を大量に行うとヒープが荒れるので、必ずアリーナ方式にする。

回転をスプライト側に持たせた副作用として、**描画は少し遅くなる**
(座標変換が描画側に移るため)。ただし全画面の回転転送1回分が消えるので差し引きで大勝ち。

---

## 7. 試して駄目だったこと

| 試したこと | 結果 |
|---|---|
| `bounce_buffer_size_px = 0` | **画面が真っ黒**。戻すこと |
| IO出力に`0xFF` / `0xDF`を書く | **画面が真っ黒**。`0x1E`から外れるのは用途のビットだけにする |
| `0x24`へ`{レジスタ番号, 値}`の2バイトを書く | モードレジスタが壊れる。CH32V003は1バイト |
| `EXIO5`をHighのままにする | アプリ起動と同時にCOMポートが消える |
| 副コンソールでログを読む | 無音。主コンソールにする必要がある |
| CH343(`USB TO UART`)側から書き込む | `No serial data received`。原因未特定 |
