#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stream_server.py —— 门禁可视对讲「流媒体 + 业务中转」服务

产品形态（★ 2026-10-20 按需推流版）：门口机（RK3506B）**只在通话期间**推流
→ 本服务 → 住户终端（网页）

职责：
  1) 接收 RK3506B 门口机推送的音视频流（TCP 9100，自定义 SCAV1 TLV 协议）
  2) 支持**多客户端同时拉取同一路流**（HTTP MJPEG 分发，共享最新帧）
  3) 呼叫状态机（本次新增，按需推流的核心）：
       POST /api/call/start?dev=      访客按「访客呼叫」→ 状态 ringing
       GET  /api/callstate/<dev>      板端/终端轮询，决定"开不开流/拉不拉流"
       POST /api/call/<dev>/accept    住户点【接听】→ accepted → 板端开始推流
       POST /api/call/<dev>/reject    住户点【拒绝】→ rejected（全程不推流）
       POST /api/call/<dev>/hangup    挂断 → idle → 板端停流
       状态：idle / ringing / accepted / rejected / timeout
       ringing 超过 SC_RING_TIMEOUT（默认 30s）自动转 timeout
  4) 对讲：
       * 上行（板→终端）：设备 PCM16 16k 音频直接复用现有链路
       * 下行（终端→板）：POST /api/talk 收浏览器麦克风 PCM16 → T_AUDIO_DOWN(6)
  5) 门禁业务：
       * POST /api/door   开门（nonce + 时间戳 + HMAC-SHA256，±30s 窗 + 一次性 nonce）
       * GET  /api/calls  通话记录（calls.json，很小，保留）
       * GET  /api/time   时间代理（板子没有 RTC 电池，重启会回到 1970）
       * GET  /api/weather 天气代理（10 分钟缓存，wttr.in 主源 + open-meteo 备用）
  6) 给 Web 终端提供：
       GET  /                             住户终端页（MJPEG，低延迟）
       GET  /webrtc                       同上，WebRTC(H.264+Opus) 模式
       POST /whep/<dev>                   WHEP 信令同源代理（https 页面也能用）
       GET  /live/<dev>.mjpg              实时视频
       GET  /live/<dev>.alaw              实时音频（浏览器端解码；PCM16→A-law）
       GET  /snapshot/<dev>.jpg           抓一张当前画面

只依赖 Python 3 标准库（asyncio），不需要装任何包。

用法：
    python3 stream_server.py --tcp-port 9100 --http-port 8080 [--web web/index.html]
                             [--tls-port 8443 --tls-cert certs/server.crt --tls-key certs/server.key]
