#!/bin/bash
#=============================================================================
# board_setup_display.sh —— 在**开发板上**执行的显示环境配置脚本
#   （由 PC/虚拟机 push 到板子上，再用 adb shell 执行）
#
# 做四件事：
#   1) 备份原始配置到 /userdata/backup/
#   2) 打开 weston 的 --debug，让 weston-screenshooter 能截屏（官方手册 19）
#   3) 给 MIPI 屏加横屏 transform=rotate-90（官方手册 25）
#      板子原生是 720x1280 竖屏，本项目 UI 按 1280x720 横屏适配
#   4) 把 weston 桌面背景从"原厂 iphone.jpg"换成我们自己的启动背景
#      —— 开机时 Qt 程序还要几秒才起来，这几秒会露出桌面背景，
#         换成自己的图，避免"先闪一下原厂 UI"的观感问题
#
# 用法： sh /userdata/smart_community/board_setup_display.sh
#        sh /userdata/smart_community/board_setup_display.sh restore   # 还原
#=============================================================================
set -e
WESTON_INI=/etc/xdg/weston/weston.ini
WESTON_INIT=/etc/init.d/S49weston
DESKTOP_INI=/etc/xdg/weston/weston.ini.d/02-desktop.ini
BAK=/userdata/backup
APP_DIR=/userdata/smart_community
SPLASH=$APP_DIR/splash.png
ROTATE=${ROTATE:-rotate-90}

mkdir -p $BAK
[ -f $BAK/weston.ini.orig ]       || cp $WESTON_INI   $BAK/weston.ini.orig
[ -f $BAK/S49weston.orig ]        || cp $WESTON_INIT  $BAK/S49weston.orig
[ -f $BAK/02-desktop.ini.orig ]   || cp $DESKTOP_INI  $BAK/02-desktop.ini.orig

if [ "$1" = "restore" ]; then
    cp $BAK/weston.ini.orig     $WESTON_INI
    cp $BAK/S49weston.orig      $WESTON_INIT
    cp $BAK/02-desktop.ini.orig $DESKTOP_INI
    sync
    echo "已还原 weston 配置（含原厂背景图）"
    exit 0
fi

# 1) --debug（截屏功能）
if ! grep -q 'weston --debug' $WESTON_INIT; then
    sed -i 's#/usr/bin/weston 2>&1#/usr/bin/weston --debug 2>\&1#' $WESTON_INIT
    echo "已给 weston 加上 --debug"
else
    echo "weston --debug 已存在"
fi

# 2) 横屏
if grep -q '^\[output\]' $WESTON_INI; then
    if ! grep -q "$ROTATE" $WESTON_INI; then
        sed -i "/^\[output\]/a name=DSI-1\ntransform=$ROTATE" $WESTON_INI
        echo "已在已有 [output] 段加入 transform=$ROTATE"
    else
        echo "旋转配置已存在"
    fi
else
    cat >> $WESTON_INI <<EOF

[output]
name=DSI-1
transform=$ROTATE
panel-position=none
EOF
    echo "已追加 [output] transform=$ROTATE"
fi

# 3) 换掉原厂开机背景
if [ -f "$SPLASH" ]; then
    if grep -q '^background-image=' $DESKTOP_INI 2>/dev/null; then
        sed -i "s#^background-image=.*#background-image=$SPLASH#" $DESKTOP_INI
    else
        # 没有这一行就插到 [shell] 段后面
        sed -i "/^\[shell\]/a background-image=$SPLASH" $DESKTOP_INI
    fi
    # 兜底：万一图片加载失败，桌面用同色纯色背景，也不会突兀
    grep -q '^background-color=' $DESKTOP_INI || \
        sed -i "/^\[shell\]/a background-color=0xfff5f5dc" $DESKTOP_INI
    echo "已把开机背景换成 $SPLASH"
else
    echo "警告：没找到 $SPLASH，跳过背景替换"
fi

sync
echo "=== weston.ini 尾部 ==="
tail -6 $WESTON_INI
echo "=== 02-desktop.ini ==="
cat $DESKTOP_INI
echo "=== 当前 weston 启动行 ==="
grep weston $WESTON_INIT | tail -2
