#!/bin/sh
#=============================================================================
# av_stream_on.sh —— 设备端「按需推流」：开始推流（只有通话被接听后才调用）
#
# 调用者：
#   * Qt 应用（/userdata/smart_community/intelligent_community）
#     轮询 /api/callstate/<dev> 变成 accepted 时 QProcess::startDetached 本脚本
#   * /userdata/smart_community/wd_guard.sh（accepted 且 av_stream 不在时的抢救）
#
# 做的事：
#   1) 抢操作锁（避免 Qt 与 wd_guard 同时启动 → 双实例抢 /dev/video0）
#   2) 清掉 /tmp/av_stream.intentional_stop（"这是正常停止，不要拉起"的标记）
#   3) touch /userdata/wd_guard.pause（启动期间挂起守护，防止它抢跑）
#   4) FORCE=1 /etc/init.d/S98av_stream start（参数与开机脚本完全一致）
#   5) 校验单实例，去掉 pause
#
# 手动：/userdata/av_stream_on.sh
#=============================================================================
PATH="/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin"
MARK=/tmp/av_stream.intentional_stop
PAUSE=/userdata/wd_guard.pause
LOCK=/tmp/av_stream.oplock
LOG=/userdata/av_stream_switch.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# --- 操作锁（原子 mkdir；15 秒拿不到就强拆，避免上次异常退出留下的死锁）---
i=0
while ! mkdir "$LOCK" 2>/dev/null; do
    i=$((i + 1))
    if [ $i -ge 30 ]; then
        log "操作锁等待超时，强制清理 $LOCK"
        rm -rf "$LOCK"
        mkdir "$LOCK" 2>/dev/null
        break
    fi
    sleep 0.5
done
trap 'rm -rf "$LOCK"' EXIT INT TERM

rm -f "$MARK"                       # 这次是"要推流"，清掉正常停止标记

if pidof av_stream >/dev/null 2>&1; then
    log "av_stream 已在运行（pid=$(pidof av_stream)），无需重复启动"
    echo "av_stream 已在运行 pid=$(pidof av_stream)"
    exit 0
fi

touch "$PAUSE"
log "开始按需推流：FORCE=1 S98av_stream start"
FORCE=1 /etc/init.d/S98av_stream start >> "$LOG" 2>&1

#-----------------------------------------------------------------------------
# ★ 2026-10-20 关键优化（"挂断→停流"延迟，实测踩到）：
#   S98 start 返回时 av_stream 已经起来了，到这里"起流"这件事就完了 ——
#   **必须立刻释放操作锁**。原来这把锁一直拿到脚本结束（后面还有 sleep 3 的
#   稳定期 + 校验），实测持锁 6~7 秒，于是"接听后 2~3 秒就挂断"时
#   av_stream_off.sh 只能干等锁：挂断→停流被测出 3.20s（设计要求 ≤2s，
#   稳态是 0.9~1.0s）。释放锁之后，off.sh 最多只等 S98 start 那 ~2 秒。
#   安全性：释放后本脚本只剩"等稳定 + 打印校验"，不会再去动 av_stream，
#   所以 off.sh 随时可以进来把它停掉（停流永远是赢家）。
#-----------------------------------------------------------------------------
trap - EXIT INT TERM
rm -rf "$LOCK"

sleep 3
rm -f "$PAUSE"

N=$(pidof av_stream 2>/dev/null | wc -w)
log "启动完成：实例数=$N pid=$(pidof av_stream 2>/dev/null)"
echo "av_stream 实例数=$N pid=$(pidof av_stream 2>/dev/null)"
[ "$N" = "1" ] || exit 1
exit 0
