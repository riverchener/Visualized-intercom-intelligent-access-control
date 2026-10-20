#!/bin/bash
#=============================================================================
# install_on_board.sh —— 在虚拟机里执行：把程序+自启脚本一起装到板子上
#   用法: bash install_on_board.sh
#
#   注：本脚本不安装内核模块。门禁业务不驱动板载 GPIO 灯；
#       USB 音频所需的 snd-usb-audio.ko 等见 usbaudio_modules/。
#=============================================================================
set -e
ADB=${ADB:-$HOME/platform-tools/adb}
BIN=${BIN:-$HOME/smart_community/build/intelligent_community}
BOARD_DIR=${BOARD_DIR:-$HOME/smart_community/board}
DIR=/userdata/smart_community

echo "=== 设备 ==="
$ADB devices -l
$ADB shell "mkdir -p $DIR"

echo "=== 推送文件 ==="
$ADB push "$BIN" $DIR/intelligent_community
$ADB push "$BOARD_DIR/S99smart_community" $DIR/S99smart_community
$ADB push "$BOARD_DIR/S47wifi_smart_community" $DIR/S47wifi_smart_community
$ADB push "$BOARD_DIR/wifi_connect.sh" $DIR/wifi_connect.sh
$ADB push "$BOARD_DIR/timesync.sh" $DIR/timesync.sh
$ADB push "$BOARD_DIR/board_install.sh" $DIR/board_install.sh
[ -f "$BOARD_DIR/splash.png" ] && $ADB push "$BOARD_DIR/splash.png" $DIR/splash.png
$ADB shell "chmod 755 $DIR/intelligent_community $DIR/board_install.sh $DIR/S99smart_community \
            $DIR/S47wifi_smart_community $DIR/wifi_connect.sh $DIR/timesync.sh"

echo "=== 板端执行安装 ==="
timeout 120 $ADB shell "sh $DIR/board_install.sh install"
echo "=== DONE ==="
