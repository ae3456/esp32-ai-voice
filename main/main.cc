/**
  * @file main.cc
  * @brief ESP32-S3 智能语音助手 - 核心对话循环实验
  * 实验目标：
  * 实现一个完整的“唤醒 -> 提问 -> 回答 -> 继续提问”的连续对话循环。
  * 1. 语音唤醒 - 支持"你好小智"唤醒词。
  * 2. 录音与发送 - 唤醒后录制用户语音，并通过WebSocket发送。
  * 3. 接收与播放 - 接收服务器返回的音频并播放。
  * 4. 连续对话 - 播放完毕后自动进入下一轮录音，等待用户继续提问。
  */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h" // 流缓冲区
#include "freertos/event_groups.h"  // 事件组
// #include "mbedtls/base64.h"      // 未使用，已注释
#include "esp_timer.h"              // ESP定时器，用于获取时间戳
#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "esp_vad.h"                // VAD接口
#include "esp_nsn_iface.h"          // 噪音抑制接口
#include "esp_nsn_models.h"         // 噪音抑制模型
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/hi.h"         // 欢迎音频数据文件
#include "mock_voices/ok.h"         // 确认音频数据文件
#include "mock_voices/bye.h"        // 再见音频数据文件
#include "mock_voices/custom.h"     // 自定义音频数据文件
#include "driver/gpio.h"            // GPIO驱动
#include "nvs_flash.h"              // NVS存储
}

#include "audio_manager.h"          // 音频管理器
#include "wifi_manager.h"           // WiFi管理器
#include "websocket_client.h"        // WebSocket客户端

static const char *TAG = "语音识别"; // 日志标签

// WebSocket服务器配置
#define WS_URI "ws://139.196.221.55:8888/ws/esp32" // 请改为您的电脑IP地址:8888

// WiFi和WebSocket管理器
static WiFiManager* wifi_manager = nullptr;
static WebSocketClient* websocket_client = nullptr;

// --- 3. 核心状态机 ---
typedef enum
{
   STATE_WAITING_WAKEUP = 0,   // 状态一：等待唤醒
   STATE_RECORDING = 1,        // 状态二：正在录音
   STATE_WAITING_RESPONSE = 2, // 状态三：等待AI回复
   STATE_PLAYING_FINISHED_WAITING = 3, // 【新增】状态四：回复接收完毕，等待播放结束
   STATE_PLAYING_WEATHER = 4   // 【新增】状态五：正在播放天气播报
} system_state_t;

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
// static TickType_t command_timeout_start = 0; // 未使用
static const TickType_t COMMAND_TIMEOUT_MS = 5000; // 5秒超时

// VAD（语音活动检测）相关变量
static vad_handle_t vad_inst = NULL;

// NS（噪音抑制）相关变量  
static esp_nsn_iface_t *nsn_handle = NULL;
static esp_nsn_data_t *nsn_model_data = NULL;

// 音频参数
#define SAMPLE_RATE 16000 // 采样率 16kHz

// 音频管理器
static AudioManager* audio_manager = nullptr;

// VAD（语音活动检测）相关变量
static bool vad_speech_detected = false;
static int vad_silence_frames = 0;
static const int VAD_SILENCE_FRAMES_REQUIRED = 20; // 约600ms静音判断为结束

// 连续对话功能相关变量
static bool is_continuous_conversation = false;
static TickType_t recording_timeout_start = 0;
#define RECORDING_TIMEOUT_MS 10000
static bool user_started_speaking = false;

// 实时流式传输标志
static bool is_realtime_streaming = false;

// 天气播报相关标志
static bool is_weather_report = false;
static char weather_trigger_source[32] = {0}; // 存储触发者ID

