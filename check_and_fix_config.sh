#!/bin/bash

echo "========== 检查并修复配置 =========="
echo ""

# 1. 检查 Partition Table
echo "【1】检查 Partition Table..."
if grep -q "CONFIG_PARTITION_TABLE_CUSTOM=y" sdkconfig; then
    echo "✅ 已启用自定义分区表"
    grep "CONFIG_PARTITION_TABLE_FILENAME" sdkconfig | head -1
else
    echo "❌ 未启用自定义分区表，正在修复..."
    sed -i 's/CONFIG_PARTITION_TABLE_SINGLE_APP=y/# CONFIG_PARTITION_TABLE_SINGLE_APP is not set/' sdkconfig
    sed -i 's/# CONFIG_PARTITION_TABLE_CUSTOM is not set/CONFIG_PARTITION_TABLE_CUSTOM=y/' sdkconfig
    sed -i 's/CONFIG_PARTITION_TABLE_FILENAME=.*/CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"/' sdkconfig
    echo "✅ 已修复"
fi
echo ""

# 2. 检查 ESP PSRAM
echo "【2】检查 ESP PSRAM..."
if grep -q "CONFIG_SPIRAM=y" sdkconfig; then
    echo "✅ 外部 PSRAM 已启用"
else
    echo "❌ 外部 PSRAM 未启用，正在修复..."
    sed -i 's/# CONFIG_SPIRAM is not set/CONFIG_SPIRAM=y/' sdkconfig
    sed -i 's/CONFIG_SPIRAM_MODE_QUAD=y/# CONFIG_SPIRAM_MODE_QUAD is not set/' sdkconfig
    sed -i 's/# CONFIG_SPIRAM_MODE_OCT is not set/CONFIG_SPIRAM_MODE_OCT=y/' sdkconfig
    echo "✅ 已启用外部 PSRAM"
fi
echo ""

# 3. 检查模型数据路径
echo "【3】检查模型数据路径..."
if grep -q "CONFIG_MODEL_IN_FLASH=y" sdkconfig; then
    echo "✅ 模型数据将从 Flash 读取"
else
    echo "❌ 模型数据路径错误，正在修复..."
    sed -i 's/CONFIG_MODEL_IN_SDCARD=y/# CONFIG_MODEL_IN_SDCARD is not set/' sdkconfig
    sed -i 's/# CONFIG_MODEL_IN_FLASH is not set/CONFIG_MODEL_IN_FLASH=y/' sdkconfig
    echo "✅ 已修复为从 Flash 读取"
fi
echo ""

# 4. 检查唤醒词配置
echo "【4】检查唤醒词配置..."
echo "当前唤醒词配置："
grep "CONFIG_SR_WN.*=y" sdkconfig || echo "无"

# 先禁用所有唤醒词
sed -i 's/CONFIG_SR_WN_WN9S_HIEXIN=y/# CONFIG_SR_WN_WN9S_HIEXIN is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9S_HIESP=y/# CONFIG_SR_WN_WN9S_HIESP is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y/# CONFIG_SR_WN_WN9S_NIHAOXIAOZHI is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9S_HIJASON=y/# CONFIG_SR_WN_WN9S_HIJASON is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_HILEXIN=y/# CONFIG_SR_WN_WN9_HILEXIN is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_NIHAOMIAOBAN_TTS2=y/# CONFIG_SR_WN_WN9_NIHAOMIAOBAN_TTS2 is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_XIAOAITONGXUE=y/# CONFIG_SR_WN_WN9_XIAOAITONGXUE is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9L_XIAOAITONGXUE=y/# CONFIG_SR_WN_WN9L_XIAOAITONGXUE is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y/# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9L_NIHAOXIAOZHI_TTS3=y/# CONFIG_SR_WN_WN9L_NIHAOXIAOZHI_TTS3 is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_ALEXA=y/# CONFIG_SR_WN_WN9_ALEXA is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_HIESP=y/# CONFIG_SR_WN_WN9_HIESP is not set/' sdkconfig
sed -i 's/CONFIG_SR_WN_WN9_JARVIS_TTS=y/# CONFIG_SR_WN_WN9_JARVIS_TTS is not set/' sdkconfig

# 只启用"你好小智"
if grep -q "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y" sdkconfig; then
    echo "✅ 已配置'你好小智'唤醒词"
else
    sed -i 's/# CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS is not set/CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y/' sdkconfig
    echo "✅ 已启用'你好小智'唤醒词"
fi

echo ""
echo "修复后的唤醒词配置："
grep "CONFIG_SR_WN.*=y" sdkconfig || echo "无"

echo ""
echo "========== 配置检查完成 =========="
