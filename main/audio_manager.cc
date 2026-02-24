/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件
 * 
 * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
#include "esp_heap_caps.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
    , aec_reference_queue(nullptr)
    , is_finishing(false) // 初始化
{
    // 🧮 计算所需缓冲区大小
    recording_buffer_size = sample_rate * recording_duration_sec;  // 录音缓冲区（样本数）
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 响应缓冲区（字节数）
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器...");
    
    // 分配录音缓冲区
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             recording_buffer_size * sizeof(int16_t), (unsigned long)recording_duration_sec);
    
    // 分配响应缓冲区
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    // 分配流式播放缓冲区
    // 强制使用 PSRAM (外部内存)
    streaming_buffer = (uint8_t*)heap_caps_malloc(streaming_buffer_size, MALLOC_CAP_SPIRAM);
    
    // 如果板子没有 PSRAM 或者分配失败，回退到内部 RAM (但大小可能不够)
    if (streaming_buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM分配失败，尝试使用内部SRAM...");
        streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    }
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);
    // 创建播放任务 (优先级要适中，比如 5)
    // xTaskCreate(player_task, "audio_player", 8192, this, 5, &player_task_handle);
    xTaskCreatePinnedToCore(player_task, "audio_player", 8192, this, 5, &player_task_handle, 1);
    return ESP_OK;
}

void AudioManager::deinit() {
    if (player_task_handle != nullptr) {
        vTaskDelete(player_task_handle);
        player_task_handle = nullptr;
    }

    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }
    
    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }
    
    
    if (streaming_buffer != nullptr) {
        heap_caps_free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

// 🎙️ ========== 录音功能实现 ==========

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    ESP_LOGI(TAG, "开始录音...");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", 
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }
    
    // 📏 检查缓冲区是否还有空间
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满（超过10秒上限）");
        return false;
    }
    
    // 💾 将新的音频数据追加到缓冲区末尾
    memcpy(&recording_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return recording_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

size_t AudioManager::getRecordingLength() const {
    return recording_length;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= recording_buffer_size;
}

// 🔊 ========== 音频播放功能实现 ==========

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 🔁 添加重试机制，确保音频可靠播放
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}


// 🌊 ========== 流式播放功能实现 ==========

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放");
    is_streaming = true;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    // 清空缓冲区
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 📏 计算环形缓冲区的剩余空间
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        // 写指针在读指针后面
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        // 写指针在读指针前面（已绕回）
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }
    
    if (size > available_space) {
        ESP_LOGW(TAG, "流式缓冲区空间不足: 需要 %zu, 可用 %zu", size, available_space);
        return false;
    }
    
    // 📝 将数据写入环形缓冲区
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        // 简单情况：数据不跨越缓冲区末尾
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        // 复杂情况：数据跨越末尾，需要分两段写入
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    // 如果写位置到达缓冲区末尾，循环回到开头
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    ESP_LOGD(TAG, "添加流式音频块: %zu 字节, 写位置: %zu, 读位置: %zu", 
             size, streaming_write_pos, streaming_read_pos);
    
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "结束流式音频播放");
    is_finishing = true;
}