/**
* @brief WebSocket事件处理函数
*/
static void on_websocket_event(const WebSocketClient::EventData& event)
{
   switch (event.type)
   {
   case WebSocketClient::EventType::CONNECTED:
       ESP_LOGI(TAG, "WebSocket已连接");
       break;
   case WebSocketClient::EventType::DISCONNECTED:
       ESP_LOGI(TAG, "WebSocket已断开");
       break;
   case WebSocketClient::EventType::DATA_BINARY:
   // 收到服务器发来的AI语音数据
   {
       ESP_LOGI(TAG, "收到WebSocket二进制数据，长度: %zu 字节", event.data_len);
       
       // 调试：打印小数据包的内容（可能是错误消息）
       if (event.data_len < 100 && event.data != nullptr) {
           char debug_buf[128] = {0};
           memcpy(debug_buf, event.data, event.data_len > 127 ? 127 : event.data_len);
           ESP_LOGI(TAG, "二进制数据内容: %s", debug_buf);
       }
       
       if (audio_manager != nullptr && event.data_len > 0 && 
           (current_state == STATE_WAITING_RESPONSE || current_state == STATE_PLAYING_WEATHER)) {
            // 先检查是否已经开始播放，避免竞态条件重复发送
            bool was_already_streaming = audio_manager->isStreamingActive();
            
            if (!was_already_streaming) {
                ESP_LOGI(TAG, "开始流式音频播放");
                audio_manager->startStreamingPlayback();
            }
            bool added = audio_manager->addStreamingAudioChunk(event.data, event.data_len);
            
            if (added) {
                ESP_LOGD(TAG, "添加流式音频块: %zu 字节", event.data_len);
            } else {
                ESP_LOGW(TAG, "流式音频缓冲区满");
            }
       }
   }
   break;

   case WebSocketClient::EventType::PING:
        // 用 PING 包作为流式音频结束的标志
        ESP_LOGD(TAG, "收到ping包");
        break;

   case WebSocketClient::EventType::DATA_TEXT:
       if (event.data && event.data_len > 0) {
           char *json_str = (char *)malloc(event.data_len + 1);
            if (json_str) {
                memcpy(json_str, event.data, event.data_len);
                json_str[event.data_len] = '\0';
                ESP_LOGI(TAG, "收到JSON消息: %s", json_str);
                if (strstr(json_str, "response_finished") != NULL) {
                    if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
                        ESP_LOGI(TAG, "收到结束信号，停止流式接收，等待播放缓冲区排空...");

                        // 1. 告诉 AudioManager 网络数据传完了，剩下的自己播完
                        audio_manager->finishStreamingPlayback();

                        // 2. 根据当前状态决定下一步
                        if (current_state == STATE_WAITING_RESPONSE) {
                            current_state = STATE_PLAYING_FINISHED_WAITING;
                        } else if (current_state == STATE_PLAYING_WEATHER) {
                            // 天气播报也在等待播放结束，保持当前状态
                            ESP_LOGI(TAG, "天气播报接收完成，等待播放结束...");
                        }
                    } else {
                        // 🔧 修复：如果没有在播放（比如TTS失败返回空音频），
                        ESP_LOGW(TAG, "收到结束信号但没有音频在播放，可能是TTS失败");


                        // 根据状态决定下一步
                        if (current_state == STATE_WAITING_RESPONSE) {
                            current_state = STATE_RECORDING;
                            audio_manager->clearRecordingBuffer();
                            audio_manager->startRecording();
                            vad_speech_detected = false;
                            vad_silence_frames = 0;
                            ESP_LOGI(TAG, "进入录音状态（无音频回复）");
                        } else if (current_state == STATE_PLAYING_WEATHER) {
                            // 天气播报无音频，返回等待唤醒
                            current_state = STATE_WAITING_WAKEUP;
                            is_weather_report = false;
                            ESP_LOGI(TAG, "天气播报无音频，返回等待唤醒状态");
                        }
                    }
                } else if (strstr(json_str, "\"event\":\"ping\"") != NULL) {
                    // 处理服务器心跳ping，忽略或记录
                    ESP_LOGD(TAG, "收到服务器心跳ping");
                    // 可选：发送pong响应，但服务器期望的是JSON ping，不是协议层pong
                } else if (strstr(json_str, "\"event\":\"error\"") != NULL) {
                    // 处理错误消息
                    ESP_LOGE(TAG, "收到服务器错误消息: %s", json_str);
                        // 根据状态决定下一步
                    if (current_state == STATE_WAITING_RESPONSE) {
                        current_state = STATE_RECORDING;
                        audio_manager->clearRecordingBuffer();
                        audio_manager->startRecording();
                        vad_speech_detected = false;
                        vad_silence_frames = 0;
                        ESP_LOGI(TAG, "进入录音状态（服务器错误）");
                    }
                } else if (strstr(json_str, "\"event\":\"play_weather\"") != NULL) {
                    // 🌤️ 收到天气播报指令
                    ESP_LOGI(TAG, "收到天气播报指令!");
                    
                    // 提取触发者信息
                    char* triggered_by = strstr(json_str, "\"triggered_by\":\"");
                    if (triggered_by) {
                        triggered_by += strlen("\"triggered_by\":\"");
                        char* end = strchr(triggered_by, '\"');
                        if (end) {
                            size_t len = end - triggered_by;
                            if (len > sizeof(weather_trigger_source) - 1) {
                                len = sizeof(weather_trigger_source) - 1;
                            }
                            strncpy(weather_trigger_source, triggered_by, len);
                            weather_trigger_source[len] = '\0';
                        }
                    }
                    
                    // 停止当前录音
                    if (audio_manager->isRecording()) {
                        audio_manager->stopRecording();
                    }
                    
                    // 清空缓冲区准备接收天气音频
                    audio_manager->clearRecordingBuffer();
                    
                    // 设置天气播报标志
                    is_weather_report = true;
                    
                    // 切换到天气播报状态
                    current_state = STATE_PLAYING_WEATHER;
                    
                    ESP_LOGI(TAG, "🌤️ 准备接收天气播报音频，触发者: %s", weather_trigger_source);
                }
                free(json_str);
            }
       }
       break;

   case WebSocketClient::EventType::ERROR:
       ESP_LOGI(TAG, "WebSocket错误");
       break;

   default:
       break;
   }
}

