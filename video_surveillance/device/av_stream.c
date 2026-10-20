/*=============================================================================
 * av_stream.c —— RK3506B「门禁可视对讲」设备端（门口机）
 *
 * 职责（严格按需求边界）：
 *   1) 采集：V4L2 抓 USB 摄像头画面；ALSA 抓麦克风/板载声卡音频
 *   2) 编码：视频优先用摄像头自带的 MJPEG（UVC 摄像头一般支持，几乎不耗 CPU）；
 *            不支持时退化为 YUYV + libturbojpeg 软编 MJPEG
 *            音频用 16bit PCM（S16_LE 原始直传，不再做 G.711 A-law 有损压缩）
 *   3) 推流：把编码后的帧通过 TCP 主动推给流媒体中转服务（自定义 TLV 帧）
 *   4) 指令：同一条 TCP 连接上接收后台指令 start / stop / open（开门）
 *            ★ M3：新增 T_AUDIO_DOWN(6) —— 中转把住户终端的语音送下来，
 *              这里用 ALSA 播放到 ES8323 喇叭，实现**下行对讲**
 *            ★ M4：下行 PCM 同时作为 far-end 参考喂给 libspeexdsp 的 AEC，
 *              在麦克风端做回声消除，实现**全双工**（--talk half 可回退半双工）
 *   5) 状态：每秒心跳 + 摄像头/声卡插拔检测 + 网络断开自动重连（指数退避）
 *   6) 本机 HTTP(:8081)：/live.mjpg /snap.jpg 之外新增
 *        GET /door   最近一次开门事件（Qt 每 0.5s 轮询后弹窗）
 *        GET /set?mic=0|1&spk=0|1   静音/免提（Qt 对讲页的按钮）
 *        GET /mode?fps=30           运行时改推流帧率（混合推流模式 C，见下）
 *
 *   ★ 混合推流模式 C（待机低帧率 / 呼叫全速）：
 *       门禁 99% 时间没人呼叫，常开 1080p@30fps 白烧带宽与 CPU。
 *       所以开机默认走 **idle 帧率**（--idle-fps，默认 5fps），
 *       Qt 打开「可视对讲」页时 GET /mode?fps=30 → 切全速；
 *       返回首页时 GET /mode?fps=5 → 切回待机。
 *       摄像头本身固定 30fps 出图，低帧率靠 video_thread 里的**软件帧率闸门**
 *       丢帧实现（和 CPU 保护降级用的是同一套机制），所以**不需要重开摄像头**，
 *       切换是毫秒级、无中断的。
 *
 * 明确不做（按边界约束）：
 *   ✗ 不做音视频解码（解码在 Web 客户端）
 *   ✗ 不做录像存储（录像/留影在流媒体服务侧）
 *   ✗ 不驱动 GPIO/继电器开门（只记录事件，弹窗提示）
 *
 * 编译（用 SDK 的 buildroot 工具链）：
 *   arm-buildroot-linux-gnueabihf-gcc -O2 -o av_stream av_stream.c \
 *        -ljpeg -lturbojpeg -lasound -lspeexdsp -lpthread -lm
 *
 * 运行：
 *   ./av_stream --server 192.168.137.94:9100 --dev /dev/video0 \
 *               --audio plughw:0,0 --audio-out plughw:0,0 --talk full
 *   ./av_stream --synthetic                       # 无摄像头时用测试图案自检
 *   ./av_stream --list-audio                      # 列出采集声卡
 *===========================================================================*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <linux/videodev2.h>

#include <jpeglib.h>
#include <turbojpeg.h>

#include <alsa/asoundlib.h>

/* ★ M4：回声消除（libspeexdsp 的 MDF 自适应滤波器，A7 上开销很小） */
#include <speex/speex_echo.h>
#if defined(HAVE_SPEEX_PREPROCESS)
#include <speex/speex_preprocess.h>
#endif

/*===========================================================================
 * 1. 协议定义（必须和 server/stream_server.py 一致）
 *=========================================================================*/
#define PKT_MAGIC       "SCAV"
#define PKT_HDR_LEN     12                       /* magic4 + type1 + flags1 + seq2 + len4 */
#define PKT_VIDEO_JPEG  1
#define PKT_AUDIO_ALAW  2                        /* 音频包；载荷是 S16_LE PCM16（常量名沿用旧名，
                                                  * 真实格式以 hello 的 audio 字段为准：pcm16k */
#define PKT_STATUS      3                        /* JSON，设备→服务 */
/* ★ 2026-10-20：PKT_EVENT(4) 已删除 —— 板端不再上报任何 T_EVENT（移动侦测整块删除）。
 *   开门事件仍记录在本机（:8081/door），由 Qt 轮询弹窗；摄像机插拔只写本地日志。 */
#define PKT_CONTROL     5                        /* JSON，服务→设备（start/stop/open） */

/* ★ M3：下行音频（服务→设备）。载荷 = PCM16 S16_LE、16kHz、单声道，
 *   每包 20ms = 320 采样 = 640 字节（和上行同规格，方便直接喂 AEC）。 */
#define PKT_AUDIO_DOWN  6

#define MAX_PKT         2 * 1024 * 1024          /* 单帧上限 2MB */
#define VIDEO_QUEUE     8                        /* 视频帧队列（满则丢最旧的，保低延迟） */
#define AUDIO_QUEUE     64                       /* 音频包队列 */

struct pkt {
    int   type;
    int   len;
    unsigned char *data;
};

/*===========================================================================
 * 2. 全局配置与状态
 *=========================================================================*/
static struct {
    const char *server;          /* host:port */
    const char *video_dev;       /* /dev/video0 */
    const char *audio_dev;       /* hw:1,0，可为 NULL=无音频 */
    const char *device_id;       /* 上报给后台的设备标识 */
    int   width, height, fps;
    int   idle_fps;              /* ★ 模式C：待机帧率（开机默认用它，默认 5，0=关掉该特性）*/
    int   call_fps;              /* ★ 模式C：呼叫帧率（/mode?fps=call 用，默认 30）*/
    int   synthetic;             /* 1=无摄像头时用测试图案 */
    int   list_audio;
    int   quality;               /* JPEG 质量（软编时用） */
    const char *ctrls;           /* --set-ctrl "sharpness=3,contrast=10" 图像参数 */
    int   cpu_high;              /* CPU 超过它 → 自动降级（默认 90）*/
    int   cpu_low;               /* CPU 低于它 → 恢复（默认 60）*/
    int   fps_low;               /* 降级后的帧率（默认 8）*/
    int   cpu_guard;             /* 1=启用 CPU 保护（默认开）*/
    int   audio_gate_db;         /* 音频电平门限(dB)，低于它视为"没插麦克风"→ 发静音 */
    int   audio_gate;            /* 1=启用音频门限（默认开）*/
    int   audio_rate;            /* 采集采样率（默认 16000；PCM16 每采样 2 字节）*/
    int   http_port;             /* 本机只读预览端口（MJPEG），0=关闭，默认 8081 */

    /* --- 采集时间戳 OSD（B）--- */
    int   osd;                   /* 1=把采集时刻画进画面（默认 off，1080p 重编码很贵）*/
    int   osd_interval;          /* 每 N 帧叠一次（默认 30；1=逐帧）*/

    /* --- ★ M3/M4：对讲（下行播放 + 回声消除） --- */
    const char *audio_out;       /* 播放设备，例 plughw:0,0；NULL=不做出声（只上行） */
    int   talk_full;             /* 1=全双工(full, 默认)  0=半双工(half) */
    int   aec;                   /* 1=启用 AEC（默认 on；half 模式强制 off） */
    int   aec_tail_ms;           /* 自适应滤波器尾长(ms)，默认 200（能覆盖 ALSA 缓冲延迟） */
    int   aec_delay_ms;          /* far-end 参考相对 near-end 的固定延迟(ms)，默认 0 */
    int   down_gate_db;          /* 下行噪声门限(dBFS)，默认 -50；<=-99 关闭 */
    int   aec_selftest;          /* >0 = 只跑 AEC 算法自测（合成回声）然后退出 */
} g_cfg = {
    .server    = "127.0.0.1:9100",
    .video_dev = "/dev/video0",
    .audio_dev = NULL,
    .device_id = "rk3506b-01",
    .width     = 640, .height = 480, .fps = 15,
    .idle_fps  = 5, .call_fps = 30,
    .synthetic = 0, .list_audio = 0, .quality = 75,
    .ctrls     = NULL,
    .cpu_high  = 90, .cpu_low = 60, .fps_low = 8, .cpu_guard = 1,
    .audio_gate_db = -25, .audio_gate = 1, .audio_rate = 16000,
    .http_port = 8081,
    .osd = 0, .osd_interval = 30,
    .audio_out = NULL, .talk_full = 1, .aec = 1,
    .aec_tail_ms = 200, .aec_delay_ms = 0,
    .down_gate_db = -50,
};

static volatile int g_running = 1;
static volatile int g_streaming = 1;        /* 后台可以 stop/start 推流 */
/* ★ armhf 上 long 是 32 位：now_ms() 返回"epoch 毫秒"必须用 long long，
 *   否则 2^31 ms ≈ 24.8 天就溢出（更准确地说，任何 1970 年之后的 epoch 毫秒
 *   都放不进 32 位，所以 g_start_time 一赋值就已经是错的）。
 *   send_status() 的 uptime 就是被这个溢出带坏的（既有 bug，本次修掉）。 */
static long long g_start_time;
static unsigned short g_seq = 0;
static unsigned long g_vid_fc = 0;          /* 采集帧序号（抽帧侦测 / OSD 用）*/

/* 统计 */
static volatile unsigned long g_video_frames, g_audio_pkts, g_video_bytes, g_sent_errors;
static volatile int g_video_ok, g_audio_ok;
static volatile int g_cur_w, g_cur_h;        /* 实际协商到的分辨率（上报给后台） */
static volatile int g_hw_fps = 0;            /* 摄像头是否接受了我们设置的帧率 */

/* --- CPU 保护（新增）--- */
static volatile int  g_cpu_percent = -1;     /* 最近一次整机 CPU 占用 */
static volatile int  g_degraded = 0;         /* 1 = 已降级（分辨率/帧率已下调）*/
static volatile int  g_need_reopen = 0;      /* 请求视频线程重开摄像头（换分辨率）*/
static int           g_base_fps = 0;         /* 原始帧率 */
static int           g_base_w = 0, g_base_h = 0;  /* 原始分辨率 */

/* ★ M3/M4/M5 运行期状态（本机 HTTP 客户端线程要读，所以在这里提前声明） */
static volatile int           g_spk_ok = 0;          /* 播放设备就绪 */
static volatile int           g_mic_mute = 0;        /* 静音（上行发 0） */
static volatile int           g_spk_mute = 0;        /* 喇叭关 */
static volatile long long     g_last_down_ms = 0;    /* 最近一次收到下行音频 */
static volatile unsigned long g_down_pkts = 0;       /* 收到的下行包数 */
static volatile unsigned long g_down_bytes = 0;
static volatile unsigned long g_play_samples = 0;    /* 写进 ALSA 的采样点 */
static volatile unsigned long g_down_starve = 0;     /* 播放线程"等不到下行数据"的次数 */
static volatile unsigned long g_xrun = 0;            /* 播放 underrun 恢复次数 */
static volatile int           g_down_water = 0;      /* 下行环形缓冲水位（采样点），取样用 */
static volatile int           g_short_write_logged = 0;
static volatile unsigned long g_door_count = 0;      /* 开门累计次数 */
static volatile long long     g_door_ts = 0;         /* 最近一次开门 epoch 秒 */
static SpeexEchoState        *g_aec = NULL;          /* AEC 状态（NULL=关） */

/* 用 /proc/stat 两次采样算整机 CPU 占用（返回 -1 表示还没基准） */
static int cpu_read_percent(void)
{
    static unsigned long long prev_idle = 0, prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    unsigned long long u = 0, ni = 0, sy = 0, id = 0, wa = 0, hi = 0, si = 0, st = 0;
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &u, &ni, &sy, &id, &wa, &hi, &si, &st) < 4)
        return -1;
    unsigned long long idle = id + wa;
    unsigned long long total = u + ni + sy + id + wa + hi + si + st;
    if (prev_total == 0) { prev_idle = idle; prev_total = total; return -1; }
    unsigned long long dt = total - prev_total, di = idle - prev_idle;
    prev_idle = idle; prev_total = total;
    if (dt == 0) return -1;
    return (int)(100.0 * (double)(dt - di) / (double)dt);
}

/* 日志（要放在下面 apply_ctrls 等函数之前） */
#define LOG(fmt, ...) do { \
    struct timespec _ts; clock_gettime(CLOCK_REALTIME, &_ts); \
    printf("[%ld.%03ld] " fmt "\n", (long)_ts.tv_sec, _ts.tv_nsec/1000000, ##__VA_ARGS__); \
    fflush(stdout); } while (0)

/* ★ 返回 long long：armhf 上 long 只有 32 位，tv_sec*1000 必然溢出。
 *   所有把它当"绝对时刻"用的变量（g_start_time / g_mot_last_ev / last_hb …）
 *   也必须是 long long；只做差值的比较即使溢出也能算出正确差值，但不能依赖它当绝对时刻。 */
