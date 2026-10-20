#include "videodisplay.h"
#include "ui_videodisplay.h"

#include "board_config.h"
#include "door_client.h"

#include <QMessageBox>
#include <QImage>
#include <QIcon>
#include <QPixmap>
#include <QImageReader>
#include <QBuffer>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QDebug>
#include <cstring>

/*---------------------------------------------------------------------------
 * 门禁「可视对讲」页（★ 2026-10-20 按需推流版）
 *   只有住户在终端点了【接听】、home 看到 callstate=accepted 并启动
 *   /userdata/av_stream_on.sh 之后，本页才会被打开。
 *   进页：拉本机 :8081/live.mjpg（av_stream 刚起，需要重试）→ 出画面
 *   返回：断流（首页负责挂断 + 停 av_stream）
 *-------------------------------------------------------------------------*/
#define RETRY_INTERVAL_MS  1000     /* 拉流失败重试间隔（av_stream 启动约 2~4 秒）*/
#define RETRY_MAX_NOTICE   15       /* 连续失败这么多次就明确提示"推流没起来" */

videodisplay::videodisplay(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::videodisplay)
{
    ui->setupUi(this);
    /* ★ 纯 QWidget 子类默认不画 QSS 的 background（会透出下面的首页），必须显式打开 */
    setAttribute(Qt::WA_StyledBackground, true);
    memset(&pd, 0, sizeof(pd));
    pd.fd = -1;

    /* 解码交给工作线程，GUI 线程只负责显示 */
    m_decoder = new FrameDecoder(this);
    connect(m_decoder, &FrameDecoder::decoded, this, [this](const QImage &img) {
        m_frames++;
        ui->displaytLabel->setPixmap(QPixmap::fromImage(img));
    });
    m_decoder->start();

    pd.dev_name = (char*)BOARD_VIDEO_DEV;      /* 仅保留字段（本页不再开 V4L2） */

    m_nam = new QNetworkAccessManager(this);
    m_door = new DoorClient(this);
    connect(m_door, &DoorClient::doorOpened, this, &videodisplay::onDoorOpened);
    connect(m_door, &DoorClient::doorStatus, this, &videodisplay::refreshTalkStatus);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this]{
        m_retryPending = false;
        if (m_active) startStream();
    });

    m_callTimer = new QTimer(this);
    m_callTimer->setInterval(1000);
    connect(m_callTimer, &QTimer::timeout, this, &videodisplay::onCallTick);

    connect(ui->backBtn,  &QPushButton::clicked, this, &videodisplay::onBackClicked);

    /* ★★ 2026-10-20 第二次 UI 精简（用户要求「只留最上方一行状态 + 大画面」）：
       页面上现在只有两个控件：
         * statusLabel  —— 唯一上屏文字，居中放在最上方一行
         * displaytLabel—— 画面，1280x656 撑满（原来 872x490）
       「对讲」整块侧栏（sideCard/sideTitle/talkLabel）、通话计时(timerLabel)、
       页面标题(titleLabel)、底部黄色提示条、调试行全部删掉。
       返回首页按钮留着，但**不带任何文字**（改用图标，见 .ui）——否则访客没法结束通话。 */
    ui->backBtn->setIcon(QIcon(":/pic/icons/ic_back.png"));
    ui->backBtn->setIconSize(QSize(28, 28));

    /* 状态文字是**整条 1280x64**，盖在「返回」按钮的同一行上。
       让它不接收鼠标/触摸（事件直接落到下面），并把 backBtn 显式提到最上层，
       这样按钮的点击区域永远只由它自己决定，不会被这一行文字吃掉。 */
    ui->statusLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    ui->backBtn->raise();

    ui->displaytLabel->setAlignment(Qt::AlignCenter);
    /* ★ 不拉伸：解码线程已按 KeepAspectRatio 缩到"能放进标签"的尺寸，
       QLabel 以原始像素居中显示 → 16:9 的 1920x1080 在 1280x656 里是 1166x656，
       左右留窄黑边，绝不横向拉变形（原来 setScaledContents(true) 会硬拉满）。 */
    ui->displaytLabel->setScaledContents(false);
}

videodisplay::~videodisplay()
{
    stopCall();
    if (m_decoder) { m_decoder->stopThread(); m_decoder = nullptr; }
    if (bufrgb) { free(bufrgb); bufrgb = nullptr; }
    delete ui;
}

/*---------------------------------------------------------------------------
 * 进入页面：起拉流 + 计时 + 开门轮询
 *-------------------------------------------------------------------------*/
void videodisplay::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    startCall();
}

void videodisplay::hideEvent(QHideEvent *e)
{
    stopCall();                       /* 返回首页时兜底：从"被隐藏"这条路也能停干净 */
    QWidget::hideEvent(e);
}

