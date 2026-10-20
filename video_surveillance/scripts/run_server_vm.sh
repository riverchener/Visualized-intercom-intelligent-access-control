#!/bin/bash
#=============================================================================
# run_server_vm.sh —— 在 Ubuntu 虚拟机里启动流媒体中转服务（后台常驻）
#
#   一条命令起全套：
#     * MediaMTX 网关（WebRTC :8889 = 唯一对外播放入口；RTSP :8554 = **内部发布通道**；
#       HLS 已整体删除 → 8888 不再监听）
#     * 中转服务（设备推流入口 :9100、网页与 API :8080）
#     * H.264+Opus 转码发布（板子只做 MJPEG 直通，编码在虚拟机）
#
#   用法： bash run_server_vm.sh          起（先停后起）
#          bash run_server_vm.sh stop     停（含 MediaMTX）
#          bash run_server_vm.sh status   看状态
#
#   ★ 2026-10-20（按需推流版）：中转**不再做任何录像/留影**，
#     所以 --record / --rec-dir / --record-fps / --rec-max-gb / --rec-max-days
#     这些参数与 SC_MOTION* 环境变量全部删掉。板子平时不推流 → /api/devices 为空、
#     MediaMTX 无路径，都是**预期**状态，不是故障。
#
#   ★ 2026-10-20（删除 HLS / VLC 版）：
#     - MediaMTX 配 hls: no → 8888 不再监听，脚本也不再打印任何 HLS 播放地址
#     - 对外播放入口只剩两个：网页 MJPEG(/) 与 WebRTC(:8889)
#     - RTSP :8554 **保留**，但只当内部发布通道（ffmpeg → MediaMTX），不对外宣传
#
#   可调环境变量：
#     SC_PUBLISH=0             关掉 H.264 转码发布（默认 1 开启）
#     SC_PUBLISH_BR=5M         转码码率上限
#     SC_PUBLISH_OUT=rtsp://127.0.0.1:8554   发布目标（只有 RTSP 这一种，HLS 已删除）
#     SC_PUBLISH_SCALE=1280x720 顺便降分辨率
#     SC_RING_TIMEOUT=30       响铃多久没人接就超时（秒）
#     SC_CALL_MAX_SEC=600      单通电话最长时长（秒，防状态卡死）
#=============================================================================
DIR=${DIR:-$HOME/video_surveillance}
PORT_TCP=${PORT_TCP:-9100}
PORT_HTTP=${PORT_HTTP:-8080}
LOG=$DIR/server.log
PIDF=$DIR/server.pid
MTX_DIR="$HOME/mediamtx"

# ---- ★ 直播转码发布（必须放在顶层，之前误插进 stop() 里导致不生效）----
export SC_PUBLISH="${SC_PUBLISH:-1}"
export SC_PUBLISH_FPS="${SC_PUBLISH_FPS:-30}"          # ★ 方案A：与板端一致
export SC_PUBLISH_BR="${SC_PUBLISH_BR:-5M}"            # ★ Phase1：CRF 模式下的码率上限
export SC_PUBLISH_CRF="${SC_PUBLISH_CRF-28}"           # ★ Phase1：CRF 28（实测 4.45Mbps / SSIM_Y 0.9318）
export SC_PUBLISH_X264=""                              # ★ 留空 = 用 live_publish.py 里的调优默认值
export SC_PUBLISH_OUT="${SC_PUBLISH_OUT:-rtsp://127.0.0.1:8554}"
export SC_PUBLISH_PRESET="${SC_PUBLISH_PRESET:-superfast}"   # ★ 方案A：编码档
export SC_PUBLISH_PROFILE="${SC_PUBLISH_PROFILE:-high}"     # ★ 方案A：High profile
export SC_PUBLISH_BUFSIZE="${SC_PUBLISH_BUFSIZE:-2M}"       # ★ 方案A：VBV 缓冲
# ★ 延迟探针（debug 用）：SC_LATPROBE=1 时把每帧到达中转的墙钟时间写 /tmp/sc_latprobe_<dev>.log
export SC_LATPROBE="${SC_LATPROBE:-0}"
# ★ 呼叫状态机参数（按需推流）
export SC_RING_TIMEOUT="${SC_RING_TIMEOUT:-30}"
export SC_CALL_MAX_SEC="${SC_CALL_MAX_SEC:-600}"