static long long now_ms(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

/*-----------------------------------------------------------------------------
 * ★ 混合推流模式 C：运行期改帧率（本机 HTTP /mode?fps=N 调用）
 *
 *   为什么"只改 g_cfg.fps 就够了"：
 *     video_thread 每个采集循环都读 g_cfg.fps（软件帧率闸门，时间累加器），
 *     摄像头本身始终按自己的档位出图（UVC 一般只会 30fps），低帧率就是丢帧。
 *     所以改完**立即生效**，不需要 VIDIOC_STREAMOFF/ON，也不会中断推流。
 *
 *   两个坑（都处理了）：
 *     1) g_hw_fps=1 时闸门会被旁路（"摄像头已按目标出图"），运行时改帧率必须
 *        把 g_hw_fps 清 0，强制走软件闸门，否则切了 fps 也不生效。
 *     2) CPU 保护恢复时用的是 g_base_fps，若不同步更新，呼叫中一旦经历
 *        "降级→恢复"就会被拉回开机的待机帧率。所以这里同步 g_base_fps。
 *        （当前正处降级状态时不动 g_cfg.fps，避免和 CPU 保护互相打架，
 *          只记下"新模式的帧率"，等恢复时按它来。）
 *---------------------------------------------------------------------------*/
static void set_stream_fps(int fps, const char *why)
{
    int old = g_cfg.fps;

    if (fps < 1)  fps = 1;
    if (fps > 60) fps = 60;

    g_base_fps = fps;                 /* CPU 保护恢复时回到"当前模式"的帧率 */
    g_hw_fps   = 0;                   /* 强制走软件闸门（摄像头仍是 30fps 出图）*/

    if (old == fps) {
        LOG("[mode] 帧率已是 %d fps，无需切换（%s）", fps, why);
        return;
    }
    if (g_degraded) {
        LOG("[mode] 记录目标帧率 %d fps（%s）；当前 CPU 保护降级中，实际仍按 %d fps "
            "推流，CPU 回落恢复时切到 %d fps", fps, why, old, fps);
        return;
    }
    g_cfg.fps = fps;
    LOG("[mode] 帧率切换 %d → %d（%s）", old, fps, why);
}

/*-----------------------------------------------------------------------------
 * 图像参数（--set-ctrl）
 *   注意：MJPEG 的"压缩质量"由摄像头固件决定，UVC 一般**不暴露** JPEG 质量控制项，
 *   所以这里能调的是影响观感的图像参数（锐度/对比度/饱和度/伽马/亮度）。
 *   想要"自己控制 JPEG 质量"，只能改用 YUYV 原始格式 + 本程序软编（见 --quality），
 *   但 YUYV 在高分辨率下帧率很低（1280x720 只有 10fps）。
 *---------------------------------------------------------------------------*/
static const struct { const char *name; unsigned int cid; } CTRL_MAP[] = {
    { "brightness",            V4L2_CID_BRIGHTNESS },
    { "contrast",              V4L2_CID_CONTRAST },
    { "saturation",            V4L2_CID_SATURATION },
    { "hue",                   V4L2_CID_HUE },
    { "sharpness",             V4L2_CID_SHARPNESS },
    { "gamma",                 V4L2_CID_GAMMA },
    { "backlight_compensation",V4L2_CID_BACKLIGHT_COMPENSATION },
    { "white_balance_automatic", V4L2_CID_AUTO_WHITE_BALANCE },
    { "auto_exposure",         V4L2_CID_EXPOSURE_AUTO },
    { "exposure_time_absolute",V4L2_CID_EXPOSURE_ABSOLUTE },
    { NULL, 0 }
};

static void apply_ctrls(int fd)
{
    char *spec, *save = NULL, *item;
    if (!g_cfg.ctrls) return;
    spec = strdup(g_cfg.ctrls);
    if (!spec) return;

    for (item = strtok_r(spec, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        int val = atoi(eq + 1);
        unsigned int cid = 0;
        for (int i = 0; CTRL_MAP[i].name; i++)
            if (!strcmp(CTRL_MAP[i].name, item)) { cid = CTRL_MAP[i].cid; break; }
        if (!cid) { LOG("未知图像参数: %s", item); continue; }

        struct v4l2_control ctl = { .id = cid, .value = val };
        if (ioctl(fd, VIDIOC_S_CTRL, &ctl) == 0)
            LOG("图像参数 %s = %d 已生效", item, val);
        else
            LOG("图像参数 %s = %d 设置失败（%s）", item, val, strerror(errno));
    }
    free(spec);
}

/*-----------------------------------------------------------------------------
 * 用摄像头自己的帧率控制（VIDIOC_S_PARM）设定 fps
 *   为什么要它：靠软件"每 N 毫秒放一帧"来限速会跟摄像头的出帧节拍错拍，
 *   实测会在 15fps 目标下掉到 10fps（每 3 帧才放 1 帧）。
 *   直接让摄像头按目标帧率出图，既准确又不浪费 USB 带宽。
 *   返回 1 = 摄像头接受了
 *---------------------------------------------------------------------------*/
static int set_camera_fps(int fd, int fps)
{
    struct v4l2_streamparm parm;
    if (fps <= 0) return 0;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd, VIDIOC_G_PARM, &parm) < 0) return 0;
    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        LOG("摄像头不支持设置帧率，改用软件限速");
        return 0;
    }
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        LOG("设置摄像头帧率失败（%s），改用软件限速", strerror(errno));
        return 0;
    }
    /* 读回确认：只有摄像头确实按（接近）目标帧率出图，才不需要软件再砍帧 */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
        struct v4l2_fract tf = parm.parm.capture.timeperframe;
        if (tf.numerator) {
            double got = (double)tf.denominator / tf.numerator;
            LOG("摄像头帧率: %.1f fps（目标 %d fps）", got, fps);
            return (got <= fps * 1.25) ? 1 : 0;
        }
    }
    return 1;
}

/* 队列 */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static struct pkt video_q[VIDEO_QUEUE];
static int vq_head, vq_tail, vq_count;
static struct pkt audio_q[AUDIO_QUEUE];
static int aq_head, aq_tail, aq_count;

/* （日志宏与 now_ms() 已在文件前部定义，见「日志」一节） */

/*===========================================================================
 * 3. 队列操作（视频满了丢最旧，音频满了丢最旧）
 *=========================================================================*/
static void preview_update(const unsigned char *data, int len);   /* 本机预览：更新最新帧 */

static void queue_push(int type, const unsigned char *data, int len)
{
    struct pkt *q; int *head, *tail, *count; int cap;

    if (type == PKT_VIDEO_JPEG) { q = video_q; head = &vq_head; tail = &vq_tail; count = &vq_count; cap = VIDEO_QUEUE; }
    else                        { q = audio_q; head = &aq_head; tail = &aq_tail; count = &aq_count; cap = AUDIO_QUEUE; }

    unsigned char *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);

    pthread_mutex_lock(&q_lock);
    if (*count == cap) {                     /* 满：丢掉最旧的，保证低延迟 */
        free(q[*tail].data);
        *tail = (*tail + 1) % cap;
        (*count)--;
        if (type == PKT_VIDEO_JPEG) g_sent_errors++;   /* 记一次丢帧 */
    }
    q[*head].type = type;
    q[*head].len  = len;
    q[*head].data = copy;
    *head = (*head + 1) % cap;
    (*count)++;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);

    /* 每帧顺带更新"本机预览"用的最新帧（见下面的本机 MJPEG 端口） */
    if (type == PKT_VIDEO_JPEG)
        preview_update(data, len);
}

/*===========================================================================
 * 本机只读预览端口（MJPEG over HTTP，默认 8081）
 *
 *   为什么需要：UVC 摄像头同一时刻只能被**一个进程**采集，所以由本进程
 *   统一采集、再对外分发：
 *       · 网页 / 业务后台  → 流媒体中转服务（9100，跨机器）
 *       · 板子自己的监控页 → 这里的本机端口（8081，不绕网络，延迟最低）
 *   这样"推流程序独占采集、其它程序共享画面"，是 V4L2 下唯一可行的做法。
 *=========================================================================*/
static pthread_mutex_t pv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pv_cond = PTHREAD_COND_INITIALIZER;
static unsigned char  *pv_buf  = NULL;      /* 最近一帧 JPEG 的副本 */
static int             pv_len  = 0;
static unsigned long   pv_gen  = 0;         /* 帧序号，客户端靠它判断"有新帧" */

static void preview_update(const unsigned char *data, int len)
{
    if (len <= 0 || len > 8 * 1024 * 1024)
        return;
    pthread_mutex_lock(&pv_lock);
    if (!pv_buf)
        pv_buf = (unsigned char *)malloc(8 * 1024 * 1024);
    if (pv_buf) {
        memcpy(pv_buf, data, len);
        pv_len = len;
        pv_gen++;
    }
    pthread_cond_broadcast(&pv_cond);
    pthread_mutex_unlock(&pv_lock);
}

/* 一个客户端一个线程；支持 /live.mjpg（持续推）与 /snap.jpg（抓一张） */
static void *http_client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (strstr(req, "/snap.jpg")) {
        pthread_mutex_lock(&pv_lock);
        int len = pv_len;
        if (len > 0 && pv_buf) {
            char hdr[160];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n", len);
            if (write(fd, hdr, hl) == hl) write(fd, pv_buf, len);
        } else {
            const char *e = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            write(fd, e, strlen(e));
        }
        pthread_mutex_unlock(&pv_lock);
        close(fd);
        return NULL;
    }

    /* ★ M5：开门事件查询（Qt 每 0.5s 轮询一次）
     *   {"count":N,"last_ts":<epoch秒>,"open":true/false,"mic":0/1,"spk":0/1,"talk":"full"} */
    if (strstr(req, "/door")) {
        char body[256];
        char iso[32] = "--";
        long long ts = g_door_ts;
        if (ts > 0) {
            time_t t = (time_t)ts;
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &tmv);
        }
        int bl = snprintf(body, sizeof(body),
            "{\"count\":%lu,\"last_ts\":%lld,\"last_iso\":\"%s\",\"open\":%s,"
            "\"mic\":%d,\"spk\":%d,\"talk\":\"%s\",\"aec\":%s,"
            "\"down_pkts\":%lu,\"down_bytes\":%lu,\"spk_ok\":%d,"
            "\"play_samples\":%lu,\"water\":%d,\"xrun\":%lu,\"starve\":%lu}",
            g_door_count, ts, iso,
            g_door_count > 0 ? "true" : "false",
            g_mic_mute ? 0 : 1, g_spk_mute ? 0 : 1,
            g_cfg.talk_full ? "full" : "half",
            g_aec ? "true" : "false",
            g_down_pkts, g_down_bytes, g_spk_ok,
            g_play_samples, g_down_water, g_xrun, g_down_starve);
        char hdr[200];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
        if (write(fd, hdr, hl) == hl) write(fd, body, bl);
        close(fd);
        return NULL;
    }

    /* ★ 混合推流模式 C：运行期改推流帧率（Qt 进/出「可视对讲」页时调用）
     *     GET /mode?fps=30    切 30fps（呼叫）
     *     GET /mode?fps=5     切 5fps（待机）
     *     GET /mode?fps=idle  切到 --idle-fps（默认 5）
     *     GET /mode?fps=call  切到 --call-fps（默认 30）
     *     GET /mode           只查询，不改
     *   返回 {"ok":true,"fps":<当前>,"idle":N,"call":N,"mode":"idle|call|manual",
     *         "hw_fps":0|1,"degraded":0|1} */
    if (strstr(req, "/mode")) {
        const char *q = strstr(req, "/mode?");
        const char *pf = q ? strstr(q, "fps=") : NULL;
        int want = -1, bad = 0;
        if (pf) {
            pf += 4;
            if (!strncmp(pf, "idle", 4))      want = g_cfg.idle_fps;
            else if (!strncmp(pf, "call", 4)) want = g_cfg.call_fps;
            else if (pf[0] >= '0' && pf[0] <= '9') want = atoi(pf);
            else bad = 1;
        }
        if (want > 0)      set_stream_fps(want, "HTTP /mode");
        else if (bad)      LOG("[mode] 忽略非法参数（用法 /mode?fps=30 | idle | call）");

        const char *mode = (g_cfg.fps == g_cfg.idle_fps) ? "idle"
                         : (g_cfg.fps == g_cfg.call_fps) ? "call" : "manual";
        char body[256];
        int bl = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"fps\":%d,\"idle\":%d,\"call\":%d,\"mode\":\"%s\","
            "\"hw_fps\":%d,\"degraded\":%d}",
            bad ? "false" : "true", g_cfg.fps, g_cfg.idle_fps, g_cfg.call_fps,
            mode, g_hw_fps, g_degraded);
        char hdr[200];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
        if (write(fd, hdr, hl) == hl) write(fd, body, bl);
        close(fd);
        return NULL;
    }

    /* ★ M3：对讲按钮控制（静音 / 免提）。Qt 对讲页点按钮就 GET 一下
     *   /set?mic=0|1  麦克风静音      /set?spk=0|1  喇叭（免提）开关 */
    if (strstr(req, "/set")) {
        const char *q = strstr(req, "/set?");
        if (q) {
            const char *pm = strstr(q, "mic=");
            if (pm) { g_mic_mute = (pm[4] == '0'); LOG("设置: 麦克风%s", g_mic_mute ? "静音" : "打开"); }
            const char *ps = strstr(q, "spk=");
            if (ps) { g_spk_mute = (ps[4] == '0'); LOG("设置: 喇叭%s", g_spk_mute ? "关闭" : "打开"); }
        }
        const char *b = "{\"ok\":true}";
        char hdr[160];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            (int)strlen(b));
        if (write(fd, hdr, hl) == hl) write(fd, b, strlen(b));
        close(fd);
        return NULL;
    }

    if (!strstr(req, "/live.mjpg")) {
        const char *e = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: close\r\nContent-Type: text/plain\r\n\r\n"
                        "not found (try /live.mjpg or /snap.jpg)\n";
        write(fd, e, strlen(e));
        close(fd);
        return NULL;
    }

    const char *mh = "HTTP/1.1 200 OK\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (write(fd, mh, strlen(mh)) != (ssize_t)strlen(mh)) { close(fd); return NULL; }

    unsigned char *frame = (unsigned char *)malloc(8 * 1024 * 1024);
    unsigned long last = 0;
    if (!frame) { close(fd); return NULL; }

    while (g_running) {
        int len = 0;
        pthread_mutex_lock(&pv_lock);
        while (g_running && pv_gen == last) {          /* 等新帧（最多 500ms 醒一次） */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&pv_cond, &pv_lock, &ts);
        }
        if (!g_running) { pthread_mutex_unlock(&pv_lock); break; }
        last = pv_gen;
        len = pv_len;
        if (len > 0 && len <= 8 * 1024 * 1024)
            memcpy(frame, pv_buf, len);                /* 锁内拷贝，避免发到半帧 */
        pthread_mutex_unlock(&pv_lock);

        char ph[160];
        int phl = snprintf(ph, sizeof(ph),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
        if (write(fd, ph, phl) != phl) break;
        if (write(fd, frame, len) != len) break;
        if (write(fd, "\r\n", 2) != 2) break;
    }
    free(frame);
    close(fd);
    return NULL;
}

