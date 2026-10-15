#if defined(ESP_PLATFORM)

#include "mas/can/port/esp32_twai.h"

#include <driver/twai.h>
#include <esp_idf_version.h>
#include <esp_timer.h>

#include "mas/can/host.h"

namespace mas {
namespace can {

bool Esp32Twai::begin(gpio_num_t tx, gpio_num_t rx) {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_NORMAL);
    g.tx_queue_len = 8; // 短いほど SYNC が待たされない
    g.rx_queue_len = 64;
    // 1 Mbps: APB 80 MHz ÷ 4 ＝ 20 MHz、20 tq（1 ＋ 15 ＋ 4）、サンプル点 80%、SJW 4（C-9）
    twai_timing_config_t t = {};
    t.brp = 4;
    t.tseg_1 = 15;
    t.tseg_2 = 4;
    t.sjw = 4;
    t.triple_sampling = false;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL(); // 全部受ける（使用率を数えるため）
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    return twai_start() == ESP_OK;
}

bool Esp32Twai::send(const Frame& frame) {
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state == TWAI_STATE_BUS_OFF) {
            twai_initiate_recovery();
            return false;
        }
        if (st.state == TWAI_STATE_STOPPED) twai_start(); // 復帰が済んだ
    }
    twai_message_t m = {};
    m.identifier = frame.id;
    m.data_length_code = frame.dlc;
    m.rtr = frame.rtr;
    for (int i = 0; i < frame.dlc && i < 8; i++) m.data[i] = frame.data[i];
    return twai_transmit(&m, 0) == ESP_OK;
}

bool Esp32Twai::receive(Frame& frame) {
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        if (m.extd) continue; // 拡張 ID は使わない
        frame.id = (uint16_t)m.identifier;
        frame.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
        frame.rtr = m.rtr;
        for (int i = 0; i < 8; i++) frame.data[i] = m.data[i];
        return true;
    }
    return false;
}

uint32_t Esp32Twai::micros() { return (uint32_t)esp_timer_get_time(); }

uint32_t Esp32Twai::rxOverflows() {
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    return st.rx_missed_count + st.rx_overrun_count;
#else
    return st.rx_missed_count;
#endif
}

bool Host::begin(int tx, int rx) {
    static Esp32Twai twai;
    if (!twai.begin((gpio_num_t)tx, (gpio_num_t)rx)) {
        error_ = Error::PortStartFailed;
        return false;
    }
    return begin(twai);
}

} // namespace can
} // namespace mas

#endif
