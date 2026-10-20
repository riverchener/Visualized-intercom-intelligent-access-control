#!/bin/bash
#=============================================================================
# build_device.sh —— 在 Ubuntu 虚拟机里交叉编译设备端 av_stream
#=============================================================================
set -e
SDK=${SDK:-$HOME/atk_dlrk3506_linux6.1_release_v1.3.1_20260326}
SRC=${SRC:-$HOME/video_surveillance/device}
OUT=${OUT:-$HOME/video_surveillance/build}

mkdir -p "$OUT"
cd "$SRC"
make clean >/dev/null 2>&1 || true
make SDK=$SDK

cp -f av_stream "$OUT/av_stream"
echo "=== 结果 ==="
file "$OUT/av_stream"
ls -l "$OUT/av_stream"