"""

import argparse
import live_publish            # MJPEG → H.264/Opus → MediaMTX（对外只有 WebRTC；RTSP 是内部发布通道）
import asyncio
import hashlib
import hmac
import json
import os
import shutil
import struct
import time
import urllib.request
from collections import deque

MAGIC = b"SCAV"
HDR = 12                       # magic4 + type1 + flags1 + seq2 + len4
# ★ M3：T_AUDIO_DOWN=6 —— 中转→板端 的下行音频（终端说话，PCM16 16k 单声道）
T_VIDEO, T_AUDIO, T_STATUS, T_CONTROL, T_AUDIO_DOWN = 1, 2, 3, 5, 6
DOWN_CHUNK = 640               # 320 采样 × 2 字节 = 20ms @16kHz（和板端一帧对齐）

HEARTBEAT_TIMEOUT = 6.0        # 超过这个时间没心跳就算离线

def now():
    return time.time()

# =============================================================================
# ★ 音频格式工具：设备端发的是 16bit PCM（S16_LE）
#   网页（AudioContext + A-law 查表解码）不改，所以这里把 PCM 转回 A-law 再发。
#   速率 16 kHz × 2 字节 = 32 KB/s → 转成 16 KB/s 的 A-law，纯 Python 开销可忽略。
# =============================================================================
def _pcm_to_alaw(pcm):
    """S16_LE (bytes) → G.711 A-law (bytes)，按 ITU-T G.711 量化区间实现。

    ★ A-law 编码按 ITU-T G.711 标准实现：低位与高位的奇偶位交替取反，
      不能用“全部取反”的写法（那样会偏离标准，拿 ffmpeg 的 A-law
      解码器解出来是失真的，实测增益 +13.8 dB、相关系数只有 0.91）。
      现在统一按标准编码，ffmpeg/浏览器都能正确解出。实测与 ffmpeg 的
      A-law 编码逐字节一致率 98.7%，其余为 ±1 个量化台阶（听感无差别）。
    """
    n = len(pcm) // 2
    if n <= 0:
        return b""
    out = bytearray(n)
    buf = pcm if len(pcm) == n * 2 else pcm[:n * 2]      # 丢掉可能的半个采样
    for i, v in enumerate(struct.unpack("<%dh" % n, buf)):
        if v >= 0:
            mask = 0xD5                     # 正半轴：符号位 = 1
        else:
            mask = 0x55                     # 负半轴：符号位 = 0
            v = -v
            if v > 32767:                   # -32768 取反会溢出
                v = 32767
        if v < 256:                         # 第 0 段：阶距 16
            aval = (v >> 4) & 0x0F
        else:
            seg = v.bit_length() - 8        # 段号 1..7
            aval = ((seg << 4) | ((v >> (seg + 3)) & 0x0F)) & 0x7F
        out[i] = (aval ^ mask) & 0xFF
    return bytes(out)

def audio_payload(dev, chunk):
    """把设备音频包转成网页要的 A-law 字节（已经是 A-law 就原样返回）"""
    if getattr(dev, "audio_fmt", "alaw") == "s16le":
        return _pcm_to_alaw(chunk)
    return chunk

class Device:
    """一路设备（一个摄像头流）的会话状态"""

    def __init__(self, dev_id, peer, writer):
        self.dev_id = dev_id
        self.peer = peer
        self.writer = writer              # 用于下发控制指令
        self.hello = {}
        self.conn_at = now()
        self.last_hb = now()
        self.status = {"state": "connected"}

        # 视频
        self.latest_jpeg = None
        self.last_frame_ts = 0.0
        self.frames = 0
        self.bytes_video = 0
        self.subs = set()                 # 每个拉流客户端一个 asyncio.Queue
        self.fps_window = deque(maxlen=30)

        # 音频（只保留最近 1 秒左右，供新客户端快速起播）
        # 注意：用自增序号而不是 len() 做游标 —— deque 有 maxlen，
        #       写满以后 len() 不再变化，用它当游标会导致拉流端"卡住不再发数据"
        self.audio_chunks = deque(maxlen=50)   # 元素是 (seq, bytes)
        self.audio_seq = 0
        self.audio_bytes = 0
        self.audio_fmt = "alaw"          # 设备音频格式：alaw(旧) / s16le(PCM16)
        self.audio_rate = 8000

        # ★ M3：给设备写包（控制指令 + 下行音频）必须串行，否则帧头/载荷会交错
        self.wlock = asyncio.Lock()
        self.wseq = 0

    # ---------- 服务 → 设备 ----------
    async def send_pkt(self, ptype, payload):
        """向设备写一个 SCAV1 包（控制指令/下行音频共用，带锁 + 独立序号）"""
        try:
            async with self.wlock:
                self.wseq = (self.wseq + 1) & 0xffff
                hdr = (MAGIC + bytes([ptype, 0])
                       + struct.pack("<H", self.wseq)
                       + struct.pack("<I", len(payload)))
                self.writer.write(hdr + payload)
                await self.writer.drain()
            return True
        except Exception as e:
            log("[ctl] 写设备 %s 失败: %r" % (self.dev_id, e))
            return False

    async def push_down_audio(self, pcm):
        """★ M3：下行音频（住户说话）→ T_AUDIO_DOWN，按 20ms 一包切分"""
        n = len(pcm) - (len(pcm) % 2)
        if n <= 0:
            return 0
        step = DOWN_CHUNK          # 320 采样 = 640 字节 = 20ms @16k
        sent = 0
        for off in range(0, n, step):
            chunk = pcm[off:off + step]
            if not await self.send_pkt(T_AUDIO_DOWN, chunk):
                break
            sent += len(chunk)
        if sent:
            TALK_STATS["down_bytes"] += sent
            TALK_STATS["down_pkts"] += 1
            TALK_STATS["last_at"] = now()
            c = CUR_CALL.get(self.dev_id)
            if c:
                c["audio"] = True
                c["down_bytes"] = c.get("down_bytes", 0) + sent
                c["last_act"] = round(now(), 1)
        return sent

    # ---------- 视频分发 ----------
    def push_frame(self, jpeg):
        lat_probe(self.dev_id, len(jpeg))    # ★ SC_LATPROBE=1 时的延迟探针（默认空操作）
        if PUBLISH is not None:              # 实时转码发布（不影响原有分发）
            PUBLISH.feed_video(self.dev_id, jpeg)
        self.latest_jpeg = jpeg
        self.last_frame_ts = now()
        self.frames += 1
        self.bytes_video += len(jpeg)
        self.fps_window.append(self.last_frame_ts)
        for q in list(self.subs):
            if q.full():
                try:
                    q.get_nowait()        # 丢旧帧，保证低延迟
                except asyncio.QueueEmpty:
                    pass
            try:
                q.put_nowait(jpeg)
            except asyncio.QueueFull:
                pass

    def push_audio(self, chunk):
        if PUBLISH is not None:              # 音频一并转码
            PUBLISH.feed_audio(self.dev_id, chunk)
        self.audio_seq += 1
        self.audio_chunks.append((self.audio_seq, chunk))
        self.audio_bytes += len(chunk)
        c = CUR_CALL.get(self.dev_id)
        if c:
            c["up_bytes"] = c.get("up_bytes", 0) + len(chunk)

    def close(self):
        for q in list(self.subs):
            try:
                q.put_nowait(None)
            except asyncio.QueueFull:
                pass

    def fps(self):
        if len(self.fps_window) < 2:
            return 0.0
        span = self.fps_window[-1] - self.fps_window[0]
        return round((len(self.fps_window) - 1) / span, 1) if span > 0 else 0.0

    def info(self):
        return {
            "dev": self.dev_id,
            "peer": self.peer,
            # 会话号：设备每次重连都会变。前端用它判断"是不是新会话"，
            # 从而在设备重连后重新建立拉流（否则 <img> 会一直挂在旧连接上）
            "session": int(self.conn_at),
            "online": (now() - self.last_hb) < HEARTBEAT_TIMEOUT,
            "connected_for": round(now() - self.conn_at, 1),
            "fps": self.fps(),
            "frames": self.frames,
            "video_kbytes": self.bytes_video // 1024,
            # 分辨率来自设备每秒上报的 STATUS（"res":"1280x720"），hello 里的是兜底
            "video_resolution": self.status.get("res") or self.hello.get("resolution", ""),
            "audio_kbytes": self.audio_bytes // 1024,
            "subscribers": len(self.subs),
            "audio_rate": getattr(self, "audio_rate", 8000),   # 音频采样率（网页解码要用）
            "audio_fmt": getattr(self, "audio_fmt", "alaw"),   # 设备音频格式：s16le / alaw
            "frame_age_ms": (int((now() - self.last_frame_ts) * 1000)
                             if getattr(self, "last_frame_ts", 0) else None),
            "cpu": getattr(self, "cpu", None),
            "degraded": getattr(self, "degraded", None),
            "state": self.status.get("state", "?"),
            "device_status": self.status,
            "last_frame_age": round(now() - self.last_frame_ts, 2) if self.last_frame_ts else None,
            "hello": self.hello,
        }

DEVICES = {}
#   发布目标只能是 RTSP（MediaMTX 内部通道）→ 对外由 MediaMTX 出 WebRTC。

# ★ 延迟探针（debug 开关，默认关闭）：SC_LATPROBE=1 时把每帧到达中转的墙钟时间
#   与字节数追加到 /tmp/sc_latprobe_<dev>.log，用于端到端延迟分解。
LATPROBE = os.environ.get("SC_LATPROBE") == "1"
_lat_fps = {}

def lat_probe(dev_id, nbytes):
    if not LATPROBE:
        return
    try:
        fp = _lat_fps.get(dev_id)
        if fp is None:
            fp = open("/tmp/sc_latprobe_%s.log" % dev_id, "a", buffering=1)
            _lat_fps[dev_id] = fp
        fp.write("%.6f %d\n" % (time.time(), nbytes))
    except Exception:
        pass

def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

# =============================================================================
# ★ M1：时间 / 天气代理（板端不直连外网，只问中转）
#   板子没有 RTC 电池，重启会回到 1970；而 VM 有外网+准时钟，所以由中转代取。
#   天气 10 分钟缓存（wttr.in 主源 + open-meteo 备用，都免 key）。
# =============================================================================
WEATHER_CITY = os.environ.get("SC_WEATHER_CITY", "Shenzhen")     # wttr.in 英文城市名
WEATHER_CITY_CN = os.environ.get("SC_WEATHER_CITY_CN", "深圳")
WEATHER_LAT = os.environ.get("SC_WEATHER_LAT", "22.54")          # 深圳（open-meteo 备用源）
WEATHER_LON = os.environ.get("SC_WEATHER_LON", "114.06")
WEATHER_TTL = float(os.environ.get("SC_WEATHER_TTL", "600"))     # 10 分钟

_WEATHER = {"at": 0.0, "data": None, "last_error": None, "source": None}

# 天气英文 → 中文（wttr.in 在没带 lang 参数时只给英文描述；带了就用 lang_zh）
_WX_MAP = [
    ("thundery", "雷阵雨"), ("heavy rain", "大雨"), ("moderate rain", "中雨"),
    ("light rain", "小雨"), ("patchy rain", "零星小雨"), ("rain", "雨"),
    ("heavy snow", "大雪"), ("moderate snow", "中雪"), ("light snow", "小雪"),
    ("snow", "雪"), ("sleet", "雨夹雪"), ("freezing", "冻雨"),
    ("mist", "薄雾"), ("fog", "雾"), ("haze", "霾"),
    ("overcast", "阴"), ("cloudy", "多云"), ("partly cloudy", "晴间多云"),
    ("sunny", "晴"), ("clear", "晴"), ("windy", "大风"),
]
# WMO weather code（open-meteo 备用源）
_WMO_MAP = {
    0: "晴", 1: "晴间多云", 2: "多云", 3: "阴", 45: "雾", 48: "雾凇",
    51: "毛毛雨", 53: "毛毛雨", 55: "毛毛雨", 56: "冻雨", 57: "冻雨",
    61: "小雨", 63: "中雨", 65: "大雨", 66: "冻雨", 67: "冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "雪粒",
    80: "阵雨", 81: "阵雨", 82: "强阵雨", 85: "阵雪", 86: "阵雪",
    95: "雷阵雨", 96: "雷阵雨伴冰雹", 99: "雷阵雨伴冰雹",
}

def _wx_cn(text):
    t = (text or "").lower()
    for k, v in _WX_MAP:
        if k in t:
            return v
    return text or "--"

def _http_get_json(url, timeout=8):
    req = urllib.request.Request(url, headers={
        "User-Agent": "curl/8.0 (rk3506b-door-station)", "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))

def _fetch_weather_wttr():
    d = _http_get_json("https://wttr.in/%s?format=j1&lang=zh-cn" % WEATHER_CITY)
    cur = (d.get("current_condition") or [{}])[0]
    desc = cur.get("lang_zh") or _wx_cn(((cur.get("weatherDesc") or [{}])[0] or {}).get("value", ""))
    return {
        "ok": True, "source": "wttr.in", "city": WEATHER_CITY_CN,
        "text": desc or "--",
        "temp_c": int(float(cur.get("temp_C", "nan"))) if cur.get("temp_C") else None,
        "feels_c": int(float(cur.get("FeelsLikeC", "nan"))) if cur.get("FeelsLikeC") else None,
        "humidity": int(float(cur.get("humidity", "nan"))) if cur.get("humidity") else None,
        "wind_kmph": cur.get("windspeedKmph"),
        "obs": cur.get("observation_time"),
    }

def _fetch_weather_openmeteo():
    u = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
         "&current=temperature_2m,relative_humidity_2m,weather_code"
         "&timezone=Asia%%2FShanghai" % (WEATHER_LAT, WEATHER_LON))
    d = _http_get_json(u)
    cur = d.get("current") or {}
    code = cur.get("weather_code")
    return {
        "ok": True, "source": "open-meteo", "city": WEATHER_CITY_CN,
        "text": _WMO_MAP.get(code, "未知"),
        "temp_c": round(float(cur["temperature_2m"])) if cur.get("temperature_2m") is not None else None,
        "feels_c": None,
        "humidity": int(cur["relative_humidity_2m"]) if cur.get("relative_humidity_2m") is not None else None,
        "wind_kmph": None,
        "obs": cur.get("time"),
    }

def get_weather():
    """带 10 分钟缓存；失败时返回 {ok:False,...}（板端显示 --，不阻塞）"""
    fresh = _WEATHER["data"] and (now() - _WEATHER["at"]) < WEATHER_TTL
    if fresh:
        d = dict(_WEATHER["data"])
        d["cached"] = True
        d["age"] = round(now() - _WEATHER["at"], 1)
        return d
    err = None
    for fn in (_fetch_weather_wttr, _fetch_weather_openmeteo):
        try:
            d = fn()
            d["ts"] = now()
            _WEATHER.update(at=now(), data=d, last_error=None, source=d["source"])
            log("[weather] %s: %s %s℃ 湿度%s%% (%s)" % (
                d["source"], d["text"], d["temp_c"], d["humidity"], d["city"]))
            d = dict(d)
            d["cached"] = False
            return d
        except Exception as e:
            err = "%s: %s" % (fn.__name__, e)
            log("[weather] 取数失败 %s" % err)
    _WEATHER["last_error"] = err
    if _WEATHER["data"]:                     # 取不到新的就返回旧缓存（标记 stale）
        d = dict(_WEATHER["data"])
        d["cached"] = True
        d["stale"] = True
        d["error"] = err
        return d
    return {"ok": False, "error": err, "text": "--", "temp_c": None, "humidity": None,
            "city": WEATHER_CITY_CN}

def time_info():
    t = time.localtime()
    now_s = int(time.time())
    return {
        "epoch": now_s,
        "epoch_ms": int(now() * 1000),
        "iso": time.strftime("%Y-%m-%dT%H:%M:%S%z", t),
        "date": time.strftime("%Y-%m-%d", t),
        "time": time.strftime("%H:%M:%S", t),
        "weekday": "星期" + "一二三四五六日"[t.tm_wday],
        "tz": time.strftime("%Z", t),
        "monotonic": time.monotonic(),
    }

# =============================================================================
# ★ M5：门禁会话（开门防重放 + 通话记录 + ★ 2026-10-20 新增呼叫状态机）
#   * 开门请求：nonce + ts + HMAC-SHA256，时间窗 ±30s，nonce 一次性
#   * 通话记录：calls.json（开始/结束/时长/是否有对讲音频/谁开门）—— 保留
#   * 呼叫状态：CALL_STATES[dev] = {state, since, ring_start, ...}
#       状态机 idel → ringing → accepted → idle
#                      └→ rejected / timeout
#     板端 Qt 每 1s 轮询 /api/callstate/<dev>，只有 accepted 才启动 av_stream；
#     终端网页轮询到 ringing 弹【接听】【拒绝】，接听后开始拉流。
# =============================================================================
DOOR_KEY = os.environ.get("SC_DOOR_KEY", "rk3506b-door-2026")   # 中转配置里的开门密钥
DOOR_WINDOW = float(os.environ.get("SC_DOOR_WINDOW", "30"))     # 时间窗 ±30s
DATA_DIR = os.environ.get("SC_DATA_DIR", "")                    # 空=用程序目录的上级
CALL_RING_TIMEOUT = float(os.environ.get("SC_RING_TIMEOUT", "30"))    # 响铃超时（秒）
CALL_MAX_SEC = float(os.environ.get("SC_CALL_MAX_SEC", "600"))        # 单通最长时长
CALL_LINGER = float(os.environ.get("SC_CALL_LINGER", "8"))            # rejected/timeout 保留多久

_USED_NONCE = {}          # nonce -> ts（一次性；按时间窗清理）
CALLS = []                # 内存里的通话记录（同时落盘 calls.json）
CUR_CALL = {}             # dev -> 进行中的通话 dict
CALL_STATES = {}          # dev -> 呼叫状态 dict（★ 新增，按需推流的核心）
TALK_STATS = {"down_bytes": 0, "down_pkts": 0, "up_bytes": 0, "last_at": 0.0}
WHEP_HOST, WHEP_PORT = "127.0.0.1", 8889      # MediaMTX 的 WHEP 入口（/whep 代理用）

CALL_STATES_ALL = ("idle", "ringing", "accepted", "rejected", "timeout")

def pick_device(dev_id=""):
    """按设备号取会话；没指定就取第一路（单门口机的常见用法）"""
    if dev_id:
        return DEVICES.get(dev_id)
    for d in DEVICES.values():
        return d
    return None

def _fmt_ts(ts):
    try:
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(ts)))
    except Exception:
        return "--"

# ---------------------------------------------------------------- 呼叫状态机
def cs_get(dev_id):
    """取某设备的呼叫状态（没有就返回 idle；纯内存，进程重启即清零）"""
    st = CALL_STATES.get(dev_id)
    if not st:
        return {"dev": dev_id, "state": "idle", "since": None, "since_iso": "--",
                "ring_start": None, "answered_at": None, "reason": None}
    return st

def cs_set(dev_id, state, **extra):
    st = CALL_STATES.setdefault(dev_id, {"dev": dev_id})
    st["state"] = state
    st["since"] = round(now(), 3)
    st["since_iso"] = _fmt_ts(now())
    st.update(extra)
    log("[callstate] %s → %s%s" % (dev_id, state,
                                   ("（%s）" % extra["reason"]) if extra.get("reason") else ""))
    return st

def cs_public(dev_id):
    """给 HTTP 的状态视图（/api/callstate）"""
    st = dict(cs_get(dev_id))
    st["call"] = CUR_CALL.get(dev_id)
    st["dev_connected"] = (dev_id in DEVICES)
    return st

# ---------------------------------------------------------------- 通话日志
def call_start(dev_id, source="board"):
    """开始一次通话（幂等：同一设备同时只记一通）"""
    c = CUR_CALL.get(dev_id)
    if c:
        return c
    c = {
        "id": "%s-%d" % (dev_id, int(now())),
        "dev": dev_id,
        "source": source,
        "start": round(now(), 1),
        "start_iso": _fmt_ts(now()),
        "end": None,
        "end_iso": None,
        "duration": None,
        "audio": False,          # 有没有真的收到对讲音频（上行/下行任一）
        "up_bytes": 0,
        "down_bytes": 0,
        "door": [],              # [{"at":..,"by":"web"}]
        "ended_by": None,
        "last_act": round(now(), 1),
    }
    CUR_CALL[dev_id] = c
    CALLS.append(c)
    log("[call] 开始通话 %s（来源=%s）" % (dev_id, source))
    return c

def call_end(dev_id, reason="hangup"):
    c = CUR_CALL.pop(dev_id, None)
    if not c:
        return None
    end = now()
    c["end"] = round(end, 1)
    c["end_iso"] = _fmt_ts(end)
    c["duration"] = round(end - c["start"], 1)
    c["ended_by"] = reason
    _save_calls()
    log("[call] 结束通话 %s 时长 %.1fs 对讲音频=%s 开门=%d 次（%s）" %
        (dev_id, c["duration"], "有" if c["audio"] else "无", len(c["door"]), reason))
    return c

def _call_touch(dev_id):
    """有对讲音频就标记本通电话"有声音"（网页 POST /api/talk 时调）

    ★ 按需推流：只有真的在通话（ringing/accepted）才算，否则会在"平时没有通话
      但板端还在推流"的情况下凭空造出一条通话记录（2026-10-20 实测踩到）。
    """
    st = cs_get(dev_id)
    if st.get("state") not in ("accepted", "ringing"):
        return None
    c = CUR_CALL.get(dev_id)
    if not c:
        c = call_start(dev_id, source="audio")
    c["audio"] = True
    return c

# ---------------------------------------------------------------- 呼叫动作
def call_ring(dev_id, source="board"):
    """访客按下呼叫按钮 → ringing（30 秒内没人接就 timeout）"""
    st = cs_get(dev_id)
    if st.get("state") == "ringing":
        return st                          # 重复按：保持原响铃计时
    if st.get("state") == "accepted":
        return st                          # 已在通话中，不重新响铃
    call_start(dev_id, source=source)
    return cs_set(dev_id, "ringing", ring_start=round(now(), 3), reason=None,
                  answered_at=None, source=source)

def call_accept(dev_id):
    st = cs_get(dev_id)
    if st.get("state") != "ringing":
        return None, "not-ringing"
    c = CUR_CALL.get(dev_id) or call_start(dev_id, source="accept")
    c["last_act"] = round(now(), 1)
    return cs_set(dev_id, "accepted", answered_at=round(now(), 3), reason="accept"), "ok"

def call_reject(dev_id):
    st = cs_get(dev_id)
    if st.get("state") != "ringing":
        return None, "not-ringing"
    call_end(dev_id, reason="rejected")
    return cs_set(dev_id, "rejected", reason="rejected"), "ok"

def _stop_publish(dev_id):
    """停掉某设备的 H.264 转发（非阻塞）。

    ★ 必须丢到线程池：DevicePublisher.stop() 里 ffmpeg 要 terminate + wait(3s)，
      直接调会把 asyncio 事件循环卡住 3 秒 —— 实测表现为
      "点挂断后中转 3 秒才回响应"，板端停流延迟被白白拉长
      （2026-10-20 实测：挂断→停流 3.64s，其中 3s 是这里阻塞）。
    """
    if PUBLISH is None:
        return
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        loop = asyncio.get_event_loop()
    try:
        loop.run_in_executor(None, PUBLISH.stop_dev, dev_id)
    except Exception as e:
        log("[发布] 排队停止发布器失败: %r" % e)

def call_hangup(dev_id, reason="hangup"):
    st = cs_get(dev_id)
    if st.get("state") in ("idle", None):
        return st, "already-idle"
    call_end(dev_id, reason=reason)
    # ★ 挂断就停 H.264 转发（ffmpeg），让 MediaMTX 的路径尽快消失：
    #   只等设备 TCP 断开的话要慢 2~4 秒，期间 MediaMTX 里还挂着一条"有路径但没帧"。
    _stop_publish(dev_id)
    return cs_set(dev_id, "idle", reason=reason), "ok"

def _cs_file(name):
    d = DATA_DIR or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, name)

def _load_calls():
    global CALLS
    try:
        p = _cs_file("calls.json")
        if os.path.isfile(p):
            with open(p, "r", encoding="utf-8") as f:
                CALLS = json.load(f) or []
    except Exception as e:
        log("[call] 读 calls.json 失败: %r" % e)
        CALLS = []

def _save_calls():
    try:
        p = _cs_file("calls.json")
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(CALLS[-500:], f, ensure_ascii=False, indent=1)
        os.replace(tmp, p)
    except Exception as e:
        log("[call] 写 calls.json 失败: %r" % e)

def door_sign(nonce, ts, dev, key=None):
    """HMAC-SHA256(key, "<dev>|<ts>|<nonce>")；网页端有同一套实现"""
    msg = "%s|%s|%s" % (dev, ts, nonce)
    return hmac.new((key or DOOR_KEY).encode(), msg.encode(), hashlib.sha256).hexdigest()

def door_verify(dev, nonce, ts, sig):
    """返回 (ok, reason)。校验时间窗 + 签名 + nonce 一次性"""
    if not nonce or len(nonce) < 8:
        return False, "nonce 缺失或过短"
    try:
        ts = float(ts)
    except Exception:
        return False, "ts 非法"
    drift = abs(now() - ts)
    if drift > DOOR_WINDOW:
        return False, "时间窗超限（%.1fs > %.0fs）" % (drift, DOOR_WINDOW)
    for k, v in list(_USED_NONCE.items()):      # 清理过期 nonce
        if now() - v > DOOR_WINDOW * 4:
            _USED_NONCE.pop(k, None)
    if nonce in _USED_NONCE:
        return False, "nonce 已被使用（重放）"
    exp = door_sign(nonce, int(ts), dev)
    if not hmac.compare_digest(exp, (sig or "").lower()):
        return False, "签名不匹配"
    _USED_NONCE[nonce] = now()
    return True, "ok"

# ★ 直播转码发布（MJPEG → H.264/Opus → MediaMTX）；ffmpeg 只用于转码，不落盘
FFMPEG = shutil.which("ffmpeg") or ""
PUBLISH = None                 # 直播转码发布器（H.264 → MediaMTX），默认关闭

# =============================================================================
# 设备接入（TCP 9100）
# =============================================================================
async def handle_device(reader, writer):
    peer = writer.get_extra_info("peername")
    peer = "%s:%s" % (peer[0], peer[1]) if peer else "?"
    dev = None
    log("[tcp] 设备接入 %s" % peer)

    try:
        while True:
            hdr = await reader.readexactly(HDR)
            if hdr[:4] != MAGIC:
                log("[tcp] 魔数错误，关闭 %s" % peer)
                break
            ptype = hdr[4]
            flags = hdr[5]
            seq = struct.unpack_from("<H", hdr, 6)[0]
            length = struct.unpack_from("<I", hdr, 8)[0]
            if length > 4 * 1024 * 1024:
                log("[tcp] 包过大 %d，关闭" % length)
                break
            payload = await reader.readexactly(length) if length else b""

            # --- 首包：hello（flags=1 的 STATUS）---
            if dev is None:
                try:
                    hello = json.loads(payload.decode("utf-8", "replace"))
                except Exception:
                    hello = {}
                dev_id = hello.get("dev") or ("dev-%s" % peer.split(":")[0])
                # 同一设备重连：顶掉旧会话
                if dev_id in DEVICES:
                    old = DEVICES[dev_id]
                    old.close()
                    log("[tcp] %s 重连，替换旧会话" % dev_id)
                dev = Device(dev_id, peer, writer)
                dev.hello = hello
                # ★ 音频格式/采样率：设备 Hello 里上报 pcm16k / pcm8k / alaw16k / alaw8k
                try:
                    _a = str(dev.hello.get('audio', 'alaw8k'))
                    dev.audio_rate = 16000 if '16' in _a else 8000
                    dev.audio_fmt = "s16le" if _a.startswith("pcm") else "alaw"   # PCM16 直传
                except Exception:
                    dev.audio_rate = 8000
                    dev.audio_fmt = "alaw"
                if PUBLISH is not None:
                    PUBLISH.set_audio_rate(dev.dev_id, dev.audio_rate)
                    PUBLISH.set_audio_fmt(dev.dev_id, dev.audio_fmt)
                log("[tcp] %s 音频: fmt=%s rate=%d" % (dev_id, dev.audio_fmt, dev.audio_rate))

                DEVICES[dev_id] = dev
                # 设备连上来了 → 记一笔 "dev_seen"，供 accepted 后的兜底判断用
                st = CALL_STATES.setdefault(dev_id, {"dev": dev_id, "state": "idle"})
                st["dev_seen_at"] = round(now(), 3)
                log("[tcp] 设备注册: %s (%s) hello=%s" % (dev_id, peer, hello))
                continue

            dev.last_hb = now()

            if ptype == T_VIDEO:
                dev.push_frame(payload)
            elif ptype == T_AUDIO:
                dev.push_audio(payload)
            elif ptype == T_STATUS:
                try:
                    dev.status = json.loads(payload.decode("utf-8", "replace"))
                except Exception:
                    pass
    except (asyncio.IncompleteReadError, ConnectionResetError):
        pass
    except Exception as e:
        log("[tcp] %s 异常: %r" % (peer, e))
    finally:
        if dev:
            dev.close()
            # 只在自己还是当前会话时移除（防止重连后被旧会话删掉）
            if DEVICES.get(dev.dev_id) is dev:
                del DEVICES[dev.dev_id]
            # ★ 按需推流：设备断开时把发布器也停掉，别让 ffmpeg 空转重连
            #   （非阻塞，见 _stop_publish 的注释）
            _stop_publish(dev.dev_id)
            log("[tcp] 设备断开: %s (共收 %d 帧)" % (dev.dev_id, dev.frames))
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass

async def send_control(dev, cmd):
    """给设备下发控制指令（复用推流连接）"""
    payload = json.dumps({"cmd": cmd}).encode()
    ok = await dev.send_pkt(T_CONTROL, payload)
    log("[ctl] -> %s: %s %s" % (dev.dev_id, cmd, "ok" if ok else "失败"))
    return ok

# =============================================================================
# HTTP（8080）：给 Web 客户端 / 业务后台
# =============================================================================
HTTP_REASONS = {200: "OK", 404: "Not Found", 400: "Bad Request", 405: "Method Not Allowed",
                403: "Forbidden", 500: "Internal Server Error"}

async def http_head(writer, code=200, ctype="text/plain; charset=utf-8", extra=""):
    body = b""
    head = ("HTTP/1.1 %d %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n%s\r\n" % (code, HTTP_REASONS.get(code, "OK"), ctype, len(body), extra))
    writer.write(head.encode())
    await writer.drain()

async def http_json(writer, obj, code=200):
    body = json.dumps(obj, ensure_ascii=False, indent=2).encode()
    head = ("HTTP/1.1 %d %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: %d\r\n\r\n" % (code, HTTP_REASONS.get(code, "OK"), len(body)))
    writer.write(head.encode() + body)
    await writer.drain()

async def http_text(writer, text, code=200):
    body = (text or "").encode()
    head = ("HTTP/1.1 %d %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: %d\r\n\r\n" % (code, HTTP_REASONS.get(code, "OK"), len(body)))
    writer.write(head.encode() + body)
    await writer.drain()

async def stream_mjpeg(dev, writer):
    """multipart/x-mixed-replace —— 多个客户端可以同时拉同一路流"""
    q = asyncio.Queue(maxsize=1)
    dev.subs.add(q)
    log("[http] +1 拉流客户端 %s（当前 %d 个）" % (dev.dev_id, len(dev.subs)))
    try:
        writer.write(("HTTP/1.1 200 OK\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n"
                      "Cache-Control: no-cache, no-store\r\n"
                      "Pragma: no-cache\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n").encode())
        await writer.drain()
        # 先把当前帧发出去，新客户端立刻有画面
        if dev.latest_jpeg:
            await send_mjpeg_part(writer, dev.latest_jpeg)
        while True:
            frame = await q.get()
            if frame is None:
                break
            await send_mjpeg_part(writer, frame)
    except (ConnectionResetError, BrokenPipeError, asyncio.CancelledError):
        pass
    finally:
        dev.subs.discard(q)
        log("[http] -1 拉流客户端 %s（剩余 %d 个）" % (dev.dev_id, len(dev.subs)))

async def send_mjpeg_part(writer, jpeg):
    # ★ 多带一个 X-Frame-Time（中转分发时刻，epoch 毫秒）：浏览器/探针页面可以据此
    #   算 "中转→客户端" 的传输+渲染延迟；<img> 原生解码会忽略未知 part 头，无副作用。
    writer.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                 b"X-Frame-Time: %d\r\nContent-Length: %d\r\n\r\n"
                 % (int(time.time() * 1000), len(jpeg)))
    writer.write(jpeg)
    writer.write(b"\r\n")
    await writer.drain()

async def stream_alaw(dev, writer):
    """把最近 1 秒音频先补发，再持续推送（浏览器端解码）

    源是 PCM16（dev.audio_fmt == 's16le'）时逐包转成 A-law：网页与 AudioContext
    那边完全不用改；源本来就是 A-law 时原样透传。
    """
    writer.write(("HTTP/1.1 200 OK\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Connection: close\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Type: application/octet-stream\r\n\r\n").encode())
    try:
        last_seq = 0
        for seq, c in list(dev.audio_chunks):      # 先补发最近 1 秒，客户端立刻有声
            writer.write(audio_payload(dev, c))
            last_seq = seq
        await writer.drain()
        while True:
            await asyncio.sleep(0.02)
            for seq, c in list(dev.audio_chunks):
                if seq > last_seq:                 # 用序号判断"新数据"，不依赖 len()
                    writer.write(audio_payload(dev, c))
                    last_seq = seq
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, asyncio.CancelledError):
        pass

async def proxy_whep(dev, method, body, ctype, writer):
    """把浏览器的 WHEP 请求原样转给 MediaMTX（本机 8889），响应原样回传。

    ★ 为什么要代理：浏览器只在**安全上下文**（https / localhost）里允许
      getUserMedia，而对讲上行必须要麦克风。所以页面额外用 https 提供
      （--tls-port 8443）。但 https 页面不能直接 fetch http://<ip>:8889
      （active mixed content 会被浏览器拦掉），于是让中转在**同一个源**上
      代理 /whep —— 页面对 https://<ip>:8443/whep/<dev> 发请求即可。
      ICE 走 UDP 8189，不受混合内容规则影响。
    """
    if not dev or "/" in dev:
        await http_json(writer, {"error": "bad dev"}, 400)
        return
    try:
        up_r, up_w = await asyncio.open_connection(WHEP_HOST, WHEP_PORT)
    except Exception as e:
        await http_json(writer, {"error": "whep upstream unreachable: %r" % e}, 502)
        return
    try:
        hdr = ("%s /%s/whep HTTP/1.1\r\nHost: %s:%d\r\n"
               "Content-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
               % (method, dev, WHEP_HOST, WHEP_PORT, ctype or "application/sdp", len(body)))
        up_w.write(hdr.encode() + body)
        await up_w.drain()
        while True:
            chunk = await up_r.read(65536)
            if not chunk:
                break
            writer.write(chunk)
            await writer.drain()
    except Exception as e:
        log("[whep] 代理异常: %r" % e)
    finally:
        try:
            up_w.close()
        except Exception:
            pass

async def serve_static(writer, path):
    try:
        with open(path, "rb") as f:
            body = f.read()
    except Exception:
        await http_json(writer, {"error": "web page not found: %s" % path}, 404)
        return
    ctype = "text/html; charset=utf-8" if path.endswith(".html") else "application/octet-stream"
    # ★ 网页不缓存：否则改了页面、浏览器还在跑旧的 JS（按钮对不上就会报错）
    head = ("HTTP/1.1 200 OK\r\nConnection: close\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate\r\n"
            "Pragma: no-cache\r\nExpires: 0\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n\r\n" % (ctype, len(body)))
    writer.write(head.encode() + body)
    await writer.drain()

async def handle_http(reader, writer):
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=10)
        if not line:
            writer.close()
            return
        parts = line.decode("latin1").split()
        method, path = (parts + ["", ""])[0], (parts + ["", ""])[1]
        # 吃掉请求头（★ M3 起要读 POST body，所以把头部存下来拿 Content-Length）
        hdrs = {}
        while True:
            h = await asyncio.wait_for(reader.readline(), timeout=10)
            if not h or h in (b"\r\n", b"\n"):
                break
            try:
                k, _, v = h.decode("latin1").partition(":")
                hdrs[k.strip().lower()] = v.strip()
            except Exception:
                pass
        body = b""
        try:
            clen = int(hdrs.get("content-length") or 0)
        except Exception:
            clen = 0
        if clen > 0:
            if clen > 4 * 1024 * 1024:
                await http_json(writer, {"ok": False, "error": "body too large"}, 400)
                return
            body = await asyncio.wait_for(reader.readexactly(clen), timeout=15)

        path_only, _, query = path.partition("?")
        qs = {}
        for kv in query.split("&"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                qs[k] = v

        # ---- / ---- 监控页面
        if path_only in ("/", "/index.html"):
            await serve_static(writer, WEB_PAGE)
            return

        # ---- /webrtc  WebRTC(H.264) 入口：同一个监控页，自动切到 WebRTC ----
        if path_only in ("/webrtc", "/webrtc/"):
            await serve_static(writer, WEB_PAGE)
            return

        # ---- /api/devices ----
        if path_only == "/api/devices":
            # ★ 附带 "now"（中转服务器墙钟，epoch 秒）：客户端可拿它做时钟偏移估计
            await http_json(writer, {"devices": [d.info() for d in DEVICES.values()],
                                     "count": len(DEVICES), "now": now()})
            return

        # ---- GET /api/time   中转墙钟（板端没 RTC 电池，靠它校时）----
        if path_only == "/api/time":
            await http_json(writer, time_info())
            return

        # ---- GET /api/weather   天气代理（10 分钟缓存；板端只问中转）----
        if path_only == "/api/weather":
            await http_json(writer, get_weather())
            return

        # ---- GET /api/status   简化状态（设备在线 + 通话中 + 最近开门）----
        if path_only == "/api/status":
            devq = qs.get("dev") or ""
            d = pick_device(devq)
            did = d.dev_id if d else (devq or "")
            await http_json(writer, {
                "ok": True,
                "now": now(),
                "dev": d.dev_id if d else None,
                "online": bool(d and (now() - d.last_hb) < HEARTBEAT_TIMEOUT),
                "fps": d.fps() if d else 0,
                "audio_fmt": getattr(d, "audio_fmt", None) if d else None,
                "call": (CUR_CALL.get(did) if did else None),
                "callstate": cs_public(did) if did else None,
                "talk_down_bytes": TALK_STATS["down_bytes"],
                "talk_down_pkts": TALK_STATS["down_pkts"],
            })
            return

        # ================= ★ 按需推流：呼叫状态 API =================
        # ---- GET /api/callstate       所有设备的呼叫状态 ----
        if path_only in ("/api/callstate", "/api/callstate/"):
            items = [cs_public(k) for k in list(CALL_STATES.keys())]
            await http_json(writer, {"now": now(), "count": len(items), "calls": items})
            return

        # ---- GET /api/callstate/<dev>     单台设备（JSON）----
        # ---- GET /api/callstate/<dev>.txt 单台设备（纯文本，给板端 busybox 用）----
        if path_only.startswith("/api/callstate/"):
            tail = path_only[len("/api/callstate/"):].strip("/")
            want_txt = tail.endswith(".txt")
            if want_txt:
                tail = tail[:-4]
            dev_id = tail.split("/")[0]
            if not dev_id:
                await http_json(writer, {"error": "bad dev"}, 400)
                return
            if want_txt:
                await http_text(writer, cs_get(dev_id).get("state") or "idle")
            else:
                await http_json(writer, cs_public(dev_id))
            return

        # ---- POST /api/call/start?dev=X   访客按呼叫 → ringing ----
        if path_only in ("/api/call/start", "/api/call/start/"):
            dev_id = qs.get("dev") or ""
            if not dev_id and body:
                try:
                    dev_id = json.loads(body.decode("utf-8", "replace")).get("dev") or ""
                except Exception:
                    dev_id = ""
            if not dev_id:
                await http_json(writer, {"ok": False, "error": "dev required"}, 400)
                return
            st = call_ring(dev_id, source=qs.get("src") or "board")
            await http_json(writer, {"ok": True, "callstate": cs_public(dev_id),
                                     "call": CUR_CALL.get(dev_id)})
            return

        # ---- POST /api/call/<dev>/accept|reject|hangup ----
        if path_only.startswith("/api/call/") and path_only.count("/") >= 4:
            tail = path_only[len("/api/call/"):].strip("/")
            bits = tail.split("/")
            if len(bits) >= 2:
                dev_id, action = bits[0], bits[1]
                if action == "accept":
                    st, why = call_accept(dev_id)
                elif action == "reject":
                    st, why = call_reject(dev_id)
                elif action == "hangup":
                    st, why = call_hangup(dev_id, reason=qs.get("reason") or "hangup")
                else:
                    await http_json(writer, {"ok": False, "error": "unknown action %s" % action}, 404)
                    return
                ok = st is not None
                await http_json(writer, {"ok": ok, "why": why, "dev": dev_id,
                                         "state": (st or cs_get(dev_id)).get("state"),
                                         "callstate": cs_public(dev_id)}, 200 if ok else 409)
                return

        # ---- /whep/<dev>   WebRTC 信令代理（https 页面也能用，见 proxy_whep）----
        if path_only.startswith("/whep/"):
            dev = path_only[len("/whep/"):].split("/")[0]
            await proxy_whep(dev, method, body,
                             hdrs.get("content-type", "application/sdp"), writer)
            return
        if path_only == "/whep" and qs.get("dev"):
            await proxy_whep(qs["dev"], method, body,
                             hdrs.get("content-type", "application/sdp"), writer)
            return

        # ---- POST /api/talk   住户终端上传麦克风 PCM16(16k mono) ----
        #      body = 裸 PCM 字节（application/octet-stream），服务端按 20ms 切片
        #      转发给板端 T_AUDIO_DOWN → 喇叭出声（全双工上行/下行同时跑）
        if path_only == "/api/talk":
            dev = pick_device(qs.get("dev", ""))
            if not dev:
                await http_json(writer, {"ok": False, "error": "device not connected"}, 404)
                return
            if not body:
                await http_json(writer, {"ok": False, "error": "empty body"}, 400)
                return
            sent = await dev.push_down_audio(body)
            _call_touch(dev.dev_id)
            await http_json(writer, {"ok": True, "sent": sent,
                                     "total_down": TALK_STATS["down_bytes"],
                                     "dev": dev.dev_id})
            return

        # ---- POST /api/door   开门（nonce + ts + HMAC-SHA256，防重放）----
        #      body: {"dev","nonce","ts","sig","by"}
        if path_only == "/api/door":
            try:
                req = json.loads(body.decode("utf-8", "replace") or "{}")
            except Exception:
                req = {}
            dev_id = req.get("dev") or qs.get("dev") or ""
            dev = pick_device(dev_id)
            if not dev:
                await http_json(writer, {"ok": False, "error": "device not connected"}, 404)
                return
            ok, why = door_verify(dev.dev_id, str(req.get("nonce") or ""),
                                  req.get("ts"), str(req.get("sig") or ""))
            if not ok:
                log("[door] %s 开门请求被拒: %s" % (dev.dev_id, why))
                await http_json(writer, {"ok": False, "error": why}, 403)
                return
            ok2 = await send_control(dev, "open")
            c = CUR_CALL.get(dev.dev_id) or call_start(dev.dev_id, source="door")
            c["door"].append({"at": round(now(), 1), "at_iso": _fmt_ts(now()),
                              "by": req.get("by") or "web"})
            c["last_act"] = round(now(), 1)
            _save_calls()
            log("[door] %s 开门成功（来源=%s，本通电话第 %d 次）"
                % (dev.dev_id, c["door"][-1]["by"], len(c["door"])))
            await http_json(writer, {"ok": bool(ok2), "dev": dev.dev_id,
                                     "door_count": len(c["door"]),
                                     "sent_to_device": bool(ok2),
                                     "extra": {"verify": why}})
            return

        # ---- GET /api/calls   通话记录（保留；中转唯一落盘的东西）----
        if path_only == "/api/calls":
            try:
                limit = int(qs.get("limit", "50") or 50)
            except Exception:
                limit = 50
            items = [dict(x) for x in CALLS[-limit:]]
            items.reverse()
            await http_json(writer, {"count": len(items), "calls": items,
                                     "active": {k: dict(v) for k, v in CUR_CALL.items()},
                                     "callstates": {k: cs_public(k) for k in CALL_STATES}})
            return

        # ---- /live/<dev>.mjpg ----
        if path_only.startswith("/live/") and path_only.endswith(".mjpg"):
            dev_id = path_only[len("/live/"):-len(".mjpg")]
            dev = DEVICES.get(dev_id)
            if not dev:
                await http_json(writer, {"error": "device %s not connected" % dev_id}, 404)
                return
            await stream_mjpeg(dev, writer)
            return

        # ---- /live/<dev>.alaw ----
        if path_only.startswith("/live/") and path_only.endswith(".alaw"):
            dev_id = path_only[len("/live/"):-len(".alaw")]
            dev = DEVICES.get(dev_id)
            if not dev:
                await http_json(writer, {"error": "device %s not connected" % dev_id}, 404)
                return
            await stream_alaw(dev, writer)
            return

        # ---- /snapshot/<dev>.jpg ----
        if path_only.startswith("/snapshot/") and path_only.endswith(".jpg"):
            dev_id = path_only[len("/snapshot/"):-len(".jpg")]
            dev = DEVICES.get(dev_id)
            if not dev or not dev.latest_jpeg:
                await http_json(writer, {"error": "no frame for %s" % dev_id}, 404)
                return
            jpg = dev.latest_jpeg
            head = ("HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\n"
                    "Content-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpg))
            writer.write(head.encode() + jpg)
            await writer.drain()
            return

        await http_json(writer, {"error": "unknown path %s" % path_only}, 404)
    except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError, asyncio.TimeoutError):
        pass
    except Exception as e:
        log("[http] 处理异常 %r" % e)
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

# =============================================================================
# main
# =============================================================================
WEB_PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "index.html")

async def main():
    global WEB_PAGE
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp-port", type=int, default=9100)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--ffmpeg", default=None, help="ffmpeg 路径（默认自动查找，仅转码发布用）")
    ap.add_argument("--web", default=WEB_PAGE)
    # ★ 对讲：浏览器只在安全上下文（https/localhost）允许取麦克风，
    #   所以额外开一个 HTTPS 端口专门给"住户终端 + 对讲上行"用。
    ap.add_argument("--tls-port", type=int, default=0,
                    help="HTTPS 端口（0=关）。开了以后 https://<ip>:<port>/ 才能用麦克风对讲")
    ap.add_argument("--tls-cert", default="certs/server.crt")
    ap.add_argument("--tls-key", default="certs/server.key")
    a = ap.parse_args()

    WEB_PAGE = a.web
    # ★ M7：通话记录落盘（重启不丢）。★ 2026-10-20：除 calls.json 外不写任何文件。
    _load_calls()
    global FFMPEG
    if a.ffmpeg:
        FFMPEG = a.ffmpeg
    # ★ 直播转码发布：SC_PUBLISH=1 时开启（默认关闭，出问题不影响原有功能）
    global PUBLISH
    if os.environ.get("SC_PUBLISH") == "1":
        if live_publish.ffmpeg_available(FFMPEG or "ffmpeg"):
            _out = os.environ.get("SC_PUBLISH_OUT", "")
            if not _out:
                _out = os.environ.get("SC_RTSP", "") or "rtsp://127.0.0.1:8554"
            PUBLISH = live_publish.PublishManager(
                enabled=True,
                out_target=_out,
                ffmpeg=FFMPEG or "ffmpeg",
                fps=int(os.environ.get("SC_PUBLISH_FPS", "30")),
                video_bitrate=os.environ.get("SC_PUBLISH_BR", "4M"),
                audio_bitrate=os.environ.get("SC_PUBLISH_ABR", "64k"),
                scale=os.environ.get("SC_PUBLISH_SCALE", ""),
                log=log,
                # ★ 一律用关键字：之前按位置传，把 preset 传成了 vsrc_tpl 直接报错
                vsrc_tpl=os.environ.get("SC_PUBLISH_VSRC",
                                        "http://127.0.0.1:8080/live/%s.mjpg"),
                preset=os.environ.get("SC_PUBLISH_PRESET", "ultrafast"),
                profile=os.environ.get("SC_PUBLISH_PROFILE", ""),
                bufsize=os.environ.get("SC_PUBLISH_BUFSIZE", ""),
                # ★ Phase1 调优：CRF + 上限码率（SC_PUBLISH_CRF 为空则退回固定码率 ABR）
                crf=os.environ.get("SC_PUBLISH_CRF", ""),
                x264_params=os.environ.get("SC_PUBLISH_X264", ""),
            )
            log("[发布] 已开启 H.264/AAC → %s" % _out)
            log("[发布] 对外播放入口：网页 MJPEG (/) 与 WebRTC (MediaMTX :8889)；"
                "RTSP 只是内部发布通道")
        else:
            log("[发布] 系统里没有 ffmpeg，跳过直播转码")

    tcp = await asyncio.start_server(handle_device, "0.0.0.0", a.tcp_port)
    http = await asyncio.start_server(handle_http, "0.0.0.0", a.http_port)

    # ★ HTTPS（可选）：给"住户终端对讲上行"用（浏览器安全上下文要求）
    https = None
    if a.tls_port > 0:
        try:
            import ssl
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(a.tls_cert, a.tls_key)
            https = await asyncio.start_server(handle_http, "0.0.0.0", a.tls_port, ssl=ctx)
            log("  对讲(https)   : https://<本机IP>:%d/   （浏览器只在 https 下允许麦克风）"
                % a.tls_port)
        except Exception as e:
            log("[tls] HTTPS 端口启动失败（继续只用 http）: %r" % e)
            https = None
    log("流媒体中转服务已启动（★ 按需推流：平时无流，接听后才有）")
    log("  设备推流入口 : tcp://0.0.0.0:%d" % a.tcp_port)
    log("  Web/后台 API : http://0.0.0.0:%d" % a.http_port)
    log("  监控页面     : http://<本机IP>:%d/" % a.http_port)
    log("  呼叫状态 API : GET /api/callstate/<dev> | POST /api/call/start?dev=<dev>"
        " | POST /api/call/<dev>/accept|reject|hangup")
    log("  响铃超时     : %.0fs  单通最长 %.0fs  会话态保留 %.0fs"
        % (CALL_RING_TIMEOUT, CALL_MAX_SEC, CALL_LINGER))
    log("  录像/留影    : 已全部删除（中转不写任何音视频文件；只保留 calls.json 通话记录）")

    # ★ 呼叫状态机定时器：
    #   1) ringing 超过 SC_RING_TIMEOUT → timeout（板端据此提示"无人接听"）
    #   2) rejected/timeout 保留 SC_CALL_LINGER 秒后回 idle（板端/网页都读到过就能收尾）
    #   3) accepted 超过 SC_CALL_MAX_SEC → 自动挂断（板子/网页异常退出时兜底）
    #   4) accepted 但设备 20 秒没连上来（板端推流没起来）→ 挂断，避免状态卡死
    async def call_janitor():
        while True:
            try:
                t = now()
                for dev_id, st in list(CALL_STATES.items()):
                    s = st.get("state")
                    since = st.get("since") or t
                    if s == "ringing":
                        rs = st.get("ring_start") or since
                        if t - rs > CALL_RING_TIMEOUT:
                            call_end(dev_id, reason="timeout")
                            cs_set(dev_id, "timeout", reason="响铃 %.0fs 无人接听" % CALL_RING_TIMEOUT)
                    elif s in ("rejected", "timeout"):
                        if t - since > CALL_LINGER:
                            cs_set(dev_id, "idle", reason="会话态复位")
                    elif s == "accepted":
                        if t - since > CALL_MAX_SEC:
                            call_hangup(dev_id, reason="max-duration")
                            continue
                        # 接听后 20 秒还没等到板端推流（av_stream 没起来）→ 收尾，
                        # 避免状态卡在 accepted 让板端一直以为在通话。
                        # ★ 用"接听时刻起算"的时间判断，不要用计数器：计数器在
                        #   上一通电话里累加过，会把新一通电话秒杀（2026-10-20 踩到）。
                        if dev_id not in DEVICES:
                            t_ans = st.get("answered_at") or since
                            if t - t_ans > 20:
                                call_hangup(dev_id, reason="device-not-streaming")
            except Exception as e:
                log("[callstate] janitor 异常: %r" % e)
            await asyncio.sleep(1)
    asyncio.create_task(call_janitor())

    # 心跳超时检测（设备掉线时把在线状态标出来）
    async def reaper():
        while True:
            for dev in list(DEVICES.values()):
                if now() - dev.last_hb > HEARTBEAT_TIMEOUT:
                    if dev.status.get("state") != "offline":
                        dev.status["state"] = "offline"
                        log("[watch] %s 心跳超时（>%.0fs），标记离线" % (dev.dev_id, HEARTBEAT_TIMEOUT))
            await asyncio.sleep(1)
    asyncio.create_task(reaper())

    async with tcp, http:
        if https is not None:
            async with https:
                await asyncio.gather(tcp.serve_forever(), http.serve_forever(),
                                     https.serve_forever())
        else:
            await asyncio.gather(tcp.serve_forever(), http.serve_forever())

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("服务退出")
