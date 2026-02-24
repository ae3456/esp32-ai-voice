#!/bin/bash
# 修复 Flash 大小配置

# 修改 sdkconfig - 将 Flash 大小从 2MB 改为 8MB
sed -i 's/CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y/# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set/' sdkconfig
sed -i 's/# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set/CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y/' sdkconfig
sed -i 's/CONFIG_ESPTOOLPY_FLASHSIZE="2MB"/CONFIG_ESPTOOLPY_FLASHSIZE="8MB"/' sdkconfig

echo "✅ Flash 大小已改为 8MB"
