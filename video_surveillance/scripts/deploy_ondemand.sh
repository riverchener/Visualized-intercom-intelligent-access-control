#!/bin/bash
#=============================================================================
# deploy_ondemand.sh —— 把「按需推流」版本部署到板子（在 VM 上跑，用 adb）
#
#   1) 停 Qt 应用与 av_stream（停 av_stream 用 off.sh：会写 intentional_stop 标记，
#      避免 wd_guard 立刻又把它拉起来）
#   2) 推：av_stream 二进制 / av_stream_on.sh / av_stream_off.sh /
#          safe_restart_av_stream.sh / S98av_stream / wd_guard.sh / Qt 应用
#   3) 安装 S98 到 /etc/init.d（并把 /etc/init.d 下的 *.bak_* 挪走 ——
#      它们以 S98 开头，开机 rcS 会**一起执行**，会在开机时偷偷起第二个 av_stream）
#   4) 重启 Qt 应用 + wd_guard，打印状态
#
# 用法：bash ~/video_surveillance/scripts/deploy_ondemand.sh
#=============================================================================
set -u
ADB=${ADB:-$HOME/platform-tools/adb}
VD=${VD:-$HOME/video_surveillance}
SC=${SC:-$HOME/smart_community}
D=/userdata
S=$D/smart_community

R() { timeout 120 $ADB shell "$*" 2>&1; }
P() { timeout 180 $ADB push "$1" "$2" 2>&1 | tail -1; }

echo "########## 1. 停旧进程 ##########"
R "killall intelligent_community 2>/dev/null; sleep 1"
# 先把 off 脚本推上去（旧板子上没有），再停流 —— 这样会留下 intentional_stop 标记
P "$VD/device/av_stream_off.sh" $D/av_stream_off.sh >/dev/null
R "chmod 755 $D/av_stream_off.sh; $D/av_stream_off.sh"

echo "########## 2. 推送新版本 ##########"
P "$VD/build/av_stream" $D/av_stream
P "$VD/device/av_stream_on.sh" $D/av_stream_on.sh
P "$VD/device/av_stream_off.sh" $D/av_stream_off.sh
P "$VD/device/safe_restart_av_stream.sh" $D/safe_restart_av_stream.sh
P "$VD/device/S98av_stream" $D/S98av_stream
P "$VD/device/wd_guard.sh" $S/wd_guard.sh
P "$SC/build/intelligent_community" $S/intelligent_community

echo "########## 3. 安装 ##########"
R "chmod 755 $D/av_stream $D/av_stream_on.sh $D/av_stream_off.sh $D/safe_restart_av_stream.sh $D/S98av_stream $S/wd_guard.sh $S/intelligent_community"
R "cp -f $D/S98av_stream /etc/init.d/S98av_stream && chmod 755 /etc/init.d/S98av_stream && echo S98_installed"
# ★ /etc/init.d 下的 *.bak_* 也匹配 rcS 的 'S??*'，必须挪出该目录
R "mkdir -p /userdata/backup_initd; for f in /etc/init.d/*.bak_*; do [ -e \"\$f\" ] && mv -f \"\$f\" /userdata/backup_initd/ && echo moved \$f; done; ls /etc/init.d/ | grep -E 'S9[89]'"

echo "########## 4. 重启应用与守护 ##########"
R "/etc/init.d/S99smart_community restart"
R "/etc/init.d/S99wd_guard restart"
sleep 3

echo "########## 5. 状态 ##########"
R "echo -n 'av_stream: '; pidof av_stream || echo '(未运行=按需推流正常)'; echo -n 'qt: '; pidof intelligent_community; echo -n 'wd_guard: '; cat /var/run/wd_guard.pid 2>/dev/null; ls -l /tmp/av_stream.intentional_stop 2>/dev/null"
R "cat /userdata/wd_guard.state 2>/dev/null"
R "echo '--- av_stream_switch.log ---'; tail -6 /userdata/av_stream_switch.log 2>/dev/null"
