#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
live_publish.py —— 把设备的 MJPEG(+PCM16/A-law) 实时转成 H.264+Opus 推给 MediaMTX

为什么放在中转服务（虚拟机）而不是板子：
    实测板子（3×Cortex-A7，无硬件编码器）软编 x264：
      1080p@15 → 60 秒都编不完 10 秒素材（<2.5fps）
      720p@15  → 10 秒素材要 27.8 秒（5.4fps）
    即"在板子上实时编 H.264"物理上做不到；而虚拟机 CPU 富余，
    于是板子继续 MJPEG 直通（CPU ~3%），由中转服务转码：
        MJPEG+PCM16 → H.264+Opus → MediaMTX → WebRTC（唯一对外播放入口）

      由它对外出 WebRTC）。RTSP 是内部发布通道。

    ★ 音频：设备上报 16bit PCM（S16_LE）时 ffmpeg 音频输入用 -f s16le；
      设备上报 alaw 时自动回退 -f alaw。

实现方式（复用已验证的导出写法）：
    给每台设备开两个 FIFO：/tmp/sc_pub_<dev>.mjpg（连帧 JPEG）与 .alaw（音频）
    中转服务收到数据就写进 FIFO；ffmpeg 以双输入读取 → 编码 → rtsp 推给 MediaMTX
