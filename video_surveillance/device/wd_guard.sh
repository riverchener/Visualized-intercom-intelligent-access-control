#!/bin/sh
#=============================================================================
# wd_guard.sh —— 板端「硬件看门狗喂狗 + 进程守护」（★ 2026-10-20 按需推流版 v3）
#
#   ★ 背景：2026-10-20 起门禁改成"按需推流"——
#       平时 av_stream 是**正常停止**的（省带宽/省 CPU/保护隐私），只有通话
#       被住户接听后才会被 /userdata/av_stream_on.sh 拉起来，挂断即停。
#
#   ★★ v3 重要修正（2026-10-20，用户复验发现严重 bug）：
#       旧版有一条"恢复兜底"：`accepted 持续 ≥10s 且 av_stream 不在 → 执行
#       av_stream_on.sh`。实测在**挂断瞬间**它会拿到"过期/边缘"的 accepted
#       状态，于是在已经挂断之后又把流拉起来（实测：挂断后第 5 秒 av_stream
#       被重新拉起 pid=1436，30 秒内出现 1 次推流），把"挂断→真正停流"从
#       0.6~1.2 秒拖到 11.19 秒，还白推了 20 秒视频（隐私事故）。
#       → 本版**彻底删除这条路径**。设计上"启动推流"只由板端 Qt 应用
#         （CallClient 轮询到 accepted）触发，守护脚本没有任何理由去起流。
#
#   本版职责（只有三件事）：
#       1) **漏停兜底**：av_stream 在跑、但通话状态连续 2 次(≈10s) 是 idle
#          → 判定"漏停"（板端漏收挂断事件），执行 /userdata/av_stream_off.sh
#       2) **Qt 应用守护**：intelligent_community 是常驻程序，不在就重启
#          （二次确认 + 人工启停标记，见下）
#       3) 硬件看门狗喂狗（本板实测**没有** /dev/watchdog，自动降级为纯软件守护）
#
#   明确**不做**的事：绝不主动启动 av_stream（那条"恢复兜底"已删除）。
#
#   标记文件：
#     /tmp/av_stream.intentional_stop   av_stream_off.sh 写的"正常停止"标记
#                                       （只用于日志/取证，本版已不靠它决定重启）
#     /userdata/wd_guard.pause          临时挂起自动重启（调试/维护用）
#     /tmp/ic.managed_stop              S99smart_community start|restart 期间写的
#                                       "人工启停中"标记（含 epoch 秒；超过 60 秒
#                                       视为过期，避免脚本被 kill 后永久不重启）
#
#   手动控制：
#     /etc/init.d/S99wd_guard start|stop|restart|status
#     touch /userdata/wd_guard.pause ; rm /userdata/wd_guard.pause
#
#   日志：/userdata/wd_guard.log     状态：/userdata/wd_guard.state
#=============================================================================

PATH="/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin"
LOG=/userdata/wd_guard.log
STATE=/userdata/wd_guard.state
PIDF=/var/run/wd_guard.pid
PAUSE=/userdata/wd_guard.pause
STOP_MARK=/tmp/av_stream.intentional_stop
MANAGED=/tmp/ic.managed_stop

INTERVAL=${INTERVAL:-5}          # 检查周期（秒）
FAIL_LIMIT=${FAIL_LIMIT:-6}      # 连续失败多少次就放弃喂狗（→ 硬件复位）
GRACE=${GRACE:-45}               # 启动宽限（秒）：开机时先别急着重启服务
LOG_MAX=${LOG_MAX:-262144}       # 日志超过 256KB 就轮转
CALL_EVERY=${CALL_EVERY:-2}      # 每 N 个周期（默认 10 秒）查一次中转的通话状态
                                 #   ★ 必须比中转的"接听后 20 秒收尾"更快（2026-10-20
                                 #     踩到：15 秒才发现就晚了）
MANAGED_MAX_AGE=${MANAGED_MAX_AGE:-60}   # 人工启停标记的最长有效期（秒）

