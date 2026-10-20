#!/bin/sh
# /userdata/modules/load_usb_audio.sh
# ---------------------------------------------------------------------------
# 加载 USB 音频（USB Audio Class / UAC1）支持，让 USB 麦克风被识别为 ALSA 声卡
#   模块：snd-usb-audio + 依赖（snd-usbmidi-lib / snd-rawmidi / snd-hwdep）
#   版本：6.1.118（与板端内核 vermagic 完全一致，见 build_info.txt）
#   手动：/userdata/modules/load_usb_audio.sh
#   日志：/userdata/modules/load_usb_audio.log
# 幂等：已加载的模块会跳过，可以反复调用
# ---------------------------------------------------------------------------
DIR=/userdata/modules
LOG=$DIR/load_usb_audio.log
MODS="snd-hwdep snd-rawmidi snd-usbmidi-lib snd-usb-audio"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

is_loaded() { [ -d "/sys/module/$1" ]; }

rc=0
for m in $MODS; do
    name=$(echo "$m" | tr '-' '_')
    if is_loaded "$name"; then
        log "skip  $m (already loaded)"
        continue
    fi
    if [ ! -f "$DIR/$m.ko" ]; then
        log "ERROR $m.ko not found in $DIR"
        rc=1
        continue
    fi
    if err=$(insmod "$DIR/$m.ko" 2>&1); then
        log "insmod $m OK"
    else
        log "insmod $m FAILED: ${err:-unknown}"
        rc=1
    fi
done

if [ $rc -eq 0 ]; then
    log "done, cards: $(cat /proc/asound/cards | sed -n 's/^ *\([0-9]\) \[\([^]]*\)\].*/\1=\2/p' | tr '\n' ' ')"
else
    log "FAILED, see above; dmesg tail: $(dmesg | tail -3 | tr '\n' '|')"
fi
exit $rc