static void *http_server_thread(void *arg)
{
    (void)arg;
    if (g_cfg.http_port <= 0) { LOG("本机预览端口已关闭 (--http-port 0)"); return NULL; }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return NULL;
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_cfg.http_port);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 4) < 0) {
        LOG("本机预览端口 %d 绑定失败（是否被占用？）", g_cfg.http_port);
        close(ls);
        return NULL;
    }
    LOG("本机预览已开启: http://127.0.0.1:%d/live.mjpg （板子监控页用它，不绕网络）",
        g_cfg.http_port);

    while (g_running) {
        struct sockaddr_in c;
        socklen_t cl = sizeof(c);
        int fd = accept(ls, (struct sockaddr *)&c, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, http_client_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else
            close(fd);
    }
    close(ls);
    return NULL;
}

/* 非阻塞取一个包；返回 0=拿到 */
static int queue_pop(struct pkt *out)
{
    pthread_mutex_lock(&q_lock);
    /* 视频优先 */
    if (vq_count > 0) {
        *out = video_q[vq_tail];
        vq_tail = (vq_tail + 1) % VIDEO_QUEUE;
        vq_count--;
    } else if (aq_count > 0) {
        *out = audio_q[aq_tail];
        aq_tail = (aq_tail + 1) % AUDIO_QUEUE;
        aq_count--;
    } else {
        pthread_mutex_unlock(&q_lock);
        return -1;
    }
    pthread_mutex_unlock(&q_lock);
    return 0;
}

static void queue_wait(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&q_lock);
    if (vq_count == 0 && aq_count == 0)
        pthread_cond_timedwait(&q_cond, &q_lock, &ts);
    pthread_mutex_unlock(&q_lock);
}

/*===========================================================================
 * 4. V4L2 采集
 *=========================================================================*/
struct v4l2_ctx {
    int fd;
    struct { void *start; size_t length; } buf[4];
    int nbuf;
    unsigned int pixfmt;
    int width, height;
};

static int v4l2_open(struct v4l2_ctx *c)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    int i;

    memset(c, 0, sizeof(*c));
    c->fd = open(g_cfg.video_dev, O_RDWR | O_NONBLOCK);
    if (c->fd < 0) { LOG("摄像头 %s 打不开: %s", g_cfg.video_dev, strerror(errno)); return -1; }

    if (ioctl(c->fd, VIDIOC_QUERYCAP, &cap) < 0) { LOG("VIDIOC_QUERYCAP 失败"); close(c->fd); return -1; }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG("%s 不是视频采集设备或不支持 streaming", g_cfg.video_dev);
        close(c->fd); return -1;
    }

    /* 优先 MJPEG（摄像头直接出压缩帧，板子几乎不用算）*/
    unsigned int try_fmt[2] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    int ok = 0;
    for (i = 0; i < 2 && !ok; i++) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width  = g_cfg.width;
        fmt.fmt.pix.height = g_cfg.height;
        fmt.fmt.pix.pixelformat = try_fmt[i];
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) continue;
        if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) continue;
        if (fmt.fmt.pix.pixelformat == try_fmt[i]) {
            c->pixfmt = fmt.fmt.pix.pixelformat;
            c->width  = fmt.fmt.pix.width;
            c->height = fmt.fmt.pix.height;
            ok = 1;
        }
    }
    if (!ok) {
        /* EBUSY 是最常见的坑：UVC 摄像头同一时刻只能被一个进程流式占用。
           本项目的社区 Qt 应用里有个"视频监控"页（原有业务），它 open 了 /dev/video0
           之后**返回也不释放**，于是推流就打不开摄像头了。 */
        LOG("设置像素格式失败（%s）", strerror(errno));
        if (errno == EBUSY)
            LOG("  ↑ 摄像头被其它进程占用。可执行 /etc/init.d/S99smart_community restart 释放");
        close(c->fd);
        return -1;
    }

    LOG("摄像头就绪: %dx%d %s", c->width, c->height,
        c->pixfmt == V4L2_PIX_FMT_MJPEG ? "MJPEG(摄像头直出)" : "YUYV(需软编)");
    g_cur_w = c->width;
    g_cur_h = c->height;

    /* 让摄像头自己按目标帧率出图（比软件限速准确，也不浪费 USB 带宽）
     *
     * ★ 混合推流模式 C（--idle-fps>0）例外：此时摄像头按**呼叫帧率**（最高档）
     *   出图，低帧率一律靠 video_thread 的软件闸门丢帧。
     *   两个原因：
     *     1) 驱动常常把 S_PARM 吸附回自己支持的档位，若它真按 5fps 出图，
     *        后面 /mode?fps=30 就只有 5fps 可用了（切不上去）；
     *     2) 保证"待机↔呼叫"来回切时摄像头参数完全不变，切换是无感的。
     *   所以模式 C 下把 g_hw_fps 强制清 0，让软件闸门永远是权威。 */
    {
        int cam_fps = g_cfg.fps;
        if (g_cfg.idle_fps > 0 && g_cfg.call_fps > cam_fps)
            cam_fps = g_cfg.call_fps;
        g_hw_fps = set_camera_fps(c->fd, cam_fps);
        if (g_cfg.idle_fps > 0) g_hw_fps = 0;
    }
    /* 影响观感的图像参数（MJPEG 的压缩质量由固件定，改不了，见 CTRL_MAP 注释） */
    apply_ctrls(c->fd);

    memset(&req, 0, sizeof(req));
    req.count = 4; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        LOG("VIDIOC_REQBUFS 失败"); close(c->fd); return -1;
    }
    c->nbuf = req.count;
    if (c->nbuf > 4) c->nbuf = 4;

    for (i = 0; i < c->nbuf; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (ioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0) { LOG("QUERYBUF 失败"); return -1; }
        c->buf[i].length = buf.length;
        c->buf[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, buf.m.offset);
        if (c->buf[i].start == MAP_FAILED) { LOG("mmap 失败"); return -1; }
        if (ioctl(c->fd, VIDIOC_QBUF, &buf) < 0) { LOG("QBUF 失败"); return -1; }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) { LOG("STREAMON 失败"); return -1; }
    return 0;
}

static void v4l2_close(struct v4l2_ctx *c)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;
    if (c->fd >= 0) {
        ioctl(c->fd, VIDIOC_STREAMOFF, &type);
        for (i = 0; i < c->nbuf; i++)
            if (c->buf[i].start && c->buf[i].start != MAP_FAILED)
                munmap(c->buf[i].start, c->buf[i].length);
        close(c->fd);
        c->fd = -1;
    }
}

/* YUYV → MJPEG 软编（只在摄像头不支持 MJPEG 时用到）*/
static int yuyv_to_jpeg(const unsigned char *yuyv, int w, int h,
                        unsigned char **out, unsigned long *outlen)
{
    static tjhandle tj = NULL;
    unsigned char *yuv420, *jpg = NULL;
    int i, j;
    unsigned long size = 0;

    if (!tj) tj = tjInitCompress();
    if (!tj) return -1;

    /* YUYV: Y0 U Y1 V → YUV420P */
    yuv420 = malloc((size_t)w * h * 3 / 2);
    if (!yuv420) return -1;
    unsigned char *Y = yuv420, *U = yuv420 + (size_t)w * h, *V = U + (size_t)w * h / 4;
    for (i = 0; i < h; i++) {
        const unsigned char *row = yuyv + (size_t)i * w * 2;
        for (j = 0; j < w / 2; j++) {
            Y[(size_t)i * w + 2 * j]     = row[4 * j + 0];
            Y[(size_t)i * w + 2 * j + 1] = row[4 * j + 2];
            if ((i & 1) == 0) {
                U[(size_t)(i / 2) * (w / 2) + j] = row[4 * j + 1];
                V[(size_t)(i / 2) * (w / 2) + j] = row[4 * j + 3];
            }
        }
    }

    if (tjCompressFromYUV(tj, Y, w, 1, h, TJSAMP_420, &jpg, &size,
                          g_cfg.quality, TJFLAG_FASTDCT) != 0) {
        free(yuv420); return -1;
    }
    free(yuv420);
    *out = jpg; *outlen = size;
    return 0;
}

/*===========================================================================
 * 5. 测试图案（没有摄像头时自检用：画一个移动的方块 + 时间戳）
 *=========================================================================*/
static void make_test_jpeg(int w, int h, unsigned char **out, unsigned long *outlen)
{
    static tjhandle tj = NULL;
    static unsigned char *rgb = NULL;
    static int frame = 0;
    unsigned char *jpg = NULL; unsigned long size = 0;
    int x, y, bx;

    if (!tj) tj = tjInitCompress();
    if (!rgb) rgb = malloc((size_t)w * h * 3);
    if (!tj || !rgb) return;

    bx = (frame * 7) % (w - 80);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            int grid = ((x / 32) + (y / 32)) & 1;
            p[0] = grid ? 40 : 90;                      /* R */
            p[1] = (unsigned char)(y * 255 / h);         /* G 渐变 */
            p[2] = (unsigned char)(x * 255 / w);         /* B 渐变 */
            if (x >= bx && x < bx + 80 && y >= h / 2 - 40 && y < h / 2 + 40) {
                p[0] = 250; p[1] = 60; p[2] = 40;        /* 移动红块 */
            }
        }
    }
    if (tjCompress2(tj, rgb, w, 0, h, TJPF_RGB, &jpg, &size, TJSAMP_420,
                    g_cfg.quality, TJFLAG_FASTDCT) == 0) {
        *out = jpg; *outlen = size;
    }
    frame++;
}

/*===========================================================================
 * ★ 2026-10-20 删除：板端事件队列(T_EVENT) 与 移动侦测（5.5 / 5.6 两节整块删除）
 *   原因（用户需求，优先级最高）：门禁不再需要移动侦测；中转也不再做任何录像/
 *   留影，板端没必要每 10 帧解码一次 1/8 灰度图做帧差（实测每次 21ms 解码，
 *   占单核约 13%）。开门事件仍由 handle_control() 记录并暴露在 :8081/door。
 *=========================================================================*/
/*===========================================================================
 * 5.7 采集时间戳 OSD（B）
 *
 *   为什么要实测：摄像头直出的是 MJPEG。要在画面上写时间戳，必须
 *   解码 → 画字 → 重新编码，1080p 在 Cortex-A7 上是"每帧几十毫秒"的量级，
 *   而 30fps 的帧周期只有 33ms。所以做成**低频 OSD**（--osd-interval，默认
 *   每 30 帧叠一次），既能在录像里取到毫秒级采集时刻，又不拖垮推流。
 *
 *   字体：内置 5x7 点阵（只含数字 - : . 空格），按帧高自动放大 2~8 倍，
 *   黑底白字，画在左上角。
 *=========================================================================*/
static const unsigned char FONT5X7[14][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   /* 9 */
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },   /* - */
    { 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00 },   /* : */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },   /* . */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   /* 空格 */
};

static int osd_font_index(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == ':') return 11;
    if (c == '.') return 12;
    if (c == ' ') return 13;
    return -1;
}

