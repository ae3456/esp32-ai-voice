// main/bsp_board.h
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化音频输入 (麦克风)
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan);
// 初始化音频输出 (扬声器)
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan);
// 从麦克风读取数据
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);
// 播放音频数据
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len);
// 流式播放音频数据
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len);
// 停止音频输出
esp_err_t bsp_audio_stop(void);
#ifdef __cplusplus
}
#endif