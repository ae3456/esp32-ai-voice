#!/bin/bash
# 恢复所有重要配置

echo "1. 恢复 WiFi 配置..."
sed -i 's/CONFIG_MY_WIFI_SSID="YOUR_SSID"/CONFIG_MY_WIFI_SSID="HONOR 100 Pro"/' sdkconfig
sed -i 's/CONFIG_MY_WIFI_PASSWORD="YOUR_PASSWORD"/CONFIG_MY_WIFI_PASSWORD="55555555"/' sdkconfig

echo "2. 恢复唤醒词配置（你好小智）..."
# 先禁用所有唤醒词
sed -i 's/CONFIG_SR_WN_WN9S_HIEXIN=y/# CONFIG_SR_WN_WN9S_HIEXIN is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9S_HIESP=y/# CONFIG_SR_WN_WN9S_HIESP is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y/# CONFIG_SR_WN_WN9S_NIHAOXIAOZHI is not set/' sdkconfig
# 启用"你好小智"唤醒词
sed -i 's/# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set/CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y/' sdkconfig

echo "3. 恢复 Flash 大小配置..."
sed -i 's/CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y/# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set/' sdkconfig
sed -i 's/# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set/CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y/' sdkconfig
sed -i 's/CONFIG_ESPTOOLPY_FLASHSIZE="2MB"/CONFIG_ESPTOOLPY_FLASHSIZE="8MB"/' sdkconfig

echo "4. 恢复分区表配置..."
sed -i 's/CONFIG_PARTITION_TABLE_SINGLE_APP=y/# CONFIG_PARTITION_TABLE_SINGLE_APP is not set/' sdkconfig
sed -i 's/# CONFIG_PARTITION_TABLE_CUSTOM is not set/CONFIG_PARTITION_TABLE_CUSTOM=y/' sdkconfig
sed -i 's/CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"/CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"/' sdkconfig

echo "✅ 配置恢复完成！"
echo ""
echo "WiFi SSID: $(grep CONFIG_MY_WIFI_SSID sdkconfig | cut -d'"' -f2)"
echo "Flash大小: $(grep CONFIG_ESPTOOLPY_FLASHSIZE= sdkconfig | cut -d'"' -f2)"
echo "唤醒词: 你好小智"