static void rgb_fill(unsigned char *rgb, int w, int h,
                     int x0, int y0, int ww, int hh,
                     unsigned char r, unsigned char g, unsigned char b)
{
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hh += y0; y0 = 0; }
    if (x0 + ww > w) ww = w - x0;
    if (y0 + hh > h) hh = h - y0;
    if (ww <= 0 || hh <= 0) return;
    for (int y = y0; y < y0 + hh; y++) {
        unsigned char *p = rgb + ((size_t)y * w + x0) * 3;
        for (int x = 0; x < ww; x++) { p[0] = r; p[1] = g; p[2] = b; p += 3; }
    }
}

/* 采集时刻文本：MM-DD HH:MM:SS.mmm（板子本地时间）*/
static void osd_stamp_text(char *out, size_t n)
{
    struct timeval tv;
    struct tm tm;
    time_t t;
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d-%02d %02d:%02d:%02d.%03d",
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(tv.tv_usec / 1000));
}

/* 解码 → 画时间戳 → 重新编码。成功返回 0，*out 需调用方 tjFree() */
static int osd_process(const unsigned char *jpg, int len,
                       unsigned char **out, unsigned long *outlen)
{
    static tjhandle dec = NULL, enc = NULL;
    static unsigned char *rgb = NULL;
    static size_t rgb_cap = 0;
    int w = 0, h = 0, sub = 0, cs = 0;
    unsigned char *j2 = NULL;
    unsigned long sz2 = 0;

    if (!dec) dec = tjInitDecompress();
    if (!enc) enc = tjInitCompress();
    if (!dec || !enc) return -1;
    if (tjDecompressHeader3(dec, jpg, (unsigned long)len, &w, &h, &sub, &cs) != 0) return -1;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;

    size_t need = (size_t)w * h * 3;
    if (need > rgb_cap) {
        unsigned char *nr = (unsigned char *)realloc(rgb, need);
        if (!nr) return -1;
        rgb = nr; rgb_cap = need;
    }
    if (tjDecompress2(dec, jpg, (unsigned long)len, rgb, w, 0, h,
                      TJPF_RGB, TJFLAG_FASTDCT) != 0) return -1;

    /* 画字：字号按帧高自适应 */
    char txt[96];
    osd_stamp_text(txt, sizeof(txt));
    int s = h / 180;
    if (s < 2) s = 2;
    if (s > 8) s = 8;
    int tw = (int)strlen(txt) * 6 * s;
    int th = 7 * s;
    int x0 = 12, y0 = 12;

    rgb_fill(rgb, w, h, x0 - 3 * s, y0 - 3 * s, tw + 6 * s, th + 6 * s, 0, 0, 0);
    for (const char *p = txt; *p; p++) {
        int idx = osd_font_index(*p);
        int cx = x0 + (int)(p - txt) * 6 * s;
        if (idx < 0) continue;
        for (int r = 0; r < 7; r++) {
            unsigned char bits = FONT5X7[idx][r];
            for (int c = 0; c < 5; c++)
                if (bits & (1 << (4 - c)))
                    rgb_fill(rgb, w, h, cx + c * s, y0 + r * s, s, s, 255, 255, 255);
        }
    }

    if (tjCompress2(enc, rgb, w, 0, h, TJPF_RGB, &j2, &sz2, TJSAMP_420,
                    g_cfg.quality, TJFLAG_FASTDCT) != 0)
        return -1;
    *out = j2;
    *outlen = sz2;
    return 0;
}

/*===========================================================================
 * 6. 采集线程
 *=========================================================================*/

/* 一帧视频的统一出口：可选 OSD → 入队
 *   owned=1 表示 jpg 是本函数内临时编码出来的（调用方随后 tjFree），本函数只读。
 *   ★ 2026-10-20：移动侦测（原来每 N 帧抽一帧做 1/8 灰度帧差）已整块删除。 */
static void video_emit(const unsigned char *jpg, int len, int owned)
{
    unsigned char *osd_jpg = NULL;
    unsigned long osd_len = 0;
    const unsigned char *out = jpg;
    int out_len = len;

    (void)owned;
    if (len <= 0) return;
    g_vid_fc++;

    /* 低频 OSD：每 --osd-interval 帧叠一次时间戳（默认 30；1=逐帧）*/
    if (g_cfg.osd && g_cfg.osd_interval > 0 &&
        (g_cfg.osd_interval <= 1 || (g_vid_fc % (unsigned long)g_cfg.osd_interval) == 0)) {
        struct timespec o0, o1;
        clock_gettime(CLOCK_MONOTONIC, &o0);
        if (osd_process(jpg, len, &osd_jpg, &osd_len) == 0 && osd_jpg) {
            clock_gettime(CLOCK_MONOTONIC, &o1);
            double ms = (double)(o1.tv_sec - o0.tv_sec) * 1000.0
                      + (double)(o1.tv_nsec - o0.tv_nsec) / 1e6;
            LOG("OSD: 第 %lu 帧 解码+画时间戳+重编码 耗时 %.1f ms（%dx%d，每 %d 帧一次，"
                "帧周期预算 %.1f ms）",
                g_vid_fc, ms, g_cur_w, g_cur_h, g_cfg.osd_interval,
                g_cfg.fps > 0 ? 1000.0 / g_cfg.fps : 0.0);
            if (ms > 10.0 && g_cfg.osd_interval < 10)
                LOG("OSD 警告: 单帧 %.1f ms 已超过 10ms，建议 --osd-interval 调到 10 以上，"
                    "否则会拖低帧率", ms);
            out = osd_jpg;
            out_len = (int)osd_len;
        } else {
            LOG("OSD: 本帧重编码失败，按原帧发送");
        }
    }

    queue_push(PKT_VIDEO_JPEG, out, out_len);
    g_video_frames++;
    g_video_bytes += (unsigned long)out_len;
    if (osd_jpg) tjFree(osd_jpg);
}

static void *video_thread(void *arg)
{
    struct v4l2_ctx ctx;
    int retry = 0;
    (void)arg;

    while (g_running) {
        if (g_cfg.synthetic) {
            unsigned char *jpg = NULL; unsigned long len = 0;
            make_test_jpeg(g_cfg.width, g_cfg.height, &jpg, &len);
            if (jpg && len) {
                video_emit(jpg, (int)len, 1);
                tjFree(jpg);
            }
            g_video_ok = 1;
            usleep(1000000 / g_cfg.fps);
            continue;
        }

        if (v4l2_open(&ctx) < 0) {
            if (g_video_ok) { LOG("事件: 摄像头断开/打开失败"); g_video_ok = 0; }
            sleep(1);
            /* 每 10 秒提示一次（别把日志刷爆），把最常见的两个原因说清楚 */
            if (++retry % 10 == 1)
                LOG("提示: 打不开 %s —— 常见原因：① 被其它进程占用（社区应用打开过"
                    "『视频监控』页会一直占着 /dev/video0，执行 /etc/init.d/S99smart_community "
                    "restart 可释放）② 摄像头接触不良/线材问题", g_cfg.video_dev);
            continue;
        }
        retry = 0;
        g_video_ok = 1;

        while (g_running) {
            struct pollfd pfd = { .fd = ctx.fd, .events = POLLIN };

            if (g_need_reopen) {                     /* CPU 保护要求换分辨率：重开设备 */
                g_need_reopen = 0;
                LOG("按 CPU 保护策略重开摄像头：%dx%d", g_cfg.width, g_cfg.height);
                break;
            }
            struct v4l2_buffer buf;
            int pr = poll(&pfd, 1, 2000);

            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pr == 0) continue;                     /* 超时，继续等（可能是拔了） */

            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(ctx.fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                break;                                  /* 设备异常 → 重开 */
            }

            /* 帧率闸门（软件限速）
               摄像头常常只支持 30fps 档位（--set-parm 会被驱动吸附回 30），
               这时靠软件抽帧。用"时间累加器"而不是"距上次 >= 间隔"：
               后者会和摄像头 33ms 的出帧节拍错拍，实测 15fps 目标会掉到 10fps。
               累加器的平均帧率准确，且对非整除目标（如 20fps）也成立。 */
            int allow = 1;
            if (g_streaming && g_cfg.fps > 0 && !g_hw_fps) {
                static long long fps_acc = 0, fps_last = 0;
                int interval = 1000 / g_cfg.fps;
                long long t = now_ms();
                if (!fps_last) fps_last = t;
                fps_acc += t - fps_last;
                fps_last = t;
                if (fps_acc > interval * 3) fps_acc = interval;   /* 卡顿后不突发 */
                if (fps_acc < interval) allow = 0;
                else fps_acc -= interval;
            } else if (g_streaming && g_cfg.fps > 0 && g_hw_fps) {
                allow = 1;                                        /* 摄像头已按目标出图 */
            }

            if (g_streaming && allow) {
                void *base = ctx.buf[buf.index].start;
                if (ctx.pixfmt == V4L2_PIX_FMT_MJPEG) {
                    /* 摄像头已经给的是 JPEG，直接转发（这就是"编码压缩"的结果）
                       移动侦测就在这条路径上抽帧做 1/8 解码，不额外占摄像头带宽 */
                    video_emit((const unsigned char *)base, (int)buf.bytesused, 0);
                } else {
                    unsigned char *jpg = NULL; unsigned long len = 0;
                    if (yuyv_to_jpeg(base, ctx.width, ctx.height, &jpg, &len) == 0 && jpg) {
                        video_emit(jpg, (int)len, 1);
                        tjFree(jpg);
                    }
                }
            }

            if (ioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0) break;
        }

        v4l2_close(&ctx);
        if (g_running) {
            LOG("事件: 视频采集中断，1 秒后重试（支持摄像头热插拔）");
            g_video_ok = 0;
            sleep(1);
        }
    }
    return NULL;
}

/*===========================================================================
 * 6.5 对讲：下行音频播放（M3）+ 回声消除（M4）+ 开门事件（M5）
 *
 *   数据流（全双工）：
 *     中转 T_AUDIO_DOWN ──► down_ring ──► playback_thread ──► ALSA plughw:0,0
 *                                    └──► far_ring（far-end 参考）
 *     麦克风 ──► audio_thread ──► speex_echo_cancellation(near, far) ──► 上行
 *
 *   为什么 far-end 用"真正写进 ALSA 的样本"而不是"收到的包"：
 *     喇叭里响的是写进 ALSA 之后、经过若干 ms 缓冲才出声的那一版；直接拿收到的
 *     包当参考会和真实回声错开一整个缓冲，AEC 就不收敛。这里 far_ring 由播放线程
 *     填，采集线程按 `--aec-delay-ms` 的偏移去取，偏移 & 尾长都不对时还能靠自适应
 *     滤波器（尾长 200ms）兜住。
 *=========================================================================*/
#define TALK_FRAME_MS     20           /* 一帧 20ms */
#define DOWN_RING_FRAMES  50           /* 环形缓冲 50 帧 × 20ms = 1 秒余量 */
#define FAR_RING_FRAMES   50

static short          *down_ring = NULL;
static int             down_cap = 0;             /* 采样点容量 */
static int             down_rd = 0, down_wr = 0, down_cnt = 0;
static pthread_mutex_t down_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  down_cond = PTHREAD_COND_INITIALIZER;

/* far-end 参考环形缓冲（AEC 用；g_down_pkts/g_spk_ok/g_mic_mute/g_door_* 等
 * 运行期状态已在文件顶部提前声明，供本机 HTTP 客户端线程读取） */
static short          *far_ring = NULL;
static int             far_cap = 0, far_wr = 0;
static long long       far_total = 0;            /* 累计写入的采样点（绝对计数） */
static pthread_mutex_t far_lock = PTHREAD_MUTEX_INITIALIZER;
/* ★ 播放链路真实延迟（采样点）：由播放线程用 snd_pcm_delay 标定后写入，
 *   AEC 取 far-end 参考时按它整体前移，否则"未来参考"消不了"现在回声"。 */
static volatile int     far_delay_samples = 0;

/* AEC 效果统计（消除前/后的 RMS，写日志作为"回声抑制"证据） */
static int             g_aec_frame = 0;
static double          g_aec_rms_in = 0.0, g_aec_rms_out = 0.0;
static unsigned long   g_aec_frames = 0;

static int talk_frame_samples(void)
{
    int rate = g_cfg.audio_rate > 0 ? g_cfg.audio_rate : 16000;
    int n = rate / (1000 / TALK_FRAME_MS);
    return n > 0 ? n : 320;
}

static void talk_init(void)
{
    int frame = talk_frame_samples();
    down_cap = frame * DOWN_RING_FRAMES;
    far_cap  = frame * FAR_RING_FRAMES;
    if (!down_ring) down_ring = (short *)calloc((size_t)down_cap, sizeof(short));
    if (!far_ring)  far_ring  = (short *)calloc((size_t)far_cap, sizeof(short));
}

/* 中转送来的下行 PCM → 环形缓冲（满了丢最旧的，保证低延迟） */
static void talk_push_down(const short *pcm, int n)
{
    if (!down_ring || n <= 0) return;
    if (n > down_cap) { pcm += (n - down_cap); n = down_cap; }
    pthread_mutex_lock(&down_lock);
    for (int i = 0; i < n; i++) {
        if (down_cnt >= down_cap) {                 /* 覆盖最旧的一帧 */
            down_rd = (down_rd + 1) % down_cap;
            down_cnt--;
        }
        down_ring[down_wr] = pcm[i];
        down_wr = (down_wr + 1) % down_cap;
        down_cnt++;
    }
    g_down_pkts++;
    g_down_bytes += (unsigned long)n * 2;
    g_last_down_ms = now_ms();
    pthread_cond_signal(&down_cond);
    pthread_mutex_unlock(&down_lock);
}

