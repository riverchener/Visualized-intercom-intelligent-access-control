#!/bin/bash
#=============================================================================
# board_install.sh —— 在开发板上执行的"一键安装/卸载"脚本
#   由 PC/虚拟机 push 到 /userdata/smart_community 后执行：
#       sh /userdata/smart_community/board_install.sh install
#       sh /userdata/smart_community/board_install.sh uninstall
#       sh /userdata/smart_community/board_install.sh status
#
# install 做的事：
#   1) 安装开机自启脚本 /etc/init.d/S99smart_community（启动 Qt 应用）
#   2) 安装 WiFi 自动连接 + NTP 校时 /etc/init.d/S47wifi_smart_community
#   3) 立即启动程序
#
# 注：本脚本不涉及内核模块。门禁业务不驱动板载 GPIO 灯，
#     USB 音频所需的 snd-usb-audio.ko 等模块见 usbaudio_modules/ 与
#     /userdata/modules/load_usb_audio.sh。
#=============================================================================

APP_DIR=/userdata/smart_community
INIT=/etc/init.d/S99smart_community
WIFI_INIT=/etc/init.d/S47wifi_smart_community

case "$1" in
install)
    echo "== 1. 安装开机自启脚本 =="
    if [ -f $APP_DIR/S99smart_community ]; then
        cp $APP_DIR/S99smart_community $INIT
        chmod 755 $INIT
        sync
        echo "   已安装 $INIT"
    else
        echo "   未找到 S99smart_community"
    fi

    echo "== 2. 安装 WiFi 自动连接 + NTP 校时脚本 =="
    chmod 755 $APP_DIR/wifi_connect.sh $APP_DIR/timesync.sh 2>/dev/null
    if [ -f $APP_DIR/S47wifi_smart_community ]; then
        cp $APP_DIR/S47wifi_smart_community $WIFI_INIT
        chmod 755 $WIFI_INIT
        sync
        echo "   已安装 $WIFI_INIT（开机自动连 WiFi + 校时）"
        # 立刻连一次，免得到下次重启才生效
        sh $APP_DIR/wifi_connect.sh >/dev/null 2>&1 &
    else
        echo "   未找到 S47wifi_smart_community"
    fi

    echo "== 3. 启动程序 =="
    $INIT restart
    sleep 3
    ps | grep intelligent_community | grep -v grep
    ;;

uninstall)
    echo "== 停止并移除自启 =="
    $INIT stop 2>/dev/null
    rm -f $INIT
    rm -f $WIFI_INIT
    sync
    echo "已卸载（/userdata/smart_community 里的文件保留）"
    ;;

status)
    echo "== 进程 =="; ps | grep intelligent_community | grep -v grep
    echo "== 自启脚本 =="; ls -l $INIT
    echo "== 日志 =="; tail -20 $APP_DIR/app.log
    ;;

*)
    echo "用法: $0 {install|uninstall|status}"
    exit 1
    ;;
esac
