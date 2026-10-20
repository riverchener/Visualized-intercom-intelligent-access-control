#!/bin/bash
#=============================================================================
# build_app.sh —— 在 Ubuntu 虚拟机里交叉编译「智慧社区」Qt 程序
#
# 用法:
#   bash build_app.sh              # 默认路径（~/smart_community）
#   SDK=/path/to/sdk bash build_app.sh
#
# 说明:
#   * 用的是 RK3506 SDK 里 buildroot 生成的 host 版 qmake（它自带 sysroot，
#     所以不需要再写 -spec / CROSS_COMPILE）。
#   * 采用"影子编译"（源码目录保持干净），产物在 build/intelligent_community。
#=============================================================================
set -e

SDK=${SDK:-$HOME/atk_dlrk3506_linux6.1_release_v1.3.1_20260326}
HOSTDIR=$SDK/buildroot/output/alientek_rk3506/host
QMAKE=$HOSTDIR/bin/qmake

PROJ_DIR=${PROJ_DIR:-$HOME/smart_community/app}
BUILD_DIR=${BUILD_DIR:-$HOME/smart_community/build}

if [ ! -x "$QMAKE" ]; then
    echo "找不到 qmake: $QMAKE"
    echo "请先编译 SDK 的 buildroot，或用 SDK=... 指定正确路径"
    exit 1
fi

export PATH=$HOSTDIR/bin:$PATH
# host 工具链里的头文件/库都在 output/.../host 下，qmake 已内建，无需额外设置
unset CROSS_COMPILE   # 防止外部环境残留干扰

echo "=== qmake 版本 ==="
$QMAKE -v
echo "=== 源码目录: $PROJ_DIR ==="
echo "=== 编译目录: $BUILD_DIR ==="

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

$QMAKE "$PROJ_DIR/intelligent_community.pro"
make -j"$(nproc)" > build.log 2>&1 || {
    echo "=========== 编译失败，错误摘要 ==========="
    grep -nE "错误|error:|Error [0-9]" build.log | tail -40
    echo "=========== 末尾 30 行 ==========="
    tail -30 build.log
    exit 1
}
echo "=========== 警告统计 ==========="
grep -c "警告" build.log || true
echo "=========== 编译结果 ==========="
file ./intelligent_community
ls -l ./intelligent_community
"$HOSTDIR/bin/arm-buildroot-linux-gnueabihf-strip" ./intelligent_community 2>/dev/null || true
ls -l ./intelligent_community
echo "OK"