/* 取 n 个采样点；超时返回 -1（播放线程据此写静音，保持 ALSA 时钟） */
static int talk_take_down(short *out, int n, int timeout_ms)
{
    struct timespec ts;
    if (!down_ring) return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&down_lock);
    while (down_cnt < n) {
        if (pthread_cond_timedwait(&down_cond, &down_lock, &ts) != 0) {
            pthread_mutex_unlock(&down_lock);
            return -1;
        }
        if (!g_running) { pthread_mutex_unlock(&down_lock); return -1; }
    }
    for (int i = 0; i < n; i++) {
        out[i] = down_ring[down_rd];
        down_rd = (down_rd + 1) % down_cap;
        down_cnt--;
    }
    pthread_mutex_unlock(&down_lock);
    return 0;
}

/* 播放线程：把写进 ALSA 的样本记到 far_ring（AEC 的参考信号） */
static void far_push(const short *pcm, int n)
{
    if (!far_ring) return;
    pthread_mutex_lock(&far_lock);
    for (int i = 0; i < n; i++) {
        far_ring[far_wr] = pcm[i];
        far_wr = (far_wr + 1) % far_cap;
    }
    far_total += n;
    pthread_mutex_unlock(&far_lock);
}

/* 取 far-end 的"刚播完的那一帧"（delay_samples 可整体平移，默认 0） */
static int far_take(short *out, int n, int delay_samples)
{
    long long end;
    if (!far_ring) return -1;
    /* ★ 叠加播放链路真实延迟：far_push 记录的是"写进 ALSA"的时刻，真正出声还要
     *   等缓冲排空，所以取参考时要再往前移 far_delay_samples 个采样点。 */
    delay_samples += far_delay_samples;
    pthread_mutex_lock(&far_lock);
    end = far_total - delay_samples;
    if (end < n) {                       /* 还没播够一帧 → 全 0 */
        memset(out, 0, (size_t)n * sizeof(short));
        pthread_mutex_unlock(&far_lock);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        long long idx = end - n + i;
        int p = (int)((idx % far_cap + far_cap) % far_cap);
        out[i] = far_ring[p];
    }
    pthread_mutex_unlock(&far_lock);
    return 0;
}

/* ★ 下行噪声门：终端麦克风底噪随风噪/AGC 放大后送到喇叭会变成持续沙沙声。
 *   这里按帧能量判断，低于门限直接当静音播，避免"一直有风声"。
 *   实现：带滞回 + 保持时间，避免说话间隙被切得一顿一顿。 */
static int down_gate_should_mute(const short *pcm, int n)
{
    if (g_cfg.down_gate_db <= -99) return 0;         /* 门限关闭 */
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double v = pcm[i] / 32768.0;
        sum += v * v;
    }
    double rms = sqrt(sum / n);
    double db  = rms > 1e-9 ? 20.0 * log10(rms) : -180.0;
    static int hold = 0;                             /* 保持计数（帧） */
    if (db > g_cfg.down_gate_db) {
        hold = 5;                                    /* 有声：保持 5 帧≈100ms */
        return 0;
    }
    if (hold > 0) { hold--; return 0; }              /* 静音但还在保持期内 */
    return 1;
}

static void *playback_thread(void *arg)
{
    (void)arg;
    snd_pcm_t *pcm = NULL;
    int rate  = g_cfg.audio_rate > 0 ? g_cfg.audio_rate : 16000;
    int frame = talk_frame_samples();
    short *buf = (short *)malloc((size_t)frame * sizeof(short));
    short *stereo = (short *)malloc((size_t)frame * 2 * sizeof(short));   /* L/R 交错 */
    int retry = 0;
    if (!buf || !stereo) { free(buf); free(stereo); return NULL; }

    while (g_running) {
        if (!g_cfg.audio_out) { sleep(1); continue; }

        int err = snd_pcm_open(&pcm, g_cfg.audio_out, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            if (g_spk_ok) LOG("事件: 播放设备不可用 (%s)", snd_strerror(err));
            g_spk_ok = 0;
            sleep(2);
            if (++retry % 10 == 0) LOG("仍在等待播放设备 %s ...", g_cfg.audio_out);
            continue;
        }
        snd_pcm_hw_params_t *hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        /* ★ 必须用**立体声**：这块 ES8323 走 plughw:0,0 时，单声道流会被硬件按
         *   "一个采样点占两个声道槽"消费，实测播放速率正好 2 倍（32k 采样/秒），
         *   声音会变调快进。这里改成 2 声道、左右写同一份单声道样本。
         *   （aplay 默认走 dmix 是立体声，所以用它对比听不出这个问题。） */
        snd_pcm_hw_params_set_channels(pcm, hw, 2);
        unsigned int r = (unsigned int)rate;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
        snd_pcm_uframes_t per = (snd_pcm_uframes_t)frame * 2;   /* 40ms */
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, 0);
        /* ★ 缓冲从 "3×period=60ms" 提到 "6×period=240ms 量级"：
         *   实测 60ms 太小 —— 水位在 15680→800 之间剧烈摆动，播放线程反复
         *   补静音（空转 82~1059 次），听感上就是断续风噪 + 片段反复。
         *   用 buffer_time 让设备挑最近可行值，避免硬写死被驱动拒绝。 */
        unsigned int buftime = 120000;                          /* 120ms */
        snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buftime, 0);
        if (snd_pcm_hw_params(pcm, hw) < 0) {
            LOG("播放设备参数设置失败: %s", g_cfg.audio_out);
            snd_pcm_close(pcm); pcm = NULL; sleep(2); continue;
        }
        /* ★ sw_params：一帧起播 + 环形缓冲剩余一帧就唤醒，配合 snd_pcm_wait
         *   做"按设备节拍喂数据"，避免一上来就 XRUN */
        snd_pcm_sw_params_t *sw;
        snd_pcm_sw_params_alloca(&sw);
        if (snd_pcm_sw_params_current(pcm, sw) == 0) {
            snd_pcm_sw_params_set_start_threshold(pcm, sw, (snd_pcm_uframes_t)frame);
            snd_pcm_sw_params_set_avail_min(pcm, sw, (snd_pcm_uframes_t)frame);
            snd_pcm_sw_params(pcm, sw);
        }
        snd_pcm_prepare(pcm);
        g_spk_ok = 1;
        retry = 0;
        {
            snd_pcm_uframes_t gp = 0, gb = 0;
            snd_pcm_hw_params_get_period_size(hw, &gp, 0);
            snd_pcm_hw_params_get_buffer_size(hw, &gb);
            LOG("下行音频就绪: %s  %u Hz  mono  16bit PCM（住户说话从喇叭出来）"
                " period=%lu buffer=%lu 帧", g_cfg.audio_out, r,
                (unsigned long)gp, (unsigned long)gb);
        }

        while (g_running && g_spk_ok) {
            /* ★ 标准 ALSA 播放循环：用 snd_pcm_wait 让**设备**当节拍器。
             *   之前"固定睡 15ms + 写一帧"的写法两头不讨好：
             *     - 睡 40ms 时 30 秒只播出 11.8 秒（播放线程跑不到实时，下行在环形
             *       缓冲里越积越多，最后丢包）；
             *     - 改成短写循环后又会把 60ms 的 ALSA 缓冲灌满并频繁 XRUN。
             *   现在：等设备可写 → 有空位才取一帧 → 没数据就补静音，
             *   既不空转也不 underrun。 */
            int wr = snd_pcm_wait(pcm, 60);
            if (wr == 0) {                        /* 60ms 没动静：下一轮再看 */
                continue;
            }
            if (wr < 0) {
                if (wr == -EPIPE) { g_xrun++; snd_pcm_prepare(pcm); continue; }
                LOG("事件: 播放等待失败(%s)，重开设备", snd_strerror(wr));
                g_spk_ok = 0;
                break;
            }
            snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
            if (avail < 0) {
                if (avail == -EPIPE) { g_xrun++; snd_pcm_prepare(pcm); continue; }
                LOG("事件: 播放状态异常(%s)，重开设备", snd_strerror((int)avail));
                g_spk_ok = 0;
                break;
            }
            if (avail < frame) continue;          /* 缓冲里没空位，等设备再吃一点 */

            if (talk_take_down(buf, frame, frame * 1000 / rate) != 0) {
                memset(buf, 0, (size_t)frame * sizeof(short));   /* 没数据→写静音 */
                g_down_starve++;
            }
            if (g_spk_mute)
                memset(buf, 0, (size_t)frame * sizeof(short));
            /* ★ 下行噪声门：终端麦克风的底噪/风噪会被原样送下来，喇叭里就是
             *   "一直有沙沙声"。这里对下行做能量判断，低于门限直接静音，
             *   既压掉底噪，也让 AEC 的参考更干净。门限默认 -50 dBFS。 */
            if (down_gate_should_mute(buf, frame))
                memset(buf, 0, (size_t)frame * sizeof(short));
            far_push(buf, frame);                            /* 参考 = 实际送出去的（单声道） */

            /* ★ 标定播放链路真实延迟：写进 ALSA 的样本要等设备播完才真正出声，
             *   snd_pcm_delay 给出"此刻还在缓冲里没播的帧数"，用它前移 far-end 参考。
             *   否则 AEC 拿超前 60ms+ 的参考去消回声，抑制量只有几 dB。 */
            {
                snd_pcm_sframes_t dly = 0;
                if (snd_pcm_delay(pcm, &dly) == 0 && dly >= 0)
                    far_delay_samples = (int)dly;
            }

            /* 单声道 → 左右交错（2 声道）再写，扬声器才有正常速度的声音 */
            for (int i = 0; i < frame; i++) {
                stereo[2 * i]     = buf[i];
                stereo[2 * i + 1] = buf[i];
            }

            int written = 0, guard = 0;
            while (written < frame && g_running && g_spk_ok && guard++ < 50) {
                snd_pcm_sframes_t w = snd_pcm_writei(pcm, stereo + written * 2,
                                                     (snd_pcm_uframes_t)(frame - written));
                if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }   /* underrun */
                if (w == -EAGAIN) { usleep(1000); continue; }
                if (w < 0) {
                    LOG("事件: 播放失败(%s)，重开设备", snd_strerror((int)w));
                    g_spk_ok = 0;
                    break;
                }
                if (w < frame - written && !g_short_write_logged) {
                    g_short_write_logged = 1;
                    LOG("提示: 播放出现短写（请求 %d 帧，实际 %ld 帧），已循环补齐",
                        frame - written, (long)w);
                }
                written += (int)w;
            }
            if (!g_spk_ok) break;
            g_play_samples += (unsigned long)written;
            g_down_water = down_cnt;                   /* 环形缓冲水位（采样点）*/
        }
        snd_pcm_close(pcm); pcm = NULL;
        if (g_running) sleep(1);
    }
    free(buf);
    free(stereo);
    return NULL;
}

/*--------------- AEC（全双工）---------------*/
static int aec_init(int rate)
{
    int frame = talk_frame_samples();
    int tail  = rate * g_cfg.aec_tail_ms / 1000;
    if (!g_cfg.talk_full || !g_cfg.aec) {
        if (!g_cfg.talk_full)
            LOG("对讲模式: 半双工（AEC 关闭；住户说话时门口机自动闭嘴，防啸叫）");
        else
            LOG("对讲模式: 全双工但 AEC 关闭（--aec off，用于对比回声抑制效果）");
        return -1;
    }
    if (tail < frame * 8) tail = frame * 8;
    g_aec = speex_echo_state_init(frame, tail);
    if (!g_aec) {
        LOG("⚠️ AEC 初始化失败（libspeexdsp 不可用？）→ 退回半双工");
        return -1;
    }
    int sr = rate;
    speex_echo_ctl(g_aec, SPEEX_ECHO_SET_SAMPLING_RATE, &sr);
    g_aec_frame = frame;
    LOG("对讲模式: 全双工 + AEC（speexdsp MDF，帧 %d 采样=20ms，尾长 %d ms=%d taps，"
        "far-end 延迟 %d ms）", frame, g_cfg.aec_tail_ms, tail, g_cfg.aec_delay_ms);
    return 0;
}

static void aec_reset(void)
{
    if (g_aec) { speex_echo_state_destroy(g_aec); g_aec = NULL; }
    g_aec_frame = 0;
}

static inline double pcm_rms_db(const short *pcm, int n);   /* 定义在下面 */

/*===========================================================================
 * AEC 算法自测（--aec-selftest N）
 *
 *   为什么需要它：现场这块 ES8323 的麦克风里几乎听不到喇叭的声音
 *   （实测：喇叭放 1kHz，上行残留在 -62dBFS 底噪上、1kHz 相关性 0.004），
 *   所以**没法用真实声学回声**证明 AEC 有没有生效。这里用合成回声把整条
 *   AEC 通路（speex_echo_state_init / speex_echo_cancellation）跑一遍：
 *     far  = 1kHz 正弦（模拟"正在播放的下行音频"）
 *     near = far 延迟 80ms × 0.6 + 白噪声（模拟"麦克风听到的喇叭回声+本地噪声"）
 *   统计 near 与 AEC 输出的 RMS，给出抑制量。
 *   注意：这是**算法/接线自测**，不等于声学实测，报告里如实这么写。
 *=========================================================================*/
