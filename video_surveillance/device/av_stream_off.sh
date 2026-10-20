#!/bin/sh
#=============================================================================
# av_stream_off.sh —— 设备端「按需推流」：停止推流（挂断 / 离开对讲页时调用）
#
# 做的事：
#   1) touch /tmp/av_stream.intentional_stop
#      —— 这是**正常停止**的标记：wd_guard 看到它就不会把 av_stream 当"挂了"重启
#         （/tmp 是 tmpfs，重启板子自动消失）
#   2) 抢操作锁 + /etc/init.d/S98av_stream stop（等进程真正退出，释放摄像头/声卡）
#   3) 打印结果（实例数应为 0）
#
# 手动：/userdata/av_stream_off.sh
#=============================================================================
PATH="/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin"
MARK=/tmp/av_stream.intentional_stop
LOCK=/tmp/av_stream.oplock
LOG=/userdata/av_stream_switch.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# --- 操作锁（与 av_stream_on.sh 同一把锁）---
#   ★ 2026-10-20：等待粒度从 0.5s 改成 0.2s，并把等待时长打进日志 ——
#     on.sh 正常只持锁 ~2 秒（它已经在 S98 start 返回后立刻释放），这里最多等
#     6 秒（30×0.2s）就强拆，避免"接听后马上挂断"时把停流拖到十几秒。
i=0
while ! mkdir "$LOCK" 2>/dev/null; do
    i=$((i + 1))
    if [ $i -ge 30 ]; then
        log "操作锁等待超时（6s），强制清理 $LOCK"
        rm -rf "$LOCK"
        mkdir "$LOCK" 2>/dev/null
        break
    fi
    sleep 0.2
done
trap 'rm -rf "$LOCK"' EXIT INT TERM
[ $i -gt 0 ] && log "等操作锁 ${i}×0.2s 才拿到（起流脚本还在跑）"

touch "$MARK"
log "按需停止推流：S98av_stream stop（已写 intentional_stop 标记）"
/etc/init.d/S98av_stream stop >> "$LOG" 2>&1

N=$(pidof av_stream 2>/dev/null | wc -w)
log "停止完成：实例数=$N"
echo "av_stream 实例数=$N"
[ "$N" = "0" ] || exit 1
exit 0