// 播放本地音频的辅助函数
static esp_err_t play_audio_with_stop(const uint8_t *audio_data, size_t data_len, const char *description)
{
   if (audio_manager != nullptr) {
       return audio_manager->playAudio(audio_data, data_len, description);
   }
   return ESP_ERR_INVALID_STATE;
}

// 退出连续对话的逻辑
static void execute_exit_logic(void)
{
   ESP_LOGI(TAG, "播放再见音频...");
   play_audio_with_stop(bye, bye_len, "再见音频");

   if (websocket_client != nullptr) {
       websocket_client->disconnect();
   }

   current_state = STATE_WAITING_WAKEUP;
   if (audio_manager != nullptr) {
       audio_manager->stopRecording();
       audio_manager->clearRecordingBuffer();
   }
   is_continuous_conversation = false;
   user_started_speaking = false;
   recording_timeout_start = 0;
   vad_speech_detected = false;
   vad_silence_frames = 0;

   ESP_LOGI(TAG, "返回等待唤醒状态，请说出唤醒词 '你好小智'");
}

// --- 5. 程序主入口 ---
extern "C" void app_main(void)
{
    // --- 初始化阶段 ---
    // 需要清理的资源指针
    srmodel_list_t *models = nullptr;
    esp_wn_iface_t *wakenet = nullptr;
    model_iface_data_t *model_data = nullptr;
    int16_t *buffer = nullptr;
    char *model_name = nullptr;
    int16_t *ns_out_buffer = nullptr;  // 噪音抑制输出缓冲区
    int audio_chunksize = 0;           // 音频块大小，稍后初始化
    size_t free_heap = 0;              // 内存状态变量，稍后初始化
    size_t free_internal = 0;
    size_t free_spiram = 0;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "正在连接WiFi...");
    wifi_manager = new WiFiManager(CONFIG_MY_WIFI_SSID, CONFIG_MY_WIFI_PASSWORD);
    if (wifi_manager->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi连接失败");
        goto cleanup;
    }

    ESP_LOGI(TAG, "正在连接WebSocket服务器...");
    websocket_client = new WebSocketClient(WS_URI, true, 5000);
    websocket_client->setEventCallback(on_websocket_event);
    if (websocket_client->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket连接失败");
        goto cleanup;
    }

    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ret = bsp_board_init(16000, 1, 16);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "INMP441麦克风初始化成功");

    ESP_LOGI(TAG, "正在初始化音频播放功能...");
    ret = bsp_audio_init(16000, 1, 16);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "音频播放初始化成功");

    ESP_LOGI(TAG, "正在初始化语音活动检测（VAD）...");
    vad_inst = vad_create_with_param(VAD_MODE_1, SAMPLE_RATE, 30, 200, 1000);
    if (vad_inst == NULL) {
        ESP_LOGE(TAG, "创建VAD实例失败");
        goto cleanup;
    }
    ESP_LOGI(TAG, "VAD初始化成功");

    ESP_LOGI(TAG, "正在加载唤醒词检测模型...");
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "内存状态检查:");
    ESP_LOGI(TAG, "  - 总可用内存: %zu KB", free_heap / 1024);
    ESP_LOGI(TAG, "  - 内部RAM: %zu KB", free_internal / 1024);
    ESP_LOGI(TAG, "  - PSRAM: %zu KB", free_spiram / 1024);

   models = esp_srmodel_init("model");
   if (models == NULL) {
       ESP_LOGE(TAG, "语音识别模型初始化失败");
       goto cleanup;
   }
   model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
   if (model_name == NULL) {
       ESP_LOGE(TAG, "未找到任何唤醒词模型！");
       goto cleanup;
   }
   ESP_LOGI(TAG, "选择唤醒词模型: %s", model_name);
   wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
   if (wakenet == NULL) {
       ESP_LOGE(TAG, "获取唤醒词接口失败，模型: %s", model_name);
       goto cleanup;
   }
   model_data = wakenet->create(model_name, DET_MODE_90);
   if (model_data == NULL) {
       ESP_LOGE(TAG, "创建唤醒词模型数据失败");
       goto cleanup;
   }

   audio_chunksize = wakenet->get_samp_chunksize(model_data) * sizeof(int16_t);
   buffer = (int16_t *)malloc(audio_chunksize);
   if (buffer == NULL) {
       ESP_LOGE(TAG, "音频缓冲区内存分配失败");
       goto cleanup;
   }

   audio_manager = new AudioManager(SAMPLE_RATE, 10, 32);
   ret = audio_manager->init();
   if (ret != ESP_OK) {
       ESP_LOGE(TAG, "音频管理器初始化失败: %s", esp_err_to_name(ret));
       goto cleanup;
   }
   ESP_LOGI(TAG, "音频管理器初始化成功");

   ESP_LOGI(TAG, "智能语音助手系统配置完成，请说出唤醒词 '你好小智'");

   // --- 主循环 ---
   while (1)
   {
        // 从麦克风读取音频数据
        ret = bsp_get_feed_data(false, buffer, audio_chunksize);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int16_t *processed_audio = buffer;
        // 噪音抑制输出缓冲区
        if (nsn_handle != NULL && nsn_model_data != NULL) {
            // 如果输出缓冲区未分配，分配它
            if (ns_out_buffer == NULL) {
                int ns_chunksize = nsn_handle->get_samp_chunksize(nsn_model_data);
                ns_out_buffer = (int16_t *)malloc(ns_chunksize * sizeof(int16_t));
                if (ns_out_buffer == NULL) {
                    ESP_LOGW(TAG, "噪音抑制输出缓冲区分配失败");
                    nsn_handle = NULL;  // 禁用噪音抑制
                }
            }
            
            if (ns_out_buffer != NULL) {
                // 执行噪音抑制
                nsn_handle->process(nsn_model_data, buffer, ns_out_buffer);
                processed_audio = ns_out_buffer;  // 使用噪音抑制后的数据
            }
        }
       if (current_state == STATE_WAITING_WAKEUP)
       {
           // 休眠状态：监听唤醒词
           wakenet_state_t wn_state = wakenet->detect(model_data, processed_audio);
           if (wn_state == WAKENET_DETECTED)
           {
               ESP_LOGI(TAG, "检测到唤醒词 '你好小智'！");

               if (websocket_client != nullptr && !websocket_client->isConnected()) {
                   ESP_LOGI(TAG, "WebSocket未连接，正在重连...");
                   websocket_client->connect();
                   vTaskDelay(pdMS_TO_TICKS(500));
               }

               if (websocket_client != nullptr && websocket_client->isConnected()) {
                   const char* start_msg = "{\"event\":\"recording_started\"}";
                   websocket_client->sendText(start_msg);
               }

               play_audio_with_stop(hi, hi_len, "欢迎音频");

               // 进入录音状态
               current_state = STATE_RECORDING;
               audio_manager->startRecording();

               // 初始化状态变量
               vad_speech_detected = false;
               vad_silence_frames = 0;
               is_continuous_conversation = false;
               user_started_speaking = false;
               recording_timeout_start = 0;
               is_realtime_streaming = false;

               vad_reset_trigger(vad_inst);

               ESP_LOGI(TAG, "开始录音，请说话...");
           }
       }
       else if (current_state == STATE_RECORDING)
       {
           // 录音状态：记录用户说的话
           if (audio_manager->isRecording() && !audio_manager->isRecordingBufferFull())
           {
               int samples = audio_chunksize / sizeof(int16_t);
               audio_manager->addRecordingData(processed_audio, samples);

               if (is_realtime_streaming && websocket_client != nullptr && websocket_client->isConnected()) {
                   websocket_client->sendBinary((const uint8_t*)processed_audio, samples * sizeof(int16_t));
               }

               // 使用VAD检测用户是否在说话
               vad_state_t vad_state = vad_process(vad_inst, processed_audio, SAMPLE_RATE, 30);

                if (vad_state == VAD_SPEECH) {
                    vad_speech_detected = true;
                    vad_silence_frames = 0;
                    user_started_speaking = true;
                    recording_timeout_start = 0;

                    if (!is_realtime_streaming) {
                        is_realtime_streaming = true;
                        ESP_LOGI(TAG, "检测到说话，补发前500ms数据并开始实时传输...");
                        // 1. 计算需要回溯多少数据 (比如 500ms)
                        // 500ms * 16000Hz = 8000 样本
                        const size_t PREROLL_SAMPLES = 8000; 
                        // 每次最多发送 1000 样本 (2000 字节)，避免缓冲区溢出
                        const size_t MAX_CHUNK_SAMPLES = 1000;
                        
                        size_t current_len = 0;
                        const int16_t* full_buffer = audio_manager->getRecordingBuffer(current_len);
                        
                        // 2. 计算起始位置
                        size_t start_pos = 0;
                        if (current_len > PREROLL_SAMPLES) {
                            start_pos = current_len - PREROLL_SAMPLES;
                        }
                        
                        // 3. 计算要发送的总长度
                        size_t send_samples = current_len - start_pos;
                        
                        // 4. 【关键修复】分块发送，避免一次性发送太多导致断开
                        if (send_samples > 0 && websocket_client != nullptr && websocket_client->isConnected()) {
                            size_t sent = 0;
                            bool send_failed = false;
                            while (sent < send_samples && websocket_client->isConnected() && !send_failed) {
                                size_t chunk = (send_samples - sent > MAX_CHUNK_SAMPLES) ? 
                                               MAX_CHUNK_SAMPLES : (send_samples - sent);
                                
                                // 【关键】检查发送返回值，失败则停止
                                int ret = websocket_client->sendBinary(
                                    (const uint8_t*)(full_buffer + start_pos + sent), 
                                    chunk * sizeof(int16_t),
                                    500  // 500ms超时
                                );
                                
                                if (ret < 0) {
                                    ESP_LOGW(TAG, "发送音频块失败 (%d)，停止补发", ret);
                                    send_failed = true;
                                    break;
                                }
                                
                                sent += chunk;
                                
                                // 增加延时，给服务器处理时间
                                if (sent < send_samples && websocket_client->isConnected()) {
                                    vTaskDelay(pdMS_TO_TICKS(20)); // 增加到20ms
                                }
                            }
                            if (!send_failed) {
                                ESP_LOGI(TAG, "已补发 %zu/%zu 样本的历史音频", sent, send_samples);
                            } else {
                                ESP_LOGW(TAG, "补发中断，已发送 %zu/%zu 样本", sent, send_samples);
                            }
                        }
                    }

                   // 显示录音进度（每100ms显示一次）
                    static TickType_t last_log_time = 0;
                    TickType_t current_time = xTaskGetTickCount();
                    if (current_time - last_log_time > pdMS_TO_TICKS(100)) {
                        ESP_LOGD(TAG, "正在录音... 当前长度: %.2f 秒", audio_manager->getRecordingDuration());
                        last_log_time = current_time;
                    }

               } else if (vad_state == VAD_SILENCE && vad_speech_detected) {
                   vad_silence_frames++;

                   if (vad_silence_frames >= VAD_SILENCE_FRAMES_REQUIRED) {
                       ESP_LOGI(TAG, "VAD检测到用户说话结束，录音长度: %.2f 秒", audio_manager->getRecordingDuration());
                       audio_manager->stopRecording();
                       is_realtime_streaming = false;

                       size_t rec_len = 0;
                       audio_manager->getRecordingBuffer(rec_len);
                       if (user_started_speaking && rec_len > SAMPLE_RATE / 4)
                       {
                           if (websocket_client != nullptr && websocket_client->isConnected()) {
                               const char* end_msg = "{\"event\":\"recording_ended\"}";
                               websocket_client->sendText(end_msg);
                           }
                           current_state = STATE_WAITING_RESPONSE;
                           audio_manager->resetResponsePlayedFlag();
                           ESP_LOGI(TAG, "等待服务器响应音频...");
                       }
                       else
                       {
                            ESP_LOGI(TAG, "录音时间过短或用户未说话，重新开始录音");
                            // 发送录音取消事件
                            if (websocket_client != nullptr && websocket_client->isConnected())
                            {
                                const char* cancel_msg = "{\"event\":\"recording_cancelled\"}";
                                websocket_client->sendText(cancel_msg);
                            }
                            // 重新开始录音
                            audio_manager->clearRecordingBuffer();
                            audio_manager->startRecording();
                            vad_speech_detected = false;
                            vad_silence_frames = 0;
                            user_started_speaking = false;
                            is_realtime_streaming = !is_continuous_conversation;  // 只在非连续对话模式下开启流式传输
                            if (is_continuous_conversation)
                            {
                                recording_timeout_start = xTaskGetTickCount();
                            }
                            vad_reset_trigger(vad_inst);
                            // multinet->clean(mn_model_data);
                        }
                   }
               }
           }
           else if (audio_manager->isRecordingBufferFull())
           {
               ESP_LOGW(TAG, "录音缓冲区已满，停止录音");
               audio_manager->stopRecording();
               is_realtime_streaming = false;

               if (websocket_client != nullptr && websocket_client->isConnected()) {
                   const char* end_msg = "{\"event\":\"recording_ended\"}";
                   websocket_client->sendText(end_msg);
               }
               current_state = STATE_WAITING_RESPONSE;
               audio_manager->resetResponsePlayedFlag();
               ESP_LOGI(TAG, "等待服务器响应音频...");
           }

           // 连续对话模式下，检查是否超时没说话
           if (is_continuous_conversation && recording_timeout_start > 0 && !user_started_speaking)
           {
               TickType_t current_time = xTaskGetTickCount();
               if ((current_time - recording_timeout_start) > pdMS_TO_TICKS(RECORDING_TIMEOUT_MS))
               {
                   ESP_LOGW(TAG, "超过10秒没说话，退出对话");
                   audio_manager->stopRecording();
                   execute_exit_logic();
               }
               // 每秒提示一次剩余时间
                static TickType_t last_timeout_log = 0;
                if (current_time - last_timeout_log > pdMS_TO_TICKS(1000))
                {
                    int remaining_seconds = (RECORDING_TIMEOUT_MS - (current_time - recording_timeout_start) * portTICK_PERIOD_MS) / 1000;
                    if (remaining_seconds > 0)
                    {
                        ESP_LOGI(TAG, "等待用户说话... 剩余 %d 秒", remaining_seconds);
                    }
                    last_timeout_log = current_time;
                }
           }
       }
       else if (current_state == STATE_WAITING_RESPONSE)
       {
           // 等待状态：等待服务器的AI回复并检查播放是否完成
           
           // 检查连接是否断开
           if (websocket_client != nullptr && !websocket_client->isConnected()) {
               ESP_LOGW(TAG, "WebSocket连接断开，等待重连...");
               vTaskDelay(pdMS_TO_TICKS(1000));
               continue;  // 跳过本次循环，等待重连
           }
           
           // 定期发送 ping 保活，防止 WebSocket 空闲断开
           static TickType_t last_ping_time = 0;
           TickType_t now = xTaskGetTickCount();
           if (now - last_ping_time > pdMS_TO_TICKS(5000)) {  // 每5秒 ping 一次
               if (websocket_client != nullptr && websocket_client->isConnected()) {
                   websocket_client->sendPing();
               }
               last_ping_time = now;
           }
           
           if (audio_manager->isResponsePlayed())
           {
                // 在播放停止和下一次录音开始之间，给I2S驱动和DMA一个短暂的稳定时间。
                // ESP_LOGI(TAG, "播放完成，延时100ms以稳定硬件状态...");
                // vTaskDelay(pdMS_TO_TICKS(100)); // 延时100毫秒
                // AI回复完毕，进入连续对话模式
                if (websocket_client != nullptr && websocket_client->isConnected()) {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
               }

               current_state = STATE_RECORDING;
               audio_manager->clearRecordingBuffer();
               audio_manager->startRecording();
               vad_speech_detected = false;
               vad_silence_frames = 0;
               is_continuous_conversation = true;
               user_started_speaking = false;
               recording_timeout_start = xTaskGetTickCount();
               is_realtime_streaming = false;
               audio_manager->resetResponsePlayedFlag();
               vad_reset_trigger(vad_inst);
               ESP_LOGI(TAG, "进入连续对话模式，请在%d秒内继续说话...", RECORDING_TIMEOUT_MS / 1000);
           }
        }  else if (current_state == STATE_PLAYING_FINISHED_WAITING) {
            // 检查 AudioManager 是否还在播放
            // 注意：我们在 audio_manager.cc 的 player_task 里写了：
            // 当数据播完后，会设置 is_streaming = false
            if (!audio_manager->isStreamingActive()) {
                
                ESP_LOGI(TAG, "播放逻辑结束，等待硬件静音...");
                // 等待 I2S 硬件彻底播完，并让扬声器余振消失
                vTaskDelay(pdMS_TO_TICKS(500)); 
                // -----------------------------------------
                ESP_LOGI(TAG, "播放彻底结束，转入录音状态");
                // --- 这里才是真正开始录音的时机 ---
                
                // 1. 通知服务器开始新一轮录音
                if (websocket_client != nullptr && websocket_client->isConnected()) {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
                }
                
                // 清空录音缓冲区，防止残留
                audio_manager->clearRecordingBuffer(); 
                
                // 进入录音状态
                current_state = STATE_RECORDING;
                audio_manager->startRecording();
                
                // 重置所有计数器
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = true; // 保持连续对话
                user_started_speaking = false;
                recording_timeout_start = xTaskGetTickCount(); // 【关键】现在才开始倒计时！
                is_realtime_streaming = false;
                
                // 重置 VAD
                vad_reset_trigger(vad_inst);
                
                ESP_LOGI(TAG, "进入连续对话模式，请在10秒内继续说话...");
            } else {
                // 还在播放尾巴，稍微等一下，不要占用 CPU
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else if (current_state == STATE_PLAYING_WEATHER) {
            // 🌤️ 天气播报播放状态
            if (!audio_manager->isStreamingActive()) {
                // 天气播报播放完成
                ESP_LOGI(TAG, "🌤️ 天气播报播放完成");
                
                // 通知服务器天气播报完成
                if (websocket_client != nullptr && websocket_client->isConnected()) {
                    const char* weather_done_msg = "{\"event\":\"weather_played\"}";
                    websocket_client->sendText(weather_done_msg);
                    ESP_LOGI(TAG, "已通知服务器天气播报完成");
                }
                
                // 等待硬件稳定
                vTaskDelay(pdMS_TO_TICKS(500));
                
                // 重置天气播报标志
                is_weather_report = false;
                memset(weather_trigger_source, 0, sizeof(weather_trigger_source));
                
                // 返回等待唤醒状态（天气播报后不进入连续对话）
                current_state = STATE_WAITING_WAKEUP;
                
                // 重置所有状态
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = false;
                user_started_speaking = false;
                recording_timeout_start = 0;
                is_realtime_streaming = false;
                
                ESP_LOGI(TAG, "天气播报结束，返回等待唤醒状态，请说出唤醒词 '你好小智'");
            } else {
                // 还在播放中
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
   }

cleanup:
   // 资源清理
   ESP_LOGI(TAG, "正在清理系统资源...");
   if (vad_inst != NULL) vad_destroy(vad_inst);
   if (model_data != NULL) wakenet->destroy(model_data);
   if (buffer != NULL) free(buffer);
   if (ns_out_buffer != NULL) free(ns_out_buffer);
   // 注意：models 由 esp_srmodel_deinit 释放，但 esp-sr 库可能没有提供此函数
   if (websocket_client != nullptr) delete websocket_client;
   if (wifi_manager != nullptr) delete wifi_manager;
   if (audio_manager != nullptr) delete audio_manager;
   vTaskDelete(NULL);
}