static void aec_selftest(int sec)
{
    const int frame = 320;              /* 20ms @16k */
    const int tail  = 3200;             /* 200ms */
    const int delay = frame * 4;        /* 80ms 回声延迟 */
    SpeexEchoState *st = speex_echo_state_init(frame, tail);
    if (!st) { printf("AEC 自测: speex_echo_state_init 失败\n"); return; }
    int sr = 16000;
    speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &sr);

    short *far  = (short *)calloc((size_t)(tail + frame), sizeof(short));
    short *near = (short *)malloc((size_t)frame * sizeof(short));
    short *out  = (short *)malloc((size_t)frame * sizeof(short));
    if (!far || !near || !out) { printf("AEC 自测: 内存不足\n"); goto done; }

    double sum_in = 0, sum_out = 0;
    long frames = 0, i = 0;
    unsigned int seed = 12345;
    long total = (long)(sec > 0 ? sec : 5) * 50;     /* 50 帧/秒 */

    printf("AEC 自测: 帧 %d 采样(20ms) 尾长 %d taps(%dms) 回声延迟 %dms 增益 0.6，"
           "共 %ld 帧（%.1f 秒）\n", frame, tail, tail * 1000 / 16000, delay * 1000 / 16000,
           total, total / 50.0);

    for (long f = 0; f < total; f++) {
        /* far-end：1kHz 正弦，写进历史环 */
        for (int k = 0; k < frame; k++, i++) {
            double t = (double)i / 16000.0;
            far[(int)((f * frame + k) % (tail + frame))] =
                (short)(0.30 * 32767 * sin(2 * M_PI * 1000.0 * t));
        }
        /* near-end：80ms 前的 far × 0.6 + 白噪声（约 -50dBFS） */
        for (int k = 0; k < frame; k++) {
            long idx = f * frame + k - delay;
            short e = 0;
            if (idx >= 0) e = far[(int)(idx % (tail + frame))];
            seed = seed * 1103515245u + 12345u;
            int noise = (int)((seed >> 16) % 200) - 100;
            int v = (int)(e * 0.6) + noise;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            near[k] = (short)v;
        }
        speex_echo_cancellation(st, near, far + ((f * frame) % (tail + frame)), out);
        if (f > 50) {                    /* 前 1 秒是收敛期，不计入统计 */
            sum_in  += pcm_rms_db(near, frame);
            sum_out += pcm_rms_db(out, frame);
            frames++;
        }
    }
    if (frames > 0) {
        double a = sum_in / frames, b = sum_out / frames;
        printf("AEC 自测结果: 消除前 %.1f dBFS → 消除后 %.1f dBFS，**抑制 %.1f dB**"
               "（%ld 帧平均，跳过前 1 秒收敛期）\n", a, b, a - b, frames);
    }

done:
    free(far); free(near); free(out);
    speex_echo_state_destroy(st);
}

static inline double pcm_rms_db(const short *pcm, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)pcm[i] * (double)pcm[i];
    if (n <= 0) return -96.0;
    double r = sqrt(s / n) / 32768.0;
    return 20.0 * log10(r > 1e-9 ? r : 1e-9);
}

/*===========================================================================
 * 7. 音频采集（ALSA → 16bit PCM 直传）
 *
 *   升级说明（docs/11_音频升级方案_A-law改PCM16.md）：
 *   以前把 ADC 采到的 S16_LE 用 G.711 A-law 压成 8bit 再发（128kbps），
 *   省带宽但引入 8bit 对数压缩的固有量化噪声。现在改为直接发原始 S16_LE
 *   （256kbps@16kHz，100Mbps 链路无感），板端反而少了一次编码（CPU 略降）。
 *   包类型常量仍叫 PKT_AUDIO_ALAW（协议号不变），真实格式由 hello 里
 *   的 audio 字段（pcm16k）告知中转服务。
 *=========================================================================*/
/* ---- ★ 音频门限：没插麦克风时把"空载自激的嗡声"换成数字静音 ----
 * 实测：没插麦克风时 ADC 采到 -31.7dB 的等幅振荡（听感就是持续嗡声）。
 *       这里直接对 S16_LE 采样做峰值统计，低于门限就发 PCM 全 0 静音，
 *       包数与节奏不变（不会造成 A/V 失步、也不会让下游 ffmpeg 阻塞等待）。 */
static int      g_gate_silent = 0;     /* 1 = 当前判定为"没有真实声音" */
static unsigned g_gate_win_ms = 0;     /* 窗口计时 */
static int      g_gate_peak = 0;       /* 窗口内峰值（0..32767） */

/* 返回 1 表示本包应发静音（pcm: S16_LE，n: 采样点数） */
static int audio_gate_should_mute(const short *pcm, int n)
{
    if (!g_cfg.audio_gate) return 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i];                    /* short 已是有符号 16bit */
        if (v < 0) v = -v;
        if (v > g_gate_peak) g_gate_peak = v;
    }
    g_gate_win_ms += (unsigned)(n * 1000 / (g_cfg.audio_rate > 0 ? g_cfg.audio_rate : 8000));
    if (g_gate_win_ms < 2000) return g_gate_silent;    /* 2 秒评估一次 */

    double db = (g_gate_peak > 0) ? 20.0 * log10((double)g_gate_peak / 32768.0) : -96.0;
    int was = g_gate_silent;
    g_gate_silent = (db < (double)g_cfg.audio_gate_db);
    if (g_gate_silent != was) {
        if (g_gate_silent)
            LOG("音频门限: 输入电平 %.1f dB < %d dB → 判定没插麦克风，发送静音", db, g_cfg.audio_gate_db);
        else
            LOG("音频门限: 输入电平 %.1f dB >= %d dB → 恢复真实声音", db, g_cfg.audio_gate_db);
    }
    g_gate_win_ms = 0;
    g_gate_peak = 0;
    return g_gate_silent;
}

static void *audio_thread(void *arg)
{
    snd_pcm_t *pcm = NULL;
    short buf[4096];
    int retry = 0;
    (void)arg;

    while (g_running) {
        if (!g_cfg.audio_dev) { audio_no_dev: sleep(2); continue; }

        int err = snd_pcm_open(&pcm, g_cfg.audio_dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (err < 0) {
            if (g_audio_ok) LOG("事件: 麦克风/声卡不可用 (%s)", snd_strerror(err));
            g_audio_ok = 0;
            sleep(1);
            if (++retry % 5 == 0) LOG("仍在等待音频设备 %s ...", g_cfg.audio_dev);
            continue;
        }

        snd_pcm_hw_params_t *hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm, hw, 1);
        unsigned int rate = (unsigned int)(g_cfg.audio_rate > 0 ? g_cfg.audio_rate : 8000);
        snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
        snd_pcm_hw_params_set_period_size_near(pcm, hw, (snd_pcm_uframes_t[]){160}, 0);
        if (snd_pcm_hw_params(pcm, hw) < 0) { snd_pcm_close(pcm); pcm = NULL; goto audio_no_dev; }
        snd_pcm_prepare(pcm);

        g_audio_ok = 1;
        retry = 0;
        LOG("音频就绪: %s  %u Hz  mono  16bit PCM (S16_LE)", g_cfg.audio_dev, rate);

        /* ★ M3/M4：对讲缓冲 + AEC（按实际协商到的采样率建） */
        talk_init();
        aec_init((int)rate);

        short *aec_out = (short *)malloc((size_t)g_aec_frame * sizeof(short));
        short *far_ref = (short *)malloc((size_t)g_aec_frame * sizeof(short));
        if (!aec_out || !far_ref) {
            free(aec_out); free(far_ref);
            aec_out = NULL; far_ref = NULL;
            aec_reset();
        }
        int delay_samples = g_cfg.aec_delay_ms * (int)rate / 1000;
        int half_muted = 0;

        while (g_running && g_audio_ok) {
            if (!g_streaming) { usleep(50000); continue; }
            /* 每包 20ms：8kHz=160、16kHz=320 个采样点。
             * ★ 必须**凑满整帧**再交给 AEC：plughw 插件常按 period(160) 返回半帧，
             *   半帧既不满足 speex_echo_cancellation 的固定帧长，也会让 AEC 整段跳过
             *   （实测踩过：日志里 AEC 一直不出统计，因为 n=160 != frame=320）。
             *   非阻塞 readi 返回 -EAGAIN 就小睡等下一批，最多等 200ms。 */
            int want = g_aec ? g_aec_frame : (int)(rate / 50);
            int got = 0, spins = 0;
            if (want <= 0) want = (int)(rate / 50);
            while (got < want && g_running && g_audio_ok && spins < 200) {
                snd_pcm_sframes_t rd = snd_pcm_readi(pcm, buf + got,
                                                     (snd_pcm_uframes_t)(want - got));
                if (rd == -EAGAIN || rd == 0) { usleep(2000); spins++; continue; }
                if (rd < 0) {
                    LOG("事件: 音频读取失败(%s)，重开设备", snd_strerror((int)rd));
                    g_audio_ok = 0;
                    break;
                }
                got += (int)rd;
            }
            if (!g_audio_ok) break;
            if (got < want) { usleep(5000); continue; }   /* 凑不满就丢掉这一轮 */
            snd_pcm_sframes_t n = (snd_pcm_sframes_t)got;

            /* ★ M4：全双工回声消除。near = 麦克风，far = 正在播放的下行音频，
             *   输出直接覆盖 buf 后照常向上推（下行在喇叭里响、上行里已经把它抠掉）。
             *   帧长必须和 speex_echo_state_init 的一致，不一致就跳过这一帧。 */
            if (g_aec && g_aec_frame == (int)n && aec_out && far_ref) {
                far_take(far_ref, g_aec_frame, delay_samples);
                double dbin = pcm_rms_db(buf, g_aec_frame);
                double dbfar = pcm_rms_db(far_ref, g_aec_frame);
                if (dbfar > -60.0) {          /* 只有"喇叭真在响"时才统计抑制效果 */
                    speex_echo_cancellation(g_aec,
                            (const spx_int16_t *)buf,
                            (const spx_int16_t *)far_ref,
                            (spx_int16_t *)aec_out);
                    memcpy(buf, aec_out, (size_t)g_aec_frame * sizeof(short));
                    g_aec_rms_in  += dbin;
                    g_aec_rms_out += pcm_rms_db(aec_out, g_aec_frame);
                    g_aec_frames++;
                } else {
                    static int once_quiet = 0;
                    if (!once_quiet) {        /* 第一次发现"喇叭没在响"就报一次，省得刷屏 */
                        once_quiet = 1;
                        LOG("AEC: far-end 参考为静音(%0.1f dB)——下行还没数据或没在播放", dbfar);
                    }
                    /* 喇叭没声：仍喂一次 playback，让自适应滤波器保持对齐 */
                    speex_echo_cancellation(g_aec,
                            (const spx_int16_t *)buf,
                            (const spx_int16_t *)far_ref,
                            (spx_int16_t *)aec_out);
                    memcpy(buf, aec_out, (size_t)g_aec_frame * sizeof(short));
                }
            } else if (g_aec) {
                static int once_len = 0;
                if (!once_len) {
                    once_len = 1;
                    LOG("AEC: 采集帧长 %d != %d，本帧跳过 AEC", (int)n, g_aec_frame);
                }
            } else if (!g_cfg.talk_full) {
                /* ★ 半双工回退：对方正在说话（400ms 内有下行音频）→ 门口机闭嘴，
                 *   避免回声/啸叫。这就是"对讲机"手感。 */
                int recent = (now_ms() - g_last_down_ms) < 400;
                if (recent && !half_muted) {
                    half_muted = 1;
                    LOG("半双工: 住户正在说话 → 门口机上行静音");
                } else if (!recent && half_muted) {
                    half_muted = 0;
                    LOG("半双工: 住户说完 → 门口机恢复送话");
                }
                if (half_muted) memset(buf, 0, (size_t)n * 2);
            }

            /* 静音按钮：上行发数字静音（0） */
            if (g_mic_mute) memset(buf, 0, (size_t)n * 2);

            /* ★ 直接投递 S16_LE 原始 PCM（不再 A-law 编码），字节数 = 采样点数 × 2 */
            if (audio_gate_should_mute(buf, (int)n))
                memset(buf, 0, (size_t)n * 2);      /* PCM 数字静音（全 0）*/
            queue_push(PKT_AUDIO_ALAW, (unsigned char *)buf, (int)n * 2);
            g_audio_pkts++;
        }
        free(aec_out); free(far_ref);
        aec_reset();
        snd_pcm_close(pcm); pcm = NULL;
        if (g_running) sleep(1);
    }
    return NULL;
}

/*===========================================================================
 * 8. 推流 + 控制通道
 *=========================================================================*/