# 中转地址：沿用 /etc/default/av_stream 的 SERVER（host:9100 → host:8080）
[ -f /etc/default/av_stream ] && . /etc/default/av_stream
SERVER=${SERVER:-192.168.137.94:9100}
DEVICE_ID=${DEVICE_ID:-rk3506b-01}
RELAY_HOST=${SERVER%%:*}
CALL_URL="http://${RELAY_HOST}:8080/api/callstate/${DEVICE_ID}.txt"

# ---- 找看门狗设备（有就喂，没有就纯软件守护）--------------------------------
WD=""
for c in /dev/watchdog /dev/watchdog0; do
    [ -e "$c" ] && { WD=$c; break; }
done
WD_OPEN=0
WD_FD=9
FEED_STOP=0
FAIL_IC=0
T0=$(date +%s)
LAST_OK_LOG=0
LAST_ROT=0
TICK=0
CALLSTATE="unknown"
IDLE_RUNS=0

log()
{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"
}

log_rotate()
{
    [ -f "$LOG" ] || return 0
    sz=$(wc -c < "$LOG" 2>/dev/null | tr -d ' ')
    [ -z "$sz" ] && return 0
    if [ "$sz" -gt "$LOG_MAX" ]; then
        tail -n 400 "$LOG" > "$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
        log "日志已轮转（超过 ${LOG_MAX} 字节）"
    fi
}

wd_open()
{
    [ -z "$WD" ] && return 1
    [ "$WD_OPEN" = "1" ] && return 0
    if eval "exec $WD_FD>\"$WD\"" 2>/dev/null; then
        WD_OPEN=1
        log "硬件看门狗已打开: $WD（打开即开始计时，之后每 ${INTERVAL}s 喂一次）"
        return 0
    fi
    log "硬件看门狗 $WD 打不开（权限/驱动），只做软件守护"
    WD=""
    return 1
}

wd_feed()
{
    [ -z "$WD" ] && return 1
    [ "$WD_OPEN" = "1" ] || wd_open || return 1
    # 写任意非 'V' 字节 = 喂狗（'V' 是 magic close，会关掉看门狗）
    printf 'k' >&$WD_FD 2>/dev/null || { WD_OPEN=0; return 1; }
    return 0
}

wd_close()
{
    [ "$WD_OPEN" = "1" ] || return 0
    # magic close：写 'V' 再关 fd，尽量让看门狗停下来（nowayout=1 时无效）
    printf 'V' >&$WD_FD 2>/dev/null
    eval "exec $WD_FD>&-" 2>/dev/null
    WD_OPEN=0
    log "硬件看门狗已关闭（magic close）"
}

on_exit()
{
    wd_close
    rm -f "$PIDF"
    log "wd_guard 退出（ic_fail=$FAIL_IC wd=$WD）"
    exit 0
}

# 问中转"这台设备现在是不是通话中"（返回 accepted / ringing / idle / rejected /
# timeout / unknown）。用 .txt 变体，纯文本，busybox 下最好解析。
query_callstate()
{
    r=$(curl -s -m 3 "$CALL_URL" 2>/dev/null | tr -d ' \r\n')
    case "$r" in
        accepted|ringing|idle|rejected|timeout) echo "$r" ;;
        *) echo "unknown" ;;
    esac
}

# 人工启停中？（S99smart_community start|restart 会写 /tmp/ic.managed_stop）
# 返回 0 = 是（别插手），1 = 否
in_managed_stop()
{
    [ -f "$MANAGED" ] || return 1
    mts=$(cat "$MANAGED" 2>/dev/null | tr -d ' \r\n')
    case "$mts" in
        ''|*[!0-9]*) mts=0 ;;
    esac
    age=$(($(date +%s) - mts))
    if [ "$mts" -gt 0 ] && [ "$age" -ge 0 ] && [ "$age" -le "$MANAGED_MAX_AGE" ]; then
        return 0
    fi
    # 过期（脚本被 kill / 手工 touch 了空文件）→ 清掉，别让它永久拦住重启
    log "人工启停标记已过期（age=${age}s > ${MANAGED_MAX_AGE}s），清理 $MANAGED"
    rm -f "$MANAGED"
    return 1
}