void AudioManager::player_task(void* pvParameters) {
    AudioManager* manager = (AudioManager*)pvParameters;
    // 在堆上分配临时缓冲区，而不是在栈上
    uint8_t* temp_buffer = (uint8_t*)malloc(STREAMING_CHUNK_SIZE);
    if (temp_buffer == nullptr) {
        ESP_LOGE(TAG, "播放任务临时缓冲区分配失败！任务退出。");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        // 检查是否在流式播放模式
        if (!manager->is_streaming) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 检查缓冲区数据量
        // 注意：这里需要处理一下线程安全，或者简单点，直接读取
        // 因为是单生产者(Net)-单消费者(Audio)，简单的读写指针操作通常是安全的，
        // 但为了严谨，最好加个互斥锁。不过为了演示，我们先用简单逻辑：
        
        size_t available_data;
        if (manager->streaming_write_pos >= manager->streaming_read_pos) {
            available_data = manager->streaming_write_pos - manager->streaming_read_pos;
        } else {
            available_data = manager->streaming_buffer_size - manager->streaming_read_pos + manager->streaming_write_pos;
        }

        if (available_data >= STREAMING_CHUNK_SIZE) {
            // 从环形缓冲区读取数据
            size_t bytes_to_end = manager->streaming_buffer_size - manager->streaming_read_pos;
            if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
                memcpy(temp_buffer, manager->streaming_buffer + manager->streaming_read_pos, STREAMING_CHUNK_SIZE);
                manager->streaming_read_pos += STREAMING_CHUNK_SIZE;
            } else {
                memcpy(temp_buffer, manager->streaming_buffer + manager->streaming_read_pos, bytes_to_end);
                memcpy(temp_buffer + bytes_to_end, manager->streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
                manager->streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
            }

            // 环形回绕
            if (manager->streaming_read_pos >= manager->streaming_buffer_size) {
                manager->streaming_read_pos = 0;
            }

            // 播放！(这里是阻塞的，但因为在独立任务里，不会卡住网络接收)
            // 播放 (这里阻塞是没问题的，因为是在独立任务里)
            esp_err_t ret = bsp_play_audio_stream(temp_buffer, STREAMING_CHUNK_SIZE);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "流式播放I2S写入失败: %s", esp_err_to_name(ret));
            }
            // 发送 AEC 参考信号
            int16_t* audio_samples = (int16_t*)temp_buffer;
            size_t sample_count = STREAMING_CHUNK_SIZE / sizeof(int16_t);
            manager->sendAECReference(audio_samples, sample_count);
            
        } else if (manager->is_finishing && available_data > 0) {
            // --- 收尾阶段：播放剩余的不足一个块的数据 ---
            ESP_LOGI(TAG, "任务处理剩余尾巴: %zu 字节", available_data);

            // 这里可以用 temp_buffer 复用，不用再 malloc temp_chunk 了，省内存
            // 读取剩余数据
            if (manager->streaming_write_pos >= manager->streaming_read_pos) {
                memcpy(temp_buffer, manager->streaming_buffer + manager->streaming_read_pos, available_data);
            } else {
                size_t bytes_to_end = manager->streaming_buffer_size - manager->streaming_read_pos;
                memcpy(temp_buffer, manager->streaming_buffer + manager->streaming_read_pos, bytes_to_end);
                memcpy(temp_buffer + bytes_to_end, manager->streaming_buffer, available_data - bytes_to_end);
            }

            bsp_play_audio_stream(temp_buffer, available_data);
            
            // 播放完毕，重置状态
            manager->streaming_read_pos = 0;
            manager->streaming_write_pos = 0;
            manager->is_finishing = false;
            manager->is_streaming = false; // 任务自己宣布下班
            
            // 停止 I2S 输出以防噪音
            bsp_audio_stop();
            ESP_LOGI(TAG, "流式播放自然结束");

        } else if (manager->is_finishing && available_data == 0) {
            // --- 收尾阶段：没有数据了 ---
            manager->is_finishing = false;
            manager->is_streaming = false;
            bsp_audio_stop();
            ESP_LOGI(TAG, "流式播放自然结束 (无剩余数据)");
            
        } else {
            // 数据不够，休息一下，避免死循环占用 CPU
            vTaskDelay(pdMS_TO_TICKS(10)); 
        }
    }
    // 理论上不会运行到这里，但为了严谨，如果任务退出要释放内存
    free(temp_buffer);
}


// 🔇 ========== AEC支持功能实现 ==========

void AudioManager::setAECReferenceQueue(QueueHandle_t queue_handle) {
    aec_reference_queue = queue_handle;
    ESP_LOGI(TAG, "🔇 AEC参考队列句柄已设置: %p", (void*)queue_handle);
}

bool AudioManager::sendAECReference(const int16_t* audio_data, size_t samples) {
    if (aec_reference_queue == NULL || audio_data == NULL || samples == 0) {
        return false;
    }

    // 分配临时缓冲区存储AEC参考数据
    struct aec_ref_data {
        int16_t audio_data[320];  // 假设最大320样本（20ms @ 16kHz）
        size_t samples;
        uint32_t timestamp;
    };

    static aec_ref_data ref_data;

    // 限制样本数量，避免队列溢出
    size_t samples_to_send = (samples > 320) ? 320 : samples;

    // 复制音频数据
    memcpy(ref_data.audio_data, audio_data, samples_to_send * sizeof(int16_t));
    ref_data.samples = samples_to_send;
    ref_data.timestamp = xTaskGetTickCount();

    // 非阻塞发送到队列
    BaseType_t result = xQueueSend(aec_reference_queue, &ref_data, 0);
    if (result == pdTRUE) {
        ESP_LOGD(TAG, "🔇 AEC参考: 发送 %zu 样本到队列", samples_to_send);
        return true;
    } else {
        ESP_LOGD(TAG, "🔇 AEC参考: 队列满，丢弃 %zu 样本", samples_to_send);
        return false;
    }
}

