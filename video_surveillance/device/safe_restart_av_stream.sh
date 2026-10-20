#!/bin/sh
# ---------------------------------------------------------------------------
# /userdata/safe_restart_av_stream.sh
#
# 安全重启 av_stream：先挂起 wd_guard 的自动重启，避免"手动 restart 与守护脚本
# 抢跑"造成两个 av_stream 实例（第二个实例会一直报 摄像头被其它进程占用，
# 实测还把 CPU 顶到 90%）。
#
# ★ 2026-10-20 按需推流版：
#   * 重启意味着"要推流"，所以顺手清掉 /tmp/av_stream.intentional_stop 标记，
#     否则 wd_guard 会认为这次重启是"正常停止"而不再兜底；
#   * 用 FORCE=1 调 S98av_stream，绕开"开机阶段不自动推流"的保护。
#
# 用法：/userdata/safe_restart_av_stream.sh
# ---------------------------------------------------------------------------
set -u
INIT=/etc/init.d/S98av_stream
PAUSE=/userdata/wd_guard.pause
MARK=/tmp/av_stream.intentional_stop
LOG=/userdata/av_stream_switch.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

touch "$PAUSE"
rm -f "$MARK"
killall av_stream 2>/dev/null
i=0
while [ $i -lt 20 ]; do
    pidof av_stream >/dev/null 2>&1 || break
    sleep 0.5
    i=$((i + 1))
done
if pidof av_stream >/dev/null 2>&1; then
    killall -9 av_stream 2>/dev/null
    sleep 1
fi

FORCE=1 $INIT start
sleep 6
rm -f "$PAUSE"

N=$(pidof av_stream 2>/dev/null | wc -w)
log "safe_restart 完成：实例数=$N pid=$(pidof av_stream 2>/dev/null)"
echo "av_stream 实例数=$N  pid=$(pidof av_stream 2>/dev/null)"
if [ "$N" != "1" ]; then
    echo "⚠ 实例数不为 1，请手工检查（ps | grep av_stream）"
    exit 1
fi
echo "OK：单实例运行，USB 音频模块加载日志见 /userdata/modules/load_usb_audio.log"
