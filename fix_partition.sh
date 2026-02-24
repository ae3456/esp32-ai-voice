#!/bin/bash
# 修复分区表配置

# 修改 sdkconfig
sed -i 's/CONFIG_PARTITION_TABLE_SINGLE_APP=y/# CONFIG_PARTITION_TABLE_SINGLE_APP is not set/' sdkconfig
sed -i 's/# CONFIG_PARTITION_TABLE_CUSTOM is not set/CONFIG_PARTITION_TABLE_CUSTOM=y/' sdkconfig
sed -i 's/CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"/CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"/' sdkconfig

echo "✅ 分区表配置已修复"
echo "重新编译中..."
idf.py build