"""
import os
import shutil
import stat
import subprocess
import threading
import time

FIFO_DIR = "/tmp"

# ★ 默认 x264 参数（Phase 1 画质-码率曲线调优 + Phase 2 延迟优化，见 docs/13）：
#   在保持"低延迟三件套"（zerolatency + bframes=0 + 无 lookahead）的前提下，加
#   aq-mode=3（自适应量化偏向平坦区，暗场景/墙面不糊）、psy-rd（心理视觉锐度）、
#   deblock=1,1、weightp=2、mbtree、direct=auto。
#   实测（1920x1080 真实录像 57.4s，SSIM/PSNR 对源）：
#     同码率 4446kbps：SSIM_Y 0.9318（原参数需 4741kbps）→ 等画质省 6.2% 码率
#     编码耗时 23.4s/57.4s = 0.41× 实时（原参数 25.1s = 0.44×，反而更快）
#
#   ★ sliced-threads=1（Phase 2 实测后改的）：原来写 sliced-threads=0，x264 就会用
#     "帧级多线程"（4 核 → 同时压 5 帧），编码器内部因此**多攒 3~4 帧才输出**，
#     实测多出 ~200ms 延迟（中转入队→MediaMTX 出口 249ms，其中转码占绝大部分）。
#     改成切片级多线程后每帧独立成片，延迟回到单帧量级（实测见 docs/13 Phase 2）。
DEFAULT_X264_PARAMS = ("sync-lookahead=0:rc-lookahead=0:bframes=0:sliced-threads=1:"
                       "aq-mode=3:psy-rd=1.0,0.15:deblock=1,1:weightp=2:mbtree=1:direct=auto")

class DevicePublisher:
    """一台设备的转码推流器（含 FIFO 写入与 ffmpeg 进程守护）"""

    def __init__(self, dev, out_target, ffmpeg="ffmpeg", fps=15, video_bitrate="2M",
                 audio_bitrate="64k", width=0, height=0, log=print,
                 vsrc_tpl="http://127.0.0.1:8080/live/%s.mjpg",
                 preset="ultrafast", profile="", bufsize="", audio_rate=16000,
                 audio_fmt="s16le", crf="", x264_params=""):
        self.dev = dev
        self.rtsp = "%s/%s" % (str(out_target).rstrip("/"), dev)
        self.ffmpeg = ffmpeg
        self.fps = max(1, int(fps))
        self.vbr = video_bitrate
        self.abr = audio_bitrate
        self.audio_rate = int(audio_rate or 16000)   # ★ 音频采样率（与设备一致）
        self.audio_fmt = audio_fmt or "s16le"        # ★ s16le(PCM16) / alaw（与设备一致）
        self.preset = preset or "ultrafast"
        self.profile = profile or ""            # 例如 high / main；空=ffmpeg 默认
        self.bufsize = bufsize or video_bitrate
        # ★ crf 非空 → 用"CRF + 上限码率(maxrate)"；否则用固定码率 ABR
        self.crf = str(crf or "").strip()
        self.x264_params = x264_params or DEFAULT_X264_PARAMS
        self.width = width
        self.height = height
        self.log = log
        self.vsrc = vsrc_tpl % dev       # ★ 视频输入：标准 multipart MJPEG（避免绿屏）

        self.vfifo = os.path.join(FIFO_DIR, "sc_pub_%s.mjpg" % dev)
        self.afifo = os.path.join(FIFO_DIR, "sc_pub_%s.alaw" % dev)
        self.proc = None
        self.vfd = None
        self.afd = None
        self.lock = threading.Lock()
        self.running = False
        self.thread = None
        self.published = 0          # 写进 FIFO 的视频帧数
        self.logf = None
        self.errfd = None
        self.last_error = ""
        self.started_at = 0

    # ---------------- FIFO 与进程 ----------------
    def _make_fifos(self):
        for p in (self.vfifo, self.afifo):
            if os.path.exists(p):
                if not stat.S_ISFIFO(os.stat(p).st_mode):
                    os.remove(p)
            if not os.path.exists(p):
                os.mkfifo(p, 0o666)

    def _cmd(self):
        afmt = "s16le" if self.audio_fmt == "s16le" else "alaw"   # ★ 设备发的是 PCM16
        c = [self.ffmpeg, "-hide_banner", "-loglevel", "warning",
             "-fflags", "nobuffer", "-flags", "low_delay",
             "-avioflags", "direct",                 # 少一层 IO 缓冲
             "-probesize", "1000000", "-analyzeduration", "1000000",
             "-use_wallclock_as_timestamps", "1",
             "-thread_queue_size", "512",            # 防丢帧：队列太小会出现 Thread message queue blocking
             "-framerate", str(self.fps), "-f", "mjpeg", "-i", self.vsrc,
             "-thread_queue_size", "512",            # 音频同样加大队列
             "-f", afmt, "-ar", str(self.audio_rate), "-ac", "1", "-i", self.afifo,
             "-c:v", "libx264", "-preset", self.preset, "-tune", "zerolatency",
             "-g", str(self.fps),                    # ★ 1 秒一个关键帧（原来 2 秒）
             "-x264-params", self.x264_params,        # ★ 调优后的 x264 参数（见文件头注释）
             "-pix_fmt", "yuv420p"]
        if self.crf:
            # ★ CRF 模式：质量恒定、码率随画面复杂度浮动，用 -maxrate 兜住峰值
            c += ["-crf", self.crf, "-maxrate", self.vbr, "-bufsize", self.bufsize]
        else:
            c += ["-b:v", self.vbr, "-maxrate", self.vbr, "-bufsize", self.bufsize]
        if self.profile:
            c += ["-profile:v", self.profile]
        if self.width and self.height:                       # 需要时顺便降分辨率
            c += ["-vf", "scale=%d:%d" % (self.width, self.height)]
        c += ["-af", "highpass=f=100,lowpass=f=6000,afftdn=nr=14:nf=-35",
              "-c:a", "libopus", "-b:a", self.abr, "-ar", "48000"]
        c += ["-f", "rtsp", "-rtsp_transport", "udp",   # ★ 本机回环用 UDP，避开 TCP 重传延迟
              "-muxdelay", "0", "-muxpreload", "0", "-max_delay", "0"]
        c += [self.rtsp]
        return c

    def start(self):
        with self.lock:
            if self.running:
                return
            try:
                self._make_fifos()
                self.vfd = os.open(self.vfifo, os.O_RDWR | os.O_NONBLOCK)
                self.afd = os.open(self.afifo, os.O_RDWR | os.O_NONBLOCK)
            except OSError as e:
                self.last_error = "FIFO 打开失败（ffmpeg 还没读）: %s" % e
                return
            try:
                self.logf = os.path.join(FIFO_DIR, "sc_pub_%s.ffmpeg.log" % self.dev)
                self.errfd = open(self.logf, "ab", buffering=0)
                self.proc = subprocess.Popen(self._cmd(), stdout=subprocess.DEVNULL,
                                             stderr=self.errfd)
                self.log("      ffmpeg 命令: %s" % " ".join(self._cmd()))
                self.log("      ffmpeg 日志: %s" % self.logf)
            except Exception as e:
                self.last_error = "启动 ffmpeg 失败: %s" % e
                return
            self.running = True
            self.started_at = time.time()
            self.thread = threading.Thread(target=self._writer, daemon=True)
            self.thread.start()
            self.log("[发布] %s → %s（H.264 %s / AAC %s）" % (self.dev, self.rtsp, self.vbr, self.abr))

    def stop(self):
        self.running = False
        for fd in (self.vfd, self.afd):
            try:
                if fd is not None:
                    os.close(fd)
            except OSError:
                pass
        self.vfd = self.afd = None
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=3)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        self.proc = None

    def alive(self):
        return bool(self.proc) and self.proc.poll() is None

    # ---------------- 写数据 ----------------
    def push_video(self, jpeg: bytes):
        """视频改由 ffmpeg 直接读 /live/<dev>.mjpg，这里只计数"""
        self.published += 1

    def push_audio(self, alaw: bytes):
        if not self.running or self.afd is None:
            return
        try:
            os.write(self.afd, alaw)
        except OSError:
            pass

    def _writer(self):
        """守护线程：ffmpeg 死了就重启（带退避），避免一次失败就永久停掉"""
        backoff = 2
        while self.running:
            time.sleep(3)
            if not self.running:
                break
            if not self.alive():
                self.last_error = "ffmpeg 退出（码 %s）" % (self.proc.poll() if self.proc else "?")
                self.log("[发布] %s 的 ffmpeg 退出，%d 秒后重启：%s"
                         % (self.dev, backoff, self.last_error))
                self.stop_only_proc()
                time.sleep(backoff)
                backoff = min(backoff * 2, 30)
                self._restart()
            else:
                backoff = 2

    def stop_only_proc(self):
        if self.proc:
            try:
                self.proc.kill()
                self.proc.wait(timeout=2)
            except Exception:
                pass
        self.proc = None
        for fd in (self.vfd, self.afd):
            try:
                if fd is not None:
                    os.close(fd)
            except OSError:
                pass
        self.vfd = self.afd = None

    def _restart(self):
        try:
            self._make_fifos()
            self.vfd = os.open(self.vfifo, os.O_RDWR | os.O_NONBLOCK)
            self.afd = os.open(self.afifo, os.O_RDWR | os.O_NONBLOCK)
            self.errfd = open(self.logf, "ab", buffering=0)
            self.proc = subprocess.Popen(self._cmd(), stdout=subprocess.DEVNULL,
                                         stderr=self.errfd)
            self.log("[发布] %s 的 ffmpeg 已重启" % self.dev)
        except Exception as e:
            self.last_error = "重启失败: %s" % e

class PublishManager:
    """按设备管理发布器；由 stream_server 在收到设备数据时调用"""

    def __init__(self, enabled=False, out_target="rtsp://127.0.0.1:8554",
                 ffmpeg="ffmpeg", fps=15, video_bitrate="2M", audio_bitrate="64k",
                 scale="", log=print,
                 vsrc_tpl="http://127.0.0.1:8080/live/%s.mjpg",
                 preset="ultrafast", profile="", bufsize="", audio_rate=16000,
                 audio_fmt="s16le", crf="", x264_params=""):
        self.enabled = enabled
        self.out_target = out_target
        self.ffmpeg = ffmpeg
        self.fps = fps
        self.vbr = video_bitrate
        self.abr = audio_bitrate
        self.scale = scale
        self.log = log
        self.vsrc_tpl = vsrc_tpl
        self.preset = preset
        self.profile = profile
        self.bufsize = bufsize
        self.audio_rate = int(audio_rate or 16000)
        self.audio_fmt = audio_fmt or "s16le"
        self.crf = str(crf or "").strip()
        self.x264_params = x264_params or DEFAULT_X264_PARAMS
        self.pubs = {}
        self.lock = threading.Lock()

    def _wh(self):
        if not self.scale or "x" not in self.scale:
            return 0, 0
        try:
            w, h = self.scale.lower().split("x")
            return int(w), int(h)
        except Exception:
            return 0, 0

    def feed_video(self, dev, jpeg: bytes):
        if not self.enabled:
            return
        p = self.pubs.get(dev)
        if p is None:
            with self.lock:
                p = self.pubs.get(dev)
                if p is None:
                    w, h = self._wh()
                    p = DevicePublisher(dev, self.out_target, self.ffmpeg, self.fps,
                                        self.vbr, self.abr, w, h, self.log, self.vsrc_tpl,
                                        self.preset, self.profile, self.bufsize, self.audio_rate,
                                        self.audio_fmt, self.crf, self.x264_params)
                    self.pubs[dev] = p
            p.start()
        p.push_video(jpeg)

    def feed_audio(self, dev, alaw: bytes):
        if not self.enabled:
            return
        p = self.pubs.get(dev)
        if p:
            p.push_audio(alaw)

    def set_audio_rate(self, dev, rate):
        """设备上报采样率后同步给发布器（发布器通常还没启动，先记下来）"""
        try:
            self.audio_rate = int(rate or 16000)
        except Exception:
            pass
        p = self.pubs.get(dev)
        if p is not None:
            p.audio_rate = self.audio_rate

    def set_audio_fmt(self, dev, fmt):
        """设备上报音频格式（s16le / alaw）后同步给发布器；格式变了要重起 ffmpeg"""
        fmt = "s16le" if str(fmt).lower() in ("s16le", "pcm", "pcm16") else "alaw"
        self.audio_fmt = fmt
        p = self.pubs.get(dev)
        if p is not None and p.audio_fmt != fmt:
            self.log("[发布] %s 音频格式变化 %s → %s，重启 ffmpeg"
                     % (dev, p.audio_fmt, fmt))
            p.audio_fmt = fmt
            p.stop_only_proc()       # 守护线程会在 3 秒内用新参数重启

    def status(self):
        out = []
        for dev, p in self.pubs.items():
            out.append({"dev": dev, "rtsp": p.rtsp, "running": p.alive(),
                        "frames": p.published, "error": p.last_error,
                        "uptime": int(time.time() - p.started_at) if p.started_at else 0})
        return out

    def stop_dev(self, dev):
        """设备断开（挂断/停流）时把该设备的发布器彻底停掉并移除。

        ★ 2026-10-20 按需推流：不这么做的话，设备停止推流后 ffmpeg 会一直
          "读不到 MJPEG → 退出 → 守护线程重启 → 再读不到" 地空转（每次都要
          新起一个 ffmpeg 进程），既浪费 CPU 又刷日志。现在只在通话期间存在
          发布器；挂断即销毁，下次通话重新创建。
        """
        p = None
        with self.lock:
            p = self.pubs.pop(dev, None)
        if p is None:
            return False
        try:
            p.stop()
        except Exception as e:
            self.log("[发布] 停止 %s 失败: %r" % (dev, e))
        self.log("[发布] %s 已停止发布（设备断流）" % dev)
        return True

    def stop_all(self):
        for p in list(self.pubs.values()):
            p.stop()
        self.pubs.clear()

def ffmpeg_available(ffmpeg="ffmpeg"):
    return shutil.which(ffmpeg) is not None