static int tcp_connect(const char *hostport, int timeout_ms)
{
    char host[128]; int port = 9100;
    const char *colon = strchr(hostport, ':');
    if (colon) {
        size_t n = (size_t)(colon - hostport);
        if (n >= sizeof(host)) n = sizeof(host) - 1;
        memcpy(host, hostport, n); host[n] = 0;
        port = atoi(colon + 1);
    } else snprintf(host, sizeof(host), "%s", hostport);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* 带超时的非阻塞 connect */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        r = poll(&pfd, 1, timeout_ms);
        if (r > 0) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            r = err ? -1 : 0;
        } else r = -1;
    } else if (r < 0) r = -1;
    freeaddrinfo(res);

    if (r < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, flags);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_all(int fd, const void *buf, int len)
{
    const unsigned char *p = buf;
    int sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int send_packet(int fd, int type, const unsigned char *data, int len, int flags)
{
    unsigned char hdr[PKT_HDR_LEN];
    unsigned short seq = ++g_seq;
    memcpy(hdr, PKT_MAGIC, 4);
    hdr[4] = (unsigned char)type;
    hdr[5] = (unsigned char)flags;
    hdr[6] = seq & 0xff; hdr[7] = (seq >> 8) & 0xff;       /* 小端 */
    hdr[8]  = len & 0xff;         hdr[9]  = (len >> 8) & 0xff;
    hdr[10] = (len >> 16) & 0xff; hdr[11] = (len >> 24) & 0xff;
    if (send_all(fd, hdr, PKT_HDR_LEN) < 0) return -1;
    if (len > 0 && send_all(fd, data, len) < 0) return -1;
    return 0;
}

static int send_status(int fd)
{
    char json[512];
    int video_fps = 0;
    static unsigned long last_frames = 0;
    static long long last_t = 0;
    long long t = now_ms();
    long long dt = last_t ? (t - last_t) : 0;

    /* ★ 用 32 位除法算帧率：帧数增量×1000 远小于 2^32，没必要为一次除法拉进
     *   libgcc 的 64 位除法（那会让二进制明显变大）。只有时刻本身需要 64 位。 */
    if (dt > 0)
        video_fps = (int)((unsigned long)((g_video_frames - last_frames) * 1000UL)
                          / (unsigned long)dt);
    last_frames = g_video_frames; last_t = t;

    /* ★ uptime 用 long long：armhf 上 g_start_time 是 epoch 毫秒，32 位 long 装不下，
     *   之前算出来的 uptime 是天文数字（实测 1791001461 秒 ≈ 56 年）。
     *   现在 = 当前 epoch 秒 − 启动时的 epoch 秒。 */
    snprintf(json, sizeof(json),
        "{\"dev\":\"%s\",\"state\":\"%s\",\"video\":%d,\"audio\":%d,"
        "\"fps\":%d,\"frames\":%lu,\"audio_pkts\":%lu,\"kbytes\":%lu,"
        "\"res\":\"%dx%d\",\"uptime\":%lld,\"net\":\"up\"}",
        g_cfg.device_id, g_streaming ? "streaming" : "paused",
        g_video_ok, g_audio_ok, video_fps, g_video_frames, g_audio_pkts,
        g_video_bytes / 1024, g_cur_w, g_cur_h,
        (long long)time(NULL) - g_start_time / 1000LL);
    return send_packet(fd, PKT_STATUS, (unsigned char *)json, (int)strlen(json), 0);
}

/* ★ 2026-10-20：send_event_ex()/send_event() 已删除 —— 板端不再发 T_EVENT。
 *   摄像机/声卡插拔只写本地日志（health_thread），开门事件走本机 :8081/door。 */

/* 处理后台下发的控制指令（极简 JSON 解析：找 "cmd":"xxx"）*/
static void handle_control(const unsigned char *data, int len)
{
    char buf[512];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n); buf[n] = 0;

    if (strstr(buf, "\"start\"")) {
        g_streaming = 1;
        LOG("收到后台指令: 开始推流");
    } else if (strstr(buf, "\"stop\"")) {
        g_streaming = 0;
        LOG("收到后台指令: 停止推流（保持心跳与状态上报）");
    } else if (strstr(buf, "\"open\"")) {
        /* ★ M5：开门事件。按用户要求**只记录 + 弹窗**，不驱动 GPIO/继电器。
         * Qt（videodisplay）每 0.5s 轮询 :8081/door，看到 count 变大就弹"已开门"。 */
        g_door_count++;
        g_door_ts = (long long)time(NULL);
        LOG("★ 收到开门指令（累计第 %lu 次）：已记录事件，等待 Qt 弹窗确认（不驱动 GPIO）",
            g_door_count);
    } else {
        LOG("收到未知指令: %s", buf);
    }
}

static void *pusher_thread(void *arg)
{
    int backoff = 1;
    char hello[256];
    (void)arg;

    snprintf(hello, sizeof(hello),
             "{\"dev\":\"%s\",\"name\":\"RK3506B监控\",\"proto\":\"SCAV1\","
             "\"video\":\"image/jpeg\",\"audio\":\"pcm%dk\",\"synthetic\":%d}",
             g_cfg.device_id, g_cfg.audio_rate / 1000, g_cfg.synthetic);

    while (g_running) {
        int fd = tcp_connect(g_cfg.server, 3000);
        if (fd < 0) {
            LOG("连接中转服务 %s 失败，%d 秒后重试", g_cfg.server, backoff);
            sleep(backoff);
            if (backoff < 10) backoff *= 2;
            continue;
        }
        LOG("已连接中转服务 %s", g_cfg.server);
        backoff = 1;
        send_packet(fd, PKT_STATUS, (unsigned char *)hello, (int)strlen(hello), 1 /*flags=1: hello*/);

        long long last_hb = 0;
        int last_video_state = -1, last_audio_state = -1;
        /* ★ 收包缓冲：一次 recv 常常带回好几个包（下行音频是 640 字节一包，中转
         *   会连着送）。所以必须"先把缓冲区里所有完整的包都解析完，再去 recv"，
         *   否则每次只处理 1 个、剩下的一直压在缓冲里——实测 9 个包只收到 4 个
         *   （丢 55% 的下行语音）。这里把缓冲加大到 16KB 并循环排空。 */
        unsigned char rbuf[PKT_HDR_LEN + 16384];
        int rlen = 0;

        while (g_running) {
            /* 1) 先把缓冲区里已完整的包全部处理掉 */
            for (;;) {
                if (rlen < PKT_HDR_LEN) break;
                int plen = rbuf[8] | (rbuf[9] << 8) | (rbuf[10] << 16) | (rbuf[11] << 24);
                if (plen < 0 || plen > MAX_PKT) { rlen = 0; break; }   /* 异常长度：丢弃 */
                if (rlen < PKT_HDR_LEN + plen) break;                 /* 还没收全 */
                if (rbuf[4] == PKT_CONTROL) {
                    handle_control(rbuf + PKT_HDR_LEN, plen);
                } else if (rbuf[4] == PKT_AUDIO_DOWN) {
                    /* ★ M3：住户语音（PCM16 16k 单声道）→ 喇叭 + AEC 参考 */
                    talk_push_down((const short *)(rbuf + PKT_HDR_LEN), plen / 2);
                }
                memmove(rbuf, rbuf + PKT_HDR_LEN + plen, (size_t)(rlen - PKT_HDR_LEN - plen));
                rlen -= PKT_HDR_LEN + plen;
            }

            /* 2) 再收新数据（非阻塞） */
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            while (poll(&pfd, 1, 0) > 0) {
                int n = recv(fd, rbuf + rlen, (int)sizeof(rbuf) - rlen, 0);
                if (n <= 0) { LOG("连接被对端关闭"); goto reconnect; }
                rlen += n;
                /* 收完立刻解析（不等下一轮 poll） */
                for (;;) {
                    if (rlen < PKT_HDR_LEN) break;
                    int plen = rbuf[8] | (rbuf[9] << 8) | (rbuf[10] << 16) | (rbuf[11] << 24);
                    if (plen < 0 || plen > MAX_PKT) { rlen = 0; break; }
                    if (rlen < PKT_HDR_LEN + plen) break;
                    if (rbuf[4] == PKT_CONTROL) {
                        handle_control(rbuf + PKT_HDR_LEN, plen);
                    } else if (rbuf[4] == PKT_AUDIO_DOWN) {
                        talk_push_down((const short *)(rbuf + PKT_HDR_LEN), plen / 2);
                    }
                    memmove(rbuf, rbuf + PKT_HDR_LEN + plen, (size_t)(rlen - PKT_HDR_LEN - plen));
                    rlen -= PKT_HDR_LEN + plen;
                }
                if (rlen >= (int)sizeof(rbuf)) rlen = 0;   /* 保护：异常数据别把缓冲撑满 */
            }

            /* 2) 发数据帧 */
            if (g_streaming) {
                struct pkt p;
                int sent_any = 0;
                while (queue_pop(&p) == 0) {
                    if (send_packet(fd, p.type, p.data, p.len, 0) < 0) {
                        free(p.data);
                        LOG("发送失败");
                        goto reconnect;
                    }
                    free(p.data);
                    sent_any = 1;
                    if (p.type == PKT_AUDIO_ALAW && g_video_frames % 30 == 0) break;  /* 别饿死视频 */
                }
                if (!sent_any) queue_wait(10);
            } else {
                queue_wait(50);
            }

            /* 3) 心跳/状态上报（设备插拔只写本机日志，不再发 T_EVENT） */
            if (now_ms() - last_hb > 1000) {
                last_hb = now_ms();
                if (send_status(fd) < 0) { LOG("心跳发送失败"); goto reconnect; }
                if (last_video_state != -1 && g_video_ok != last_video_state)
                    LOG("摄像头状态变化: %s（只记本地日志）", g_video_ok ? "已接入" : "已拔出");
                if (last_audio_state != -1 && g_audio_ok != last_audio_state)
                    LOG("音频状态变化: %s（只记本地日志）", g_audio_ok ? "正常" : "异常/拔出");
                last_video_state = g_video_ok;
                last_audio_state = g_audio_ok;
            }
        }
reconnect:
        if (fd >= 0) close(fd);
        if (g_running) {
            LOG("网络断开，%d 秒后自动重连（推流不中断采集）", backoff);
            sleep(backoff);
            if (backoff < 10) backoff *= 2;
        }
    }
    return NULL;
}

/*===========================================================================
 * 9. 健康监控线程：设备插拔 + 周期性日志
 *=========================================================================*/
/* CPU 保护线程：超阈值自动降级，回落后自动恢复（带滞回，避免来回抖） */
static void *cpu_guard_thread(void *arg)
{
    int over = 0, under = 0;
    (void)arg;
    g_base_w = g_cfg.width; g_base_h = g_cfg.height; g_base_fps = g_cfg.fps;
    cpu_read_percent();                 /* 建立基准 */
    sleep(5);

    while (g_running) {
        int cpu = cpu_read_percent();
        if (cpu >= 0) {
            g_cpu_percent = cpu;
            if (cpu >= g_cfg.cpu_high) {
                if (++over >= 3) {                       /* 约 15 秒持续超阈值 */
                    if (!g_degraded) {
                        g_degraded = 1;
                        /* ★ 模式C：待机基线可能比 fps_low 更低（5 < 8），
                         *   "降级"绝不能反而把帧率调高，取两者较小者。 */
                        g_cfg.fps = (g_base_fps > 0 && g_base_fps < g_cfg.fps_low)
                                  ? g_base_fps : g_cfg.fps_low;
                        g_hw_fps = 0;                    /* 关掉"摄像头已按目标出图"的旁路 */
                        if (g_base_w > 1280 || g_base_h > 720) {
                            g_cfg.width = 1280; g_cfg.height = 720;
                            g_need_reopen = 1;           /* 让视频线程换分辨率重开 */
                        }
                        LOG("⚠️ CPU %d%% ≥ %d%%：自动降级 → %dx%d @%dfps（保护系统）",
                            cpu, g_cfg.cpu_high, g_cfg.width, g_cfg.height, g_cfg.fps);
                    }
                }
                under = 0;
            } else if (cpu <= g_cfg.cpu_low) {
                if (++under >= 12 && g_degraded) {       /* 约 1 分钟持续回落 */
                    g_degraded = 0;
                    g_cfg.width = g_base_w; g_cfg.height = g_base_h;
                    g_cfg.fps = g_base_fps;
                    g_need_reopen = 1;
                    LOG("✅ CPU 回落到 %d%%：恢复 %dx%d @%dfps", cpu,
                        g_cfg.width, g_cfg.height, g_cfg.fps);
                }
                over = 0;
            } else {
                over = 0; under = 0;
            }
        }
        sleep(5);
    }
    return NULL;
}

