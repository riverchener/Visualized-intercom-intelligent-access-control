#!/bin/bash
#=============================================================================
# install_av_on_board.sh —— 把监控推流装成"开机自启"
#   1) 推送 av_stream / 启动脚本 / init 脚本到板子
#   2) 安装 /etc/init.d/S98av_stream 并立刻启动
#   3) 打印状态与中转服务上的设备列表
#=============================================================================
set -e
ADB=${ADB:-$HOME/platform-tools/adb}
VMIP=${VMIP:-192.168.137.94}
R() { echo "----- $* -----"; timeout 60 $ADB shell "$*" 2>&1; }

echo "=== 推送文件 ==="
timeout 60 $ADB push ~/video_surveillance/build/av_stream        /userdata/av_stream
timeout 60 $ADB push ~/video_surveillance/device/av_stream_start.sh /userdata/av_stream_start.sh
timeout 60 $ADB push ~/video_surveillance/device/S98av_stream    /userdata/S98av_stream
R "chmod 755 /userdata/av_stream /userdata/av_stream_start.sh /userdata/S98av_stream"

echo "=== 安装开机自启 ==="
R "cp /userdata/S98av_stream /etc/init.d/S98av_stream && chmod 755 /etc/init.d/S98av_stream && sync && echo installed"
# 中转服务地址与档位写进 /etc/default/av_stream（以后改配置只改这个文件）
R "mkdir -p /etc/default && printf 'SERVER=%s\nDEVICE_ID=rk3506b-01\nSIZE=1920x1080\nFPS=15\n# CTRL=sharpness=3,contrast=10\n# AUDIO=hw:0,0\n' $VMIP:9100 > /etc/default/av_stream && cat /etc/default/av_stream"

echo "=== 启动推流 ==="
R "/etc/init.d/S98av_stream restart"
sleep 10
R "/etc/init.d/S98av_stream status"

echo "=== 中转服务上的设备列表 ==="
cd ~/video_surveillance || exit 1
python3 scripts/inspect_state.py 2>&1 | sed -n '10,14p'
