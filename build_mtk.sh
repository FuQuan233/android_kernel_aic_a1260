#!/bin/bash

echo -e "\n[INFO]: 开始编译 MTK 内核...\n"

# export KERNEL_ROOT="$(pwd)"
export ARCH=arm
export CROSS_COMPILE=/home/fuquan/buildtools/gcc-linaro-6.3.1-2017.02-x86_64_arm-eabi/bin/arm-eabi-

# 创建输出目录
# mkdir -p "${KERNEL_ROOT}/out"

# 编译选项
make wt6580_we_n_defconfig
make -j$(nproc)

if [ -f out/arch/arm/boot/zImage ]; then
    echo -e "\n[INFO]: 编译成功！内核文件位于 out/arch/arm/boot/zImage\n"
else
    echo -e "\n[ERROR]: 编译失败！\n"
    exit 1
fi 