# ---- MediaMTX：WebRTC 网关（RTSP 仅内部发布用；HLS 已删除）（装了才启，没装不影响原有功能）----
# ★ 2026-10-20 修：原来用 `pgrep -f "$MTX_DIR/mediamtx"` 判断，但进程 cmdline 是
#   `./mediamtx ./mediamtx.yml`（相对路径），**永远匹配不到** →
#   start 会再起一个（抢不到端口、日志里一堆 bind 错误），stop 又停不掉。
#   改成按 "mediamtx + mediamtx.yml" 匹配，启动/停止两个方向都对。
MTX_PAT='mediamtx.*mediamtx\.yml'
start_mediamtx() {
    [ -x "$MTX_DIR/mediamtx" ] || return 0
    if pgrep -f "$MTX_PAT" > /dev/null; then
        echo "MediaMTX 已在运行"
    else
        ( cd "$MTX_DIR" && setsid nohup ./mediamtx ./mediamtx.yml > /tmp/mediamtx.log 2>&1 < /dev/null & )
        sleep 2
        echo "MediaMTX 已启动（对外 WebRTC :8889；内部发布 RTSP :8554；HLS 已删除）"
    fi
}
stop_mediamtx() {
    [ -x "$MTX_DIR/mediamtx" ] || return 0
    pkill -f "$MTX_PAT" 2>/dev/null && echo "MediaMTX 已停止" || true
}

stop() {
    pkill -f stream_server.py 2>/dev/null && echo "中转服务已停止" || echo "中转服务没有在跑"
    pkill -f 'ffmpeg.*sc_pub' 2>/dev/null || true
    [ -f $PIDF ] && rm -f $PIDF
    return 0
}

case "$1" in
    stop)
        stop; stop_mediamtx; exit 0 ;;
    status)
        ps aux | grep -v grep | grep stream_server.py || echo "中转服务：未运行"
        pgrep -f "$MTX_DIR/mediamtx" > /dev/null && echo "MediaMTX：运行中" || echo "MediaMTX：未运行"
        pgrep -f 'ffmpeg.*sc_pub' > /dev/null && echo "转码发布：运行中" || echo "转码发布：未运行"
        echo "--- 最近日志 ---"; tail -15 $LOG 2>/dev/null
        exit 0 ;;
esac

stop >/dev/null 2>&1
sleep 1
start_mediamtx
cd $DIR || exit 1

# ---- ★ 对讲用 HTTPS：浏览器只在安全上下文（https/localhost）允许 getUserMedia ----
#      自签证书，第一次打开浏览器会提示"不安全"，点继续即可（局域网自用）
TLS_ARG=""
if [ "${SC_TLS:-1}" = "1" ] && command -v openssl >/dev/null 2>&1; then
    mkdir -p $DIR/certs
    if [ ! -f $DIR/certs/server.crt ] || [ ! -f $DIR/certs/server.key ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -keyout $DIR/certs/server.key -out $DIR/certs/server.crt \
            -subj "/CN=192.168.137.94" >/dev/null 2>&1 \
            && echo "[tls] 已生成自签证书 $DIR/certs/server.crt"
    fi
    if [ -f $DIR/certs/server.crt ] && [ -f $DIR/certs/server.key ]; then
        TLS_ARG="--tls-port ${SC_TLS_PORT:-8443} --tls-cert $DIR/certs/server.crt --tls-key $DIR/certs/server.key"
    fi
fi

setsid nohup python3 server/stream_server.py \
    --tcp-port $PORT_TCP --http-port $PORT_HTTP \
    --web $DIR/web/index.html \
    $TLS_ARG \
    > $LOG 2>&1 < /dev/null &
echo $! > $PIDF
sleep 3
echo "=== 服务状态 ==="
ps aux | grep -v grep | grep stream_server.py
echo "=== 监听端口 ==="
(ss -tlnp 2>/dev/null || netstat -tlnp 2>/dev/null) | grep -E "($PORT_TCP|$PORT_HTTP|8554|8889)" || echo "端口未监听！"
echo "=== 启动日志 ==="
cat $LOG