void videodisplay::startCall()
{
    if (m_active)
        return;
    m_active = true;
    m_frames = 0;
    m_retryCount = 0;
    m_callSec = 0;
    ui->statusLabel->setText("已接听，正在接通…");
    ui->statusLabel->setStyleSheet("font-size: 26px; font-weight: 600; color: #7DE3C6;");

    m_callTimer->start();

    /* 只拉本机预览端口（av_stream 由首页刚拉起，可能还没 bind 8081 → 内部重试） */
    startStream();

    /* 每 0.5s 轮询本机 av_stream 的开门事件 */
    m_door->startPolling();

    qInfo() << "[talk] 进入可视对讲页：开始拉本机预览 http://" BOARD_STREAM_LOCAL "/live.mjpg";

    /* 自测钩子（等价于点"返回"）：BOARD_AUTO_BACK=秒数，到点自动返回首页。
       现场验收"返回即停流"用；正常使用不设这个环境变量。 */
    if (qgetenv("BOARD_AUTO_BACK").toInt() > 0) {
        const int sec = qgetenv("BOARD_AUTO_BACK").toInt();
        qInfo() << "[talk] 自测钩子 BOARD_AUTO_BACK，" << sec << "秒后自动返回";
        QTimer::singleShot(sec * 1000, this, &videodisplay::onBackClicked);
    }
}

void videodisplay::stopCall()
{
    if (!m_active)
        return;
    m_active = false;
    m_callTimer->stop();
    stopStream();
    m_door->stopPolling();
    ui->statusLabel->setText("已结束");
    ui->statusLabel->setStyleSheet("font-size: 26px; font-weight: 600; color: #9FB0C4;");
    qInfo() << "[talk] 通话结束，共用时" << m_callSec << "秒";
}

/*---------------------------------------------------------------------------
 * 拉本机预览流（av_stream 的 :8081）
 *-------------------------------------------------------------------------*/
void videodisplay::startStream()
{
    if (!m_active)
        return;
    m_buf.clear();
    if (m_frames == 0)
        ui->displaytLabel->setText("正在等待画面…");
    const QString url = QString("http://%1/live.mjpg").arg(BOARD_STREAM_LOCAL);

    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("User-Agent", "RK3506B-Qt/1.0");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

    m_netMode = true;
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &videodisplay::onStreamReadyRead);
    connect(m_reply, &QNetworkReply::finished,  this, &videodisplay::onStreamFinished);
    connect(m_reply, SIGNAL(error(QNetworkReply::NetworkError)),
            this,    SLOT(onStreamError(QNetworkReply::NetworkError)));
}

void videodisplay::stopStream()
{
    m_netMode = false;
    m_retryPending = false;
    if (m_retryTimer) m_retryTimer->stop();

    /* ★ 2026-10-20 修（悬空/重入 bug）：必须先把指针摘下来 + 断开本对象的槽，
       再 abort()。因为 QNetworkReply::abort() 会**同步**发 finished →
       onStreamFinished() 会把 m_reply 置 nullptr 并 deleteLater()，回到这里
       再执行 `m_reply->deleteLater()` 就是对空指针调用（实机 app.log 里实测
       打出过 `QCoreApplication::postEvent: Unexpected null receiver`，
       属未定义行为）。先 disconnect 就能彻底断掉这条重入路径。 */
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) {
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }

    m_buf.clear();
    ui->displaytLabel->setText("画面已关闭");
}

/*---------------------------------------------------------------------------
 * 网络数据到达：抽出完整 JPEG，只显示最新一帧（丢积压 → 实时性好）
 *-------------------------------------------------------------------------*/
void videodisplay::onStreamReadyRead()
{
    if (!m_reply)
        return;
    m_buf.append(m_reply->readAll());

    static const QByteArray SOI = QByteArray::fromHex("ffd8");
    static const QByteArray EOI = QByteArray::fromHex("ffd9");

    QByteArray newest;
    while (true) {
        int s = m_buf.indexOf(SOI);
        if (s < 0) {
            if (m_buf.size() > 1) m_buf = m_buf.right(1);
            break;
        }
        int e = m_buf.indexOf(EOI, s + 2);
        if (e < 0) {
            if (s > 0) m_buf.remove(0, s);
            break;
        }
        newest = m_buf.mid(s, e - s + 2);
        m_buf.remove(0, e + 2);
    }
    if (m_buf.size() > 4 * 1024 * 1024)
        m_buf.clear();

    if (newest.isEmpty())
        return;
    if (m_frames == 0) {
        ui->statusLabel->setText("通话中");
        m_retryCount = 0;
        qInfo() << "[talk] 已收到第一帧画面（av_stream 推流就绪）";
    }
    if (m_decoder) m_decoder->submit(newest, ui->displaytLabel->size());
}

void videodisplay::onStreamFinished()
{
    /* 先把指针接管过来再判断，避免和 stopStream() 互相置空（见 stopStream 注释） */
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) r->deleteLater();
    if (!m_netMode || !m_active)
        return;
    if (m_frames > 0)
        ui->statusLabel->setText("画面已断开，正在重连…");
    if (!m_retryPending && m_active) {
        m_retryPending = true;
        m_retryTimer->start(RETRY_INTERVAL_MS);
    }
}

