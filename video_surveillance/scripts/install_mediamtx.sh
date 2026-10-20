#!/bin/bash
#=============================================================================
# install_mediamtx.sh —— 在虚拟机装并启动 MediaMTX（WebRTC 网关；HLS 已删除，RTSP 仅内部发布用）
#   用法： bash install_mediamtx.sh          安装并启动
#          bash install_mediamtx.sh stop     停止
#          bash install_mediamtx.sh status   看状态（含已发布的流）
#=============================================================================
set -u
DIR=$HOME/mediamtx
BIN=$DIR/mediamtx
CFG=$DIR/mediamtx.yml
TGZ=$HOME/mediamtx.tar.gz

case "${1:-start}" in
stop)
    pkill -f "$DIR/mediamtx" && echo "已停止" || echo "没有在跑"
    exit 0
    ;;
status)
    if pgrep -f "$DIR/mediamtx" > /dev/null; then echo "MediaMTX 在跑 ✓"; else echo "MediaMTX 没跑 ✗"; fi
    echo "--- 已发布的流（API 9997）---"
    curl -s http://127.0.0.1:9997/v3/paths/list 2>/dev/null | head -c 800 || echo "（API 不可用）"
    echo
    exit 0
    ;;
esac

echo "== 1. 解包 =="
mkdir -p "$DIR"
if [ ! -f "$TGZ" ]; then echo "❌ 找不到 $TGZ（先在 Windows 侧下载并传上来）"; exit 1; fi
tar -xzf "$TGZ" -C "$DIR" 2>/dev/null || { echo "❌ 解包失败"; exit 1; }
chmod 755 "$BIN" 2>/dev/null
ls -l "$BIN"

echo
echo "== 2. 配置 =="
[ -f "$CFG" ] || cp -f "$(dirname "$0")/mediamtx.yml" "$CFG" 2>/dev/null
ls -l "$CFG"
grep -E 'rtspAddress|webrtcAddress|apiAddress' "$CFG"

echo
echo "== 3. 启动 =="
pkill -f "$DIR/mediamtx" 2>/dev/null; sleep 1
cd "$DIR"
setsid nohup ./mediamtx "$CFG" > /tmp/mediamtx.log 2>&1 < /dev/null &
sleep 4

echo
echo "== 4. 端口与日志 =="
ss -lntp 2>/dev/null | grep -E '8554|8889|9997' | head -6
tail -12 /tmp/mediamtx.log

echo
echo "== 5. 播放地址（把 <IP> 换成 $(hostname -I | awk '{print $1}')）=="
echo "   WebRTC(浏览器/手机，延迟最低，唯一对外播放入口) : http://<IP>:8889/<设备号>"
echo "   网页 MJPEG（中转服务 :8080）                   : http://<IP>:8080/"
echo "   （HLS 已删除；RTSP :8554 只是内部发布通道，不对外提供播放）"
