#!/bin/sh
#=============================================================================
# timesync.sh —— 在**开发板上**执行：联网后用 NTP 校准系统时间
#
# 为什么必须做：
#   这块板子**没有可用的 RTC 设备**（hwclock 报 /dev/misc/rtc 不存在），
#   一断电时间就回到 1970-01-01。界面上的日期/时间、文件时间戳、
#   天气接口的时效都会错。所以每次开机联网后都要校一次时间。
#
# 用法：
#   sh timesync.sh          # 等到有默认路由，然后校时
#   sh timesync.sh --now    # 不等待，立刻尝试
#=============================================================================

NOW=0
[ "$1" = "--now" ] && NOW=1

#-------------------------------------------------- 1) 等默认路由（最多 30 秒）
if [ "$NOW" = "0" ]; then
    i=0
    while [ $i -lt 30 ]; do
        if ip route 2>/dev/null | grep -q '^default'; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done
fi

if ! ip route 2>/dev/null | grep -q '^default'; then
    echo "timesync: 还没有默认路由（没联网），跳过校时"
    exit 1
fi

#----------------------------------------------------------- 2) 依次尝试 NTP 服务器
# 国内优先用阿里/腾讯的 NTP，pool.ntp.org 作兜底
for SRV in ntp.aliyun.com ntp1.aliyun.com cn.pool.ntp.org ntp.ntp.org pool.ntp.org; do
    echo "timesync: 尝试 $SRV ..."
    if ntpdate -b -u "$SRV" 2>&1; then
        echo "timesync: 校时成功 -> $(date)"
        # 有 RTC 的设备顺便写回硬件时钟（本板没有，会报错，忽略即可）
        hwclock -w >/dev/null 2>&1
        exit 0
    fi
done

echo "timesync: 所有 NTP 服务器都失败（可能校园网限制 UDP 123）"
exit 2