void videodisplay::onStreamError(QNetworkReply::NetworkError code)
{
    Q_UNUSED(code)
    if (!m_netMode || !m_active)
        return;
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) { r->disconnect(this); r->deleteLater(); }

    /* ★ av_stream 刚被拉起时 :8081 还没 listen（脚本里还要加载 USB 音频模块 +
       等 2 秒），所以这里就是简单的"每秒重试"，绝不退到本机 V4L2
       （摄像头被 av_stream 独占，抢 /dev/video0 只会让两边都黑屏）。 */
    m_retryCount++;
    if (m_retryCount == RETRY_MAX_NOTICE) {
        qWarning() << "[talk] 连续" << m_retryCount << "次拉不到本机预览，"
                   << "请检查 /userdata/av_stream.log 与 /userdata/av_stream_switch.log";
        ui->statusLabel->setText("画面暂时无法显示，正在重试…");
    }
    if (!m_retryPending && m_active) {
        m_retryPending = true;
        m_retryTimer->start(RETRY_INTERVAL_MS);
    }
}

/*---------------------------------------------------------------------------
 * 通话计时 —— ★ 2026-10-20 第二次精简：页面上**不再显示**计时（"其余文字全部去掉"），
 *   只保留 m_callSec 计数（停流时写进 app.log，取证用）。逻辑一行没动。
 *-------------------------------------------------------------------------*/
void videodisplay::onCallTick()
{
    if (!m_active) return;
    m_callSec++;
}

/*---------------------------------------------------------------------------
 * 静音 / 免提 / 开门按钮 —— 按用户要求**全部放在住户终端（网页）那侧**，
 * 门口机这边只做"画面 + 说话"。
 *
 * ★ 2026-10-20 清理：原来这里把 av_stream 上报的**技术状态**逐条显示给访客看
 *   （"全双工 · AEC 开启" / "半双工（回退）" / "下行音频：已收 N 包，正在播放" /
 *    "喇叭：ES8323 就绪 · 麦克风自测：开"）——技术词全部删掉。
 * ★ 2026-10-20 第二次：连「对讲」侧栏一起删了，本函数现在只把**最上方那一行**
 *   维持在「通话中」；JSON 接口/轮询周期完全不变（DoorClient 仍每 0.5s 取 /door）。
 *-------------------------------------------------------------------------*/
void videodisplay::refreshTalkStatus(const QString &json)
{
    Q_UNUSED(json)
    if (m_frames <= 0)
        return;                       /* 画面还没出来时保持「正在接通…」 */
    ui->statusLabel->setText("通话中");
}

/*---------------------------------------------------------------------------
 * 开门：av_stream 报来新的开门事件 → 弹窗「已开门」→ 确认后回首页
 *
 * ★ 2026-10-20 用户明确指示（见 docs/24_修正_开门弹窗不改.md）：
 *   **开门提示弹窗保持原样，不做任何改动**（位置/层级/文案/3 秒自动确认都不动）。
 *   本轮 UI 精简只动"对讲栏 / 底部提示条 / 计时 / 标题"，不碰这里。
 *-------------------------------------------------------------------------*/
void videodisplay::onDoorOpened(qint64 ts, int count)
{
    Q_UNUSED(count)
    QString when;
    if (ts > 0) {
        when = QDateTime::fromSecsSinceEpoch(ts).toString("HH:mm:ss");
    }
    qInfo() << "[door] 收到开门事件 ts=" << ts << " count=" << count;

    QMessageBox box(this);
    box.setWindowTitle("开门成功");
    box.setIcon(QMessageBox::Information);
    box.setText("已开门");
    /* ★ 2026-10-20 清理：原来这里有「门口机 rk3506b-01 已执行开门 / 累计开门 N 次 /
       点击「确定」返回首页（同时结束通话、停止推流）」——设备号、"累计次数"和
       "停止推流"都属于技术信息，访客只看时间就够了。 */
    box.setInformativeText(QString("时间：%1\n\n点击「确定」返回首页")
                           .arg(when.isEmpty() ? QString("--") : when));
    box.setStandardButtons(QMessageBox::Ok);
    /* 自测钩子：BOARD_AUTO_OK=N（秒）自动点"确定"（等价于人点确认）。
       N=1 时沿用旧的 3 秒；N>1 时按 N 秒（现场验收要留时间截图时用）。
       正常使用不设这个环境变量。 */
    const int okSec = qgetenv("BOARD_AUTO_OK").toInt();
    if (okSec > 0) {
        const int delay = (okSec > 1 ? okSec : 3) * 1000;
        qInfo() << "[door] 自测钩子 BOARD_AUTO_OK，" << delay << "ms 后自动确认";
        QTimer::singleShot(delay, &box, [&box]{ box.accept(); });
    }
    box.exec();

    stopCall();
    emit backToHome();               /* 首页负责挂断 + 停 av_stream */
}

/*---------------------------------------------------------------------------
 * 返回：断流 + 通知首页（首页负责挂断与停推流）
 *-------------------------------------------------------------------------*/
void videodisplay::onBackClicked()
{
    qInfo() << "[talk] 按返回：停止画面，通知首页挂断并停流";
    stopCall();
    emit backToHome();
}