#-----------------------------------------------------------------------------
# 启动
#-----------------------------------------------------------------------------
mkdir -p /var/run 2>/dev/null
echo $$ > "$PIDF"
trap on_exit INT TERM
log "===== wd_guard 启动 v3 (pid=$$ 周期=${INTERVAL}s 放弃阈值=${FAIL_LIMIT}次 宽限=${GRACE}s) ====="
if [ -n "$WD" ]; then
    log "探测到硬件看门狗设备: $WD（会在 Qt 应用正常时喂狗）"
else
    log "未探测到 /dev/watchdog：本板无硬件看门狗，自动降级为【纯软件进程守护】"
fi
log "职责: ①漏停兜底(通话 idle 但 av_stream 还在 → 停) ②Qt 应用守护 ③喂狗"
log "★ 已删除旧版『accepted 且 av_stream 不在 → av_stream_on.sh』恢复兜底："
log "  启动推流只能由板端 Qt 应用(CallClient)在 accepted 时触发，守护不再起流"
log "关键进程: intelligent_community(常驻) / av_stream(仅通话期间，按需启停)"
log "按需推流: 正常停止标记=$STOP_MARK  通话状态查询=$CALL_URL  人工启停标记=$MANAGED"

#-----------------------------------------------------------------------------
# 主循环
#-----------------------------------------------------------------------------
while :; do
    now=$(date +%s)
    av=$(pidof av_stream 2>/dev/null | awk '{print $1}')
    ic=$(pidof intelligent_community 2>/dev/null | awk '{print $1}')
    in_grace=0
    [ $((now - T0)) -lt "$GRACE" ] && in_grace=1
    paused=0
    [ -f "$PAUSE" ] && paused=1
    intent=0
    [ -f "$STOP_MARK" ] && intent=1
    TICK=$((TICK + 1))

    # --- 通话状态：周期性刷新；判"漏停"前再取一次新鲜值 ---
    if [ $((TICK % CALL_EVERY)) -eq 1 ]; then
        CALLSTATE=$(query_callstate)
    fi
    if [ -n "$av" ] && [ "$CALLSTATE" = "idle" ]; then
        CALLSTATE=$(query_callstate)          # 要判漏停：再确认一次
    fi

    # --- 健康判定：只要求 Qt 应用在。av_stream 在不在都算正常（按需推流）---
    healthy=0
    [ -n "$ic" ] && healthy=1

    # --- 喂狗：状态符合预期才喂 ---
    if [ "$healthy" = "1" ] && [ "$FEED_STOP" = "0" ]; then
        wd_feed
    fi

    # --- av_stream：漏停兜底（在跑但通话已 idle 连续 2 次 → 主动停掉）---
    #   ★ 这是本脚本唯一会碰 av_stream 的地方：**只会停，永远不会起**。
    if [ -n "$av" ] && [ "$CALLSTATE" = "idle" ]; then
        IDLE_RUNS=$((IDLE_RUNS + 1))
        if [ "$IDLE_RUNS" -ge 2 ]; then
            IDLE_RUNS=0
            log "漏停兜底：av_stream 在跑(pid=$av) 但通话状态连续 idle → 执行 av_stream_off.sh"
            /userdata/av_stream_off.sh >> "$LOG" 2>&1
            sleep 2
            av=$(pidof av_stream 2>/dev/null | awk '{print $1}')
        fi
    else
        IDLE_RUNS=0
    fi

    # --- Qt 应用守护 ---
    if [ "$healthy" = "0" ]; then
        if [ "$paused" = "1" ]; then
            log "维护挂起中（存在 $PAUSE）：ic=无 通话=${CALLSTATE}，不自动重启"
        elif [ "$in_grace" = "1" ]; then
            log "开机宽限中（$((now - T0))s/${GRACE}s）：ic=无，暂不重启"
        elif in_managed_stop; then
            log "人工启停进行中（存在 $MANAGED）：ic=无，不介入（避免叠出第二个界面）"
            FAIL_IC=0
        else
            # ★ 去抖：**保留"连续 3 次确认缺席才重启"的语义**，但把确认间隔从
            #   5 秒缩到 1 秒。原因：原来的 3×5s=15s 光"检测"就要 15 秒，再
            #   加上 /etc/init.d/S99smart_community restart 本身的 3~4 秒，
            #   实测崩溃→恢复要 ~19 秒，达不到"应用崩溃后 15 秒内拉起"的要求。
            #   连续 3 次、每次间隔 1 秒确认，同样不会把 1~2 秒的启停空窗当
            #   "挂了"（人工启停还有 /tmp/ic.managed_stop 标记兜着）。
            miss=0
            while [ "$miss" -lt 3 ]; do
                sleep 1
                ic=$(pidof intelligent_community 2>/dev/null | awk '{print $1}')
                [ -n "$ic" ] && break
                miss=$((miss + 1))
                log "intelligent_community 缺席确认 ${miss}/3"
            done
            if [ -n "$ic" ]; then
                log "intelligent_community 在去抖期内恢复（pid=$ic），不重启"
                FAIL_IC=0
            else
                FAIL_IC=$((FAIL_IC + 1))
                log "intelligent_community 连续 3 次确认缺席（连续第 $FAIL_IC/$FAIL_LIMIT 轮）→ 重启 /etc/init.d/S99smart_community"
                /etc/init.d/S99smart_community restart >> "$LOG" 2>&1
                sleep 3
                ic=$(pidof intelligent_community 2>/dev/null | awk '{print $1}')
                if [ -n "$ic" ]; then
                    log "intelligent_community 已拉起（pid=$ic，本轮用时约 $(( $(date +%s) - now ))s）"
                else
                    log "！！intelligent_community 拉起失败（第 $FAIL_IC 次），看 app.log / app_exit.log"
                fi
            fi
        fi
    else
        if [ "$FAIL_IC" -gt 0 ]; then
            log "intelligent_community 已恢复（pid=$ic），失败计数清零"
        fi
        FAIL_IC=0
    fi

    # --- 连续拉不起来 → 放弃喂狗，交给硬件看门狗 ---
    if [ "$FEED_STOP" = "0" ] && [ "$FAIL_IC" -ge "$FAIL_LIMIT" ]; then
        FEED_STOP=1
        log "！！严重：连续 ${FAIL_LIMIT} 次拉不起 Qt 应用（ic_fail=$FAIL_IC）"
        if [ -n "$WD" ] && [ "$WD_OPEN" = "1" ]; then
            log "！！停止喂狗 → 硬件看门狗将在超时后复位整机（这是设计行为）"
        else
            log "！！本板无硬件看门狗，无法整机复位；请人工介入（看 app.log / app_exit.log）"
        fi
    fi

    # --- 状态文件（取证用） ---
    {
        echo "ts=$(date '+%Y-%m-%d %H:%M:%S')"
        echo "guard_version=v3"
        echo "uptime_check=$(($(date +%s) - T0))s"
        echo "av_stream=${av:-NONE}"
        echo "intelligent_community=${ic:-NONE}"
        echo "callstate=$CALLSTATE"
        echo "av_stream_absent_ok=1"
        echo "intentional_stop=$intent"
        echo "pause=$paused"
        echo "managed_stop=$([ -f "$MANAGED" ] && echo 1 || echo 0)"
        echo "watchdog_dev=${WD:-NONE}"
        echo "watchdog_open=$WD_OPEN"
        echo "feed_stop=$FEED_STOP"
        echo "fail_ic=$FAIL_IC"
    } > "$STATE" 2>/dev/null

    # --- 正常时每 60 秒留一条"我还活着并且在喂狗"的痕迹 ---
    if [ "$healthy" = "1" ] && [ $((now - LAST_OK_LOG)) -ge 60 ]; then
        LAST_OK_LOG=$now
        if [ -n "$WD" ] && [ "$WD_OPEN" = "1" ]; then
            log "正常 ic=$ic av=${av:-无} 通话=$CALLSTATE，已喂狗($WD)"
        else
            log "正常 ic=$ic av=${av:-无} 通话=$CALLSTATE（软件守护；本板无硬件看门狗）"
        fi
    fi

    if [ $((now - LAST_ROT)) -ge 60 ]; then
        LAST_ROT=$now
        log_rotate
    fi

    sleep "$INTERVAL"
done
