// main/wifi_manager.h
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <string>

class WiFiManager {
public:
    WiFiManager(const std::string& ssid, const std::string& password, int max_retry = 5);
    ~WiFiManager();
    esp_err_t connect();
    void disconnect();
    bool isConnected() const;
    std::string getIpAddress() const; // 获取IP地址
private:
    // 内部使用的静态事件处理函数，作为C和C++的桥梁
    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    // 类的成员变量，存储Wi-Fi信息和状态
    std::string ssid_;
    std::string password_;
    int max_retry_;
    bool initialized_;

    // FreeRTOS 事件组，用于同步连接状态
    static EventGroupHandle_t s_wifi_event_group;
    static int s_retry_num;
    static const int WIFI_CONNECTED_BIT = BIT0;
    static const int WIFI_FAIL_BIT = BIT1;

    // 事件处理器实例句柄，用于注销
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    static esp_ip4_addr_t s_ip_addr;
};
#endif // WIFI_MANAGER_H