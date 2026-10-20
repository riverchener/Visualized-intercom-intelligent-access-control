#!/bin/sh
#=============================================================================
# board_selfcheck.sh —— 板端自检（把这份脚本推到板子上跑）
#   一次性打印：系统/存储/屏幕/摄像头/音频/网络/门口机链路状态
#=============================================================================
echo "===== 1. 系统 ====="
uname -a
cat /proc/device-tree/model; echo
echo "CPU 个数: $(grep -c ^processor /proc/cpuinfo)"

echo "===== 2. 存储 ====="
df -h / /userdata

echo "===== 3. 屏幕 / 触摸 ====="
echo -n "DSI 状态: "; cat /sys/class/drm/card0-DSI-1/status
echo -n "fb0 模式: "; cat /sys/class/graphics/fb0/modes 2>/dev/null
echo -n "触摸: "; cat /proc/bus/input/devices | grep -m1 'Name=' || echo "无"
echo -n "weston: "; ps | grep -c "[w]eston"

echo "===== 4. 摄像头 ====="
ls /dev/video* 2>/dev/null || echo "  未发现 /dev/video*（摄像头没插/没 modprobe uvcvideo）"
lsmod | grep -q uvcvideo && echo "  uvcvideo 已加载" || echo "  uvcvideo 未加载（modprobe uvcvideo）"
echo -n "  谁在占用: "
for p in /proc/[0-9]*; do
  ls -l "$p/fd" 2>/dev/null | grep -q video0 && echo -n "PID ${p#/proc/}($(cat "$p/comm" 2>/dev/null)) "
done; echo

echo "===== 5. 音频（对讲：采集 + 播放）====="
cat /proc/asound/cards
echo "  playback 设备:"; aplay -l 2>/dev/null | grep -E '^card|device'
echo "  capture  hw_params:"; cat /proc/asound/card0/pcm0c/sub0/hw_params 2>/dev/null | head -5
echo "  playback hw_params:"; cat /proc/asound/card0/pcm0p/sub0/hw_params 2>/dev/null | head -5

echo "===== 6. 网络 ====="
ip -brief a
cat /etc/resolv.conf 2>/dev/null | head -2

echo "===== 7. 门口机（推流 + 对讲）====="
echo -n "  av_stream: "; pidof av_stream || echo "未运行"
echo -n "  Qt 终端  : "; pidof intelligent_community || echo "未运行"
echo -n "  :8081/door -> "; wget -q -O - http://127.0.0.1:8081/door 2>/dev/null || echo "无响应"
echo -n "  :8081/snap -> "; wget -q -O /dev/null http://127.0.0.1:8081/snap.jpg 2>/dev/null && echo OK || echo FAIL
echo "  自启脚本: $(ls /etc/init.d/S98av_stream /etc/init.d/S99smart_community /etc/init.d/S99wd_guard 2>/dev/null | tr '\n' ' ')"
echo "  守护状态:"; sed -n '1,12p' /userdata/wd_guard.state 2>/dev/null
echo "  av_stream 日志尾部:"; tail -4 /userdata/av_stream.log 2>/dev/null | tr -d '\000'
echo "  app 日志尾部:"; tail -4 /userdata/smart_community/app.log 2>/dev/null
