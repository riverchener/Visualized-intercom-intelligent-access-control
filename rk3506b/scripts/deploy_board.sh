#!/bin/bash
#=============================================================================
# deploy_board.sh —— 在 Ubuntu 虚拟机里把编好的程序推到 RK3506B 并运行
#   （虚拟机已通过 USB 连接板子的 ADB 口；adb 在 ~/platform-tools/adb）
#
# 用法:
#   bash deploy_board.sh            # 推送 + 重启程序 + 截屏
#   RUN=0 bash deploy_board.sh      # 只推送不运行
#   SHOT=0 bash deploy_board.sh     # 不截屏
#=============================================================================
set -e
ADB=${ADB:-$HOME/platform-tools/adb}
BIN=${BIN:-$HOME/smart_community/build/intelligent_community}
DIR=/userdata/smart_community
STOP_UI=${STOP_UI:-1}

echo "=== 设备列表 ==="
$ADB devices -l

echo "=== 建立目录 ==="
$ADB shell "mkdir -p $DIR"

echo "=== 推送程序 ($(stat -c%s "$BIN") 字节) ==="
$ADB push "$BIN" $DIR/intelligent_community
$ADB shell "chmod 755 $DIR/intelligent_community"

if [ "${RUN:-1}" = "1" ]; then
    echo "=== 停掉出厂 UI（避免两个窗口抢屏幕）==="
    if [ "$STOP_UI" = "1" ]; then
        $ADB shell "/etc/init.d/S50systemui stop" || true
        sleep 1
    fi

    echo "=== 启动智慧社区 ==="
    # 注意：adb shell 里如果有后台子进程，adb 会一直等它的 stdout 关闭，
    #       所以这里用 timeout 兜底，并让子进程彻底脱离终端（setsid + 重定向）。
    timeout 15 $ADB shell "killall intelligent_community 2>/dev/null; sleep 1"
    timeout 15 $ADB shell "cd $DIR && XDG_RUNTIME_DIR=/var/run QT_QPA_PLATFORM=wayland \
        setsid ./intelligent_community > $DIR/app.log 2>&1 < /dev/null &" || true
    sleep 8

    echo "=== 进程状态 ==="
    $ADB shell "ps | grep -E 'intelligent_community|weston' | grep -v grep" || true
    echo "=== 运行日志 ==="
    $ADB shell "cat $DIR/app.log" || true

    if [ "${SHOT:-1}" = "1" ]; then
        echo "=== 截屏 ==="
        $ADB shell "XDG_RUNTIME_DIR=/var/run weston-screenshooter $DIR/shot.png" || true
        $ADB pull $DIR/shot.png /tmp/shot.png || true
        ls -l /tmp/shot.png 2>/dev/null || true
    fi
fi
echo "=== DONE ==="
