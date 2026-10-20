#!/bin/sh
#=============================================================================
# wifi_connect.sh —— 在**开发板上**执行：连接指定 WiFi（默认 cjh / 12345678）
#
#   为什么用 connman 而不是 wpa_supplicant 手工拉起？
#     板子出厂就跑着 connman（S45connman），它会托管 wpa_supplicant。
#     如果我们自己再起一个 wpa_supplicant，两者会抢 wlan0。
#     所以正确做法是给 connman 写一份"服务配置"，它会自己完成扫描、认证、DHCP，
#     并且下次开机自动重连。
#
#   用法：
#     sh wifi_connect.sh                     # 用默认 SSID/密码（脚本里可改）
#     sh wifi_connect.sh MyWiFi MyPassword   # 临时指定
#     sh wifi_connect.sh --status            # 只看当前状态
#     sh wifi_connect.sh --scan              # 扫描周围 AP
#
#   ⚠️ 前提：WiFi 硬件要先解除 rfkill 软禁用（本脚本会做）
#=============================================================================

SSID=${WIFI_SSID:-cjh}
PASS=${WIFI_PASS:-12345678}
CONF_DIR=/var/lib/connman
[ -n "$1" ] && [ "$1" != "--status" ] && [ "$1" != "--scan" ] && SSID="$1"
[ -n "$2" ] && PASS="$2"

# SSID -> 十六进制（connman 的服务配置段名要求 hex 编码，cjh => 636a68）
hex_ssid() {
    printf '%s' "$1" | od -An -tx1 | tr -d ' \n'
}

show_status() {
    echo "== rfkill =="
    rfkill list 2>/dev/null | grep -A2 -i wireless
    echo "== connman 服务 =="
    connmanctl services 2>/dev/null
    echo "== wlan0 =="
    ifconfig wlan0 2>/dev/null | grep -E 'inet addr|UP'
    echo "== 默认路由 =="
    ip route 2>/dev/null | head -3
    echo "== 当前时间 =="
    date
}

case "$1" in
--status)
    show_status
    exit 0
    ;;
--scan)
    rfkill unblock wifi
    connmanctl enable wifi >/dev/null 2>&1
    connmanctl scan wifi >/dev/null 2>&1
    sleep 6
    connmanctl services 2>/dev/null
    exit 0
    ;;
esac

HEX=$(hex_ssid "$SSID")
echo "=== 目标 WiFi: $SSID (hex=$HEX) ==="

#---------------------------------------------------------------- 1) 解除 rfkill
# 板子出厂默认把 WiFi 软禁用了（rfkill: phy0 Soft blocked: yes），不解除的话
# wlan0 永远起不来，connman 里 WiFi 也是 Powered=False。
if command -v rfkill >/dev/null 2>&1; then
    rfkill unblock wifi
    echo "rfkill: 已解除 WiFi 软禁用"
fi

#-------------------------------------------------------- 2) 打开 connman WiFi
command -v connmanctl >/dev/null 2>&1 || { echo "没有 connmanctl，退出"; exit 1; }
connmanctl enable wifi >/dev/null 2>&1
sleep 1

#------------------------------------------------ 3) 写自动连接配置（永久生效）
mkdir -p $CONF_DIR
cat > $CONF_DIR/$SSID.config <<EOF
[service_wifi_${HEX}_managed_psk]
Type = wifi
Name = $SSID
Passphrase = $PASS
EOF
echo "已写 $CONF_DIR/$SSID.config（开机自动重连）"

#------------------------------------------------------------ 4) 扫描并触发连接
connmanctl scan wifi >/dev/null 2>&1
sleep 5

SVC=$(connmanctl services 2>/dev/null | grep -w "$SSID" | awk '{print $NF}' | head -1)
if [ -z "$SVC" ]; then
    echo "!! 扫描不到 $SSID（检查 AP 是否在范围内/名字是否大小写一致）"
    connmanctl services 2>/dev/null
    exit 2
fi
echo "服务标识: $SVC"

connmanctl connect "$SVC" >/dev/null 2>&1

#-------------------------------------------------------------- 5) 等拿到 IP
i=0
while [ $i -lt 30 ]; do
    if ifconfig wlan0 2>/dev/null | grep -q "inet addr"; then
        break
    fi
    sleep 1
    i=$((i + 1))
done

if ifconfig wlan0 2>/dev/null | grep -q "inet addr"; then
    IP=$(ifconfig wlan0 | awk '/inet addr/{print $2}' | cut -d: -f2)
    echo "=== WiFi 已连接: $SSID, IP = $IP ==="
    exit 0
else
    echo "!! 连接超时（30 秒没拿到 IP）。用 'sh $0 --status' 看详情"
    exit 3
fi
