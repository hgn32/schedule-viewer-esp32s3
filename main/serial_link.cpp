#include "serial_link.h"

#include <cstring>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "serial";

static const uart_port_t PORT      = UART_NUM_0;
static const int         RX_BUF    = 4096;
// 1行が異常に長い(改行が来ない)場合に無制限に伸びないようにする上限。
static const size_t      MAX_PEND  = 8192;

static std::string s_pending;

void serialLinkBegin() {
    uart_config_t cfg = {};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));

    // UART0はブートローダとコンソールが既に開いているが、
    // uart_read_bytes()を使うにはドライバのインストールが要る。
    if (!uart_is_driver_installed(PORT)) {
        ESP_ERROR_CHECK(uart_driver_install(PORT, RX_BUF, 0, 0, nullptr, 0));
    }

    // UART0のピンは既定(TX=GPIO1 / RX=GPIO3)のまま。uart_set_pin()は呼ばない。
    s_pending.clear();
    ESP_LOGI(TAG, "UART0 115200bps ready");
}

bool serialLinkReadLine(std::string& out, uint32_t timeout_ms) {
    uint8_t buf[128];

    for (;;) {
        size_t nl = s_pending.find('\n');
        if (nl != std::string::npos) {
            out = s_pending.substr(0, nl);
            s_pending.erase(0, nl + 1);
            // 末尾のCRと空白のみ落とす。タブ区切りの本文は触らない。
            while (!out.empty() && (out.back() == '\r' || out.back() == ' ')) {
                out.pop_back();
            }
            return true;
        }

        int n = uart_read_bytes(PORT, buf, sizeof(buf), pdMS_TO_TICKS(timeout_ms));
        if (n <= 0) return false;

        s_pending.append((const char*)buf, (size_t)n);
        if (s_pending.size() > MAX_PEND) {
            ESP_LOGW(TAG, "改行の無い入力が%uバイトを超えたので破棄", (unsigned)MAX_PEND);
            s_pending.clear();
        }
    }
}

void serialLinkWriteLine(const char* line) {
    if (line == nullptr) return;
    uart_write_bytes(PORT, line, strlen(line));
    uart_write_bytes(PORT, "\n", 1);
}