static void *health_thread(void *arg)
{
    int last_video = -1, last_audio = -1;
    (void)arg;

    while (g_running) {
        struct stat st;
        int v = (g_cfg.synthetic) ? 1 : (stat(g_cfg.video_dev, &st) == 0);
        int a = g_cfg.audio_dev ? g_audio_ok : 0;

        if (last_video != -1 && v != last_video)
            LOG("摄像头状态变化: %s", v ? "已接入" : "已拔出");
        if (last_audio != -1 && a != last_audio)
            LOG("音频状态变化: %s", a ? "正常工作" : "异常/拔出");
        last_video = v; last_audio = a;

        LOG("状态: 视频%s(%lu帧) 音频%s(%lu包) 流%s 累计%luKB 丢帧%lu",
            g_video_ok ? "OK" : "断开", g_video_frames,
            g_audio_ok ? "OK" : "无", g_audio_pkts,
            g_streaming ? "推流中" : "已暂停",
            g_video_bytes / 1024, g_sent_errors);

        /* ★ M3/M4/M5：对讲与开门取证（一句话看全：有没有在通、AEC 抑制了多少） */
        {
            double tin = 0, tout = 0;
            unsigned long nf = g_aec_frames;
            static unsigned long prev_play = 0;
            static long long prev_ms = 0;
            long long tnow = now_ms();
            unsigned long played = g_play_samples - prev_play;
            double secs = prev_ms ? (double)(tnow - prev_ms) / 1000.0 : 0.0;
            double rate = (secs > 0.2) ? (double)played / secs : 0.0;
            prev_play = g_play_samples;
            prev_ms = tnow;

            tin = g_aec_rms_in; tout = g_aec_rms_out;
            g_aec_rms_in = g_aec_rms_out = 0.0; g_aec_frames = 0;
            LOG("对讲: 模式=%s AEC=%s 下行包=%lu 播放速率=%.0f 采样/秒 水位=%d 下溢=%lu 空转=%lu "
                "喇叭=%s 开门=%lu 参考延迟=%d采样%s",
                g_cfg.talk_full ? "full" : "half",
                g_aec ? "开" : "关",
                g_down_pkts, rate, g_down_water, g_xrun, g_down_starve,
                g_spk_ok ? "就绪" : "无",
                g_door_count,
                far_delay_samples,
                nf ? "" : "（本窗口无下行音频）");
            if (nf)
                LOG("AEC 抑制: 本窗口 %lu 帧 平均 消除前 %.1f dB → 消除后 %.1f dB，"
                    "抑制 %.1f dB（far-end 有声才统计）",
                    nf, tin / nf, tout / nf, (tin / nf) - (tout / nf));
        }

        /* ★ 分 1 秒睡：SIGTERM 后能立刻退出，否则重启时要等满 30 秒，
         *   新旧两个 av_stream 会抢 /dev/video0（实测踩过：新进程一直
         *   "Device or resource busy"，摄像头和声卡都被老进程占着）。 */
        for (int k = 0; k < 30 && g_running; k++) sleep(1);
    }
    return NULL;
}
/*===========================================================================
 * 10. main
 *=========================================================================*/
static void on_sig(int s) { (void)s; g_running = 0; }

static void usage(const char *p)
{
    printf("用法: %s [选项]\n"
           "  --server host:port   流媒体中转服务地址 (默认 127.0.0.1:9100)\n"
           "  --dev /dev/videoX    摄像头设备 (默认 /dev/video0)\n"
           "  --audio hw:N,0       音频采集设备 (不写则只推视频)\n"
           "  --audio-out hw:N,0   音频播放设备（★对讲下行出声用，例 plughw:0,0）\n"
           "                       板载 ES8323 一般是 plughw:0,0；不写则不播放（只上行）\n"
           "  --talk half|full     对讲模式 (默认 full 全双工+AEC；half=半双工回退)\n"
           "  --aec on|off         回声消除开关 (默认 on，仅 full 模式有效)\n"
           "  --aec-tail-ms N      AEC 自适应滤波器尾长 (默认 200ms，覆盖喇叭缓冲延迟)\n"
           "  --aec-delay-ms N     far-end 参考的固定延迟 (默认 0；调不准时用尾长兜)\n"
           "  --id 设备标识        上报给后台的名字 (默认 rk3506b-01)\n"
           "  --size WxH           分辨率 (默认 640x480；本摄像头支持到 1920x1080)\n"
           "  --fps N              帧率 (默认 15；被 --idle-fps 覆盖，仅 --idle-fps 0 时生效)\n"
           "  --idle-fps N         待机帧率 (默认 5；开机就用它。0=关掉待机低帧率)\n"
           "  --call-fps N         呼叫帧率 (默认 30；/mode?fps=call 用它)\n"
           "                       运行时切换: curl 'http://127.0.0.1:8081/mode?fps=30'\n"
           "                                   curl 'http://127.0.0.1:8081/mode?fps=5'\n"
           "  --http-port N        本机只读预览端口 (默认 8081，0=关闭)\n"
           "  --cpu-high N         CPU 超过 N%% 自动降级 (默认 90)\n"
           "  --cpu-low N          CPU 低于 N%% 自动恢复 (默认 60)\n"
           "  --fps-low N          降级后的帧率 (默认 8)\n"
           "  --no-cpu-guard       关闭 CPU 自动降级\n"
           "  --audio-gate-db N    音频门限 dB（默认 -25；低于它视为没插麦克风，发静音）\n"
           "  --no-audio-gate      关闭音频门限\n"
           "  --audio-rate N       音频采样率 (默认 16000，PCM16 每采样 2 字节)\n"
           "                       板子自己的监控页从这里取流: http://127.0.0.1:8081/live.mjpg\n"
           "  --quality N          JPEG 质量 0-100 (默认 75，仅 YUYV 软编模式有效)\n"
           "  --set-ctrl 参数      调图像观感，例: \"sharpness=3,contrast=10\"\n"
           "                       可选: brightness contrast saturation hue sharpness\n"
           "                             gamma backlight_compensation\n"
           "                             white_balance_automatic auto_exposure\n"
           "                             exposure_time_absolute\n"
           "  --synthetic          无摄像头时用测试图案自检\n"
           "  --list-audio         列出可用的音频采集设备\n"
           "  --aec-selftest N     只跑 AEC 算法自测（合成 1kHz 回声，N 秒）后退出\n"
           "\n"
           "（★ 2026-10-20：移动侦测已删除，不再有 --motion* 参数）\n"
           "\n"
           "采集时间戳 OSD（默认关；1080p 重编码很贵，见 --osd-interval）:\n"
           "  --osd on|off          把采集时刻画进画面 (默认 off)\n"
           "  --osd-interval N      每 N 帧叠一次 (默认 30，1=逐帧)\n", p);
}

static void list_audio(void)
{
    snd_pcm_info_t *info; int card = -1;
    printf("=== 可用的音频采集设备（--audio hw:CARD,0）===\n");
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *name = NULL;
        if (snd_card_get_name(card, &name) == 0) {
            fprintf(stdout, "  hw:%d,0   %s\n", card, name);
            free(name);
        }
    }
    (void)info;
}

/* "on/off" 之类的开关参数 */
static int arg_onoff(const char *s)
{
    if (!s) return 0;
    return (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes") ||
            !strcmp(s, "true") || !strcmp(s, "ON"));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) g_cfg.server = argv[++i];
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) g_cfg.video_dev = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) g_cfg.audio_dev = argv[++i];
        else if (!strcmp(argv[i], "--audio-out") && i + 1 < argc) g_cfg.audio_out = argv[++i];
        else if (!strcmp(argv[i], "--talk") && i + 1 < argc) {
            const char *m = argv[++i];
            g_cfg.talk_full = strcmp(m, "half") != 0;      /* 默认 full */
        }
        else if (!strcmp(argv[i], "--aec") && i + 1 < argc) g_cfg.aec = arg_onoff(argv[++i]);
        else if (!strcmp(argv[i], "--aec-tail-ms") && i + 1 < argc) g_cfg.aec_tail_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aec-delay-ms") && i + 1 < argc) g_cfg.aec_delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--down-gate-db") && i + 1 < argc) g_cfg.down_gate_db = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aec-selftest") && i + 1 < argc) g_cfg.aec_selftest = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--id") && i + 1 < argc) g_cfg.device_id = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &g_cfg.width, &g_cfg.height);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_cfg.fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-fps") && i + 1 < argc) g_cfg.idle_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--call-fps") && i + 1 < argc) g_cfg.call_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) g_cfg.quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--set-ctrl") && i + 1 < argc) g_cfg.ctrls = argv[++i];
        else if (!strcmp(argv[i], "--http-port") && i + 1 < argc) g_cfg.http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-high") && i + 1 < argc) g_cfg.cpu_high = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-low") && i + 1 < argc) g_cfg.cpu_low = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps-low") && i + 1 < argc) g_cfg.fps_low = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cpu-guard")) g_cfg.cpu_guard = 0;
        else if (!strcmp(argv[i], "--audio-gate-db") && i + 1 < argc) g_cfg.audio_gate_db = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio-gate")) g_cfg.audio_gate = 0;
        else if (!strcmp(argv[i], "--audio-rate") && i + 1 < argc) g_cfg.audio_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--synthetic")) g_cfg.synthetic = 1;
        else if (!strcmp(argv[i], "--list-audio")) g_cfg.list_audio = 1;
        /* 采集时间戳 OSD（B）*/
        else if (!strcmp(argv[i], "--osd") && i + 1 < argc) g_cfg.osd = arg_onoff(argv[++i]);
        else if (!strcmp(argv[i], "--osd-interval") && i + 1 < argc) g_cfg.osd_interval = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    /* 参数合法性兜底 */
    if (!g_cfg.talk_full) g_cfg.aec = 0;             /* 半双工不需要 AEC */
    if (g_cfg.aec_tail_ms < 40) g_cfg.aec_tail_ms = 40;
    if (g_cfg.aec_tail_ms > 1000) g_cfg.aec_tail_ms = 1000;
    if (g_cfg.aec_delay_ms < 0) g_cfg.aec_delay_ms = 0;
    if (g_cfg.osd_interval < 1) g_cfg.osd_interval = 1;

    /* ★ 混合推流模式 C：开机默认走「待机帧率」（--idle-fps，默认 5；0=关掉该特性，
     *   此时仍用 --fps）。帧率可以随后用本机 HTTP 端点临时改：
     *       curl 'http://127.0.0.1:8081/mode?fps=30' → 呼叫全速
     *       curl 'http://127.0.0.1:8081/mode?fps=5'  → 回到待机
     *   摄像头本身固定按自己的档位出图，低帧率靠 video_thread 的软件闸门丢帧，
     *   所以切换不需要重开摄像头，立即生效。 */
    if (g_cfg.idle_fps < 0)  g_cfg.idle_fps = 0;
    if (g_cfg.idle_fps > 60) g_cfg.idle_fps = 60;
    if (g_cfg.call_fps < 1)  g_cfg.call_fps = 30;
    if (g_cfg.call_fps > 60) g_cfg.call_fps = 60;
    if (g_cfg.idle_fps > 0) g_cfg.fps = g_cfg.idle_fps;

    if (g_cfg.list_audio) { list_audio(); return 0; }
    if (g_cfg.aec_selftest > 0) { aec_selftest(g_cfg.aec_selftest); return 0; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);
    g_start_time = now_ms();

    LOG("=== RK3506B 视频监控设备端启动 ===");
    LOG("设备标识: %s  中转服务: %s  视频: %s(%dx%d@%d)  音频: %s",
        g_cfg.device_id, g_cfg.server, g_cfg.synthetic ? "测试图案" : g_cfg.video_dev,
        g_cfg.width, g_cfg.height, g_cfg.fps, g_cfg.audio_dev ? g_cfg.audio_dev : "无");
    LOG("移动侦测: 已删除（2026-10-20 按需求整块移除，不再抽帧解码）");
    LOG("画面时间戳 OSD: %s（每 %d 帧叠一次）",
        g_cfg.osd ? "开" : "关（--osd on 打开，1080p 需解码+重编码）", g_cfg.osd_interval);
    LOG("对讲: 模式=%s  下行播放=%s  AEC=%s  上游采样率=%dHz mono PCM16",
        g_cfg.talk_full ? "full(全双工)" : "half(半双工)",
        g_cfg.audio_out ? g_cfg.audio_out : "未配置（只上行，不播放）",
        g_cfg.aec ? "开" : "关", g_cfg.audio_rate);
    LOG("开门: 收到 T_CONTROL{open} 只记录事件（:8081/door），不驱动 GPIO");
    if (g_cfg.idle_fps > 0)
        LOG("[mode] 混合推流模式 C：开机待机 %d fps，呼叫全速 %d fps；"
            "切换端点 http://127.0.0.1:%d/mode?fps=N（当前 %d fps）",
            g_cfg.idle_fps, g_cfg.call_fps, g_cfg.http_port, g_cfg.fps);
    else
        LOG("[mode] 待机低帧率已关闭（--idle-fps 0），按 %d fps 常开", g_cfg.fps);

    talk_init();                       /* 下行音频/参考信号缓冲 */
    pthread_t t1, t2, t3, t4, t6;
    pthread_create(&t1, NULL, video_thread, NULL);
    if (g_cfg.audio_dev) pthread_create(&t2, NULL, audio_thread, NULL);
    pthread_create(&t3, NULL, pusher_thread, NULL);
    pthread_create(&t4, NULL, http_server_thread, NULL);   /* 本机只读预览端口 */
    if (g_cfg.audio_out) pthread_create(&t6, NULL, playback_thread, NULL);  /* ★ M3 下行 */
    pthread_t t5;
    if (g_cfg.cpu_guard) pthread_create(&t5, NULL, cpu_guard_thread, NULL);  /* CPU 保护 */

    health_thread(NULL);          /* 主线程当健康监控 */

    g_running = 0;
    pthread_join(t1, NULL);
    if (g_cfg.audio_dev) pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    if (g_cfg.audio_out) pthread_join(t6, NULL);
    LOG("=== 退出（共推送 %lu 视频帧 / %lu 音频包 / %lu 下行包 / %lu KB）===",
        g_video_frames, g_audio_pkts, g_down_pkts, g_video_bytes / 1024);
    return 0;
}
