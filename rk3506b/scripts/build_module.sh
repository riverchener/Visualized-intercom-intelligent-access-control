#!/bin/bash
#=============================================================================
# build_module.sh —— 在 Ubuntu 虚拟机里交叉编译外部内核模块
#
# 本项目当前需要编译的内核模块只有 USB 音频相关（见 usbaudio_modules/）。
# 门禁业务本身不包含自制字符设备驱动。
#
# 关键点：外部模块必须用"和板子上运行的内核完全一致的配置"编译，
#         否则 insmod 会报 version magic 不匹配。
#         这里直接用 SDK 里的 kernel 源码树（它已经 ./build.sh kernel 编译过，
#         有 Module.symvers / .config）。
#
# 用法:
#   bash build_module.sh
#   SDK=... bash build_module.sh
#=============================================================================
set -e

SDK=${SDK:-$HOME/atk_dlrk3506_linux6.1_release_v1.3.1_20260326}
KDIR=$SDK/kernel
MODDIR=${MODDIR:-$HOME/smart_community/driver}
CROSS=$SDK/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-

echo "=== 检查内核树 ==="
if [ ! -f "$KDIR/.config" ]; then
    echo "$KDIR/.config 不存在，请先执行: cd $SDK && ./build.sh kernel"
    exit 1
fi
grep -E '^CONFIG_MODULES=|^CONFIG_MODVERSIONS=|^CONFIG_MODULE_SIG=|^CONFIG_LOCALVERSION' "$KDIR/.config" || true
echo "内核版本(include/config/kernel.release): $(cat $KDIR/include/config/kernel.release 2>/dev/null || echo '?')"

echo "=== 编译外部模块 ==="
cd "$MODDIR"
# 说明：不要在这里跑 `make kernelrelease`，未配置完整时它会交互式提问导致卡住
make -C "$KDIR" M="$MODDIR" ARCH=arm CROSS_COMPILE="$CROSS" modules </dev/null 2>&1 | tail -25

echo "=== 结果 ==="
ls -l *.ko 2>/dev/null || echo "未生成 .ko（检查 MODDIR 下的 Makefile 是否有 obj-m 条目）"
for ko in *.ko; do
    [ -f "$ko" ] && "$CROSS"objdump -s -j .modinfo "$ko" 2>/dev/null | head -8
done
echo OK
