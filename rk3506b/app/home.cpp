#include "home.h"
#include "ui_home.h"

#include "board_config.h"
#include "weather_client.h"
#include "videodisplay.h"
#include "call_client.h"

#include <QTimer>
#include <QLabel>
#include <QPixmap>
#include <QDateTime>
#include <QCloseEvent>
#include <QProcess>
#include <QDebug>

/* 板端按需推流脚本（由 av_stream_on.sh / av_stream_off.sh 统一加锁 + 处理
 * wd_guard 的 intentional_stop 标记，Qt 只负责"喊一声"） */
#define SCRIPT_AV_ON  "/userdata/av_stream_on.sh"
#define SCRIPT_AV_OFF "/userdata/av_stream_off.sh"

home::home(QWidget *parent) : QWidget(parent), ui(new Ui::home)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_StyledBackground, true);   /* 让 QSS 的浅色底生效 */

    m_wx = new WeatherClient(this);
    connect(m_wx, &WeatherClient::weatherUpdated, this, &home::onWeather);
    connect(m_wx, &WeatherClient::timeSynced, this, &home::onTimeSynced);
    connect(ui->callBtn, &QPushButton::clicked, this, &home::onCallClicked);

    /* ★ 呼叫状态机客户端：1 秒轮询中转 /api/callstate/<dev> */
    m_call = new CallClient(this);
    connect(m_call, &CallClient::stateChanged, this, &home::onCallState);
    connect(m_call, &CallClient::logText, this, [](const QString &s){ qInfo().noquote() << s; });

    /* ★ 底部动态状态条（贴在提示行下面，不修改 .ui） */
    m_hint = new QLabel(this);
    m_hint->setGeometry(340, 556, 600, 52);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet("font-size: 22px; font-weight: 600; color: #B87500;"
                          "background: rgba(255,236,200,0.9); border-radius: 10px;");
    m_hint->hide();
    m_hintTimer = new QTimer(this);
    m_hintTimer->setSingleShot(true);
    connect(m_hintTimer, &QTimer::timeout, this, [this]{ m_hint->hide(); });

    /* 秒级刷新：只刷界面，不碰网络 */
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &home::tick);
    m_tick->start();

    /* 10 分钟问一次中转（时间 + 天气），用户已确认的间隔 */
    m_net = new QTimer(this);
    m_net->setInterval(10 * 60 * 1000);
    connect(m_net, &QTimer::timeout, this, &home::refreshNet);
    m_net->start();

    tick();
    refreshNet();                    /* 开机立刻取一次 */

    /* 自测钩子：BOARD_AUTO_CALL=1 时开机 N 秒后自动按"访客呼叫"。
       现场验收/无触摸注入手段时用；正常使用不设这个环境变量。 */
    if (qgetenv("BOARD_AUTO_CALL").toInt() > 0) {
        const int delay = qgetenv("BOARD_AUTO_CALL").toInt() * 1000;
        qInfo() << "[home] 自测钩子 BOARD_AUTO_CALL，" << delay << "ms 后自动呼叫";
        QTimer::singleShot(delay, this, &home::onCallClicked);
    }
    /* 自测钩子：BOARD_AUTO_ACCEPT=N 板端代替住户点【接听】（没有浏览器时验收用） */
    if (qgetenv("BOARD_AUTO_ACCEPT").toInt() > 0) {
        const int delay = qgetenv("BOARD_AUTO_ACCEPT").toInt() * 1000;
        QTimer::singleShot(delay, this, [this]{
            qInfo() << "[home] 自测钩子 BOARD_AUTO_ACCEPT，代替住户接听";
            m_call->accept();
        });
    }
}

home::~home()
{
    delete ui;
}

void home::closeEvent(QCloseEvent *e)
{
    if (m_video) m_video->hide();
    QWidget::closeEvent(e);
}

/*---------------------------------------------------------------------------
 * 秒级刷新时间/日期（★ 2026-10-20 清理：删掉了底部"门口机 rk3506b-01 ·
 * 待机（不推流，省带宽/省 CPU）"这类技术状态行——访客不需要看这些）
 *-------------------------------------------------------------------------*/
void home::tick()
{
    const QDateTime dt = m_wx->nowAdj();
    ui->timeLabel->setText(dt.toString("HH:mm:ss"));
    ui->dateLabel->setText(QString("%1年%2月%3日 %4")
                           .arg(dt.date().year())
                           .arg(dt.date().month())
                           .arg(dt.date().day())
                           .arg(QString("星期一星期二星期三星期四星期五星期六星期日")
                                .mid((dt.date().dayOfWeek() - 1) * 3, 3)));
}

void home::refreshNet()
{
    m_wx->refresh();
}

void home::onTimeSynced(qint64 epoch)
{
    Q_UNUSED(epoch)
    m_timeOk = true;
    tick();                                   /* 立刻用新时间刷一次 */
}

void home::onWeather(const QString &text, int tempC, int humidity, const QString &city)
{
    ui->weatherLabel->setText(text.isEmpty() ? "--" : text);
    ui->tempLabel->setText(tempC <= -900 ? "--℃" : QString("%1℃").arg(tempC));
    ui->humiLabel->setText(humidity < 0 ? "湿度 --" : QString("湿度 %1%").arg(humidity));
    applyWeatherIcon(text);
    if (!city.isEmpty() && text != "--")
        ui->weatherLabel->setToolTip(city);
}

void home::applyWeatherIcon(const QString &text)
{
    QString res;
    if (text.contains("雷") || text.contains("雨"))
        res = ":/weather/raining.png";
    else if (text.contains("雪"))
        res = ":/weather/raining.png";
    else if (text.contains("阴") || text.contains("云"))
        res = ":/weather/cloudy.png";
    else if (text.contains("晴"))
        res = ":/weather/sunny.png";
    else if (text == "--")
        res.clear();
    else
        res = ":/weather/cloudy (2).png";

    if (res.isEmpty()) {
        ui->wxIconLabel->clear();
        return;
    }
    QPixmap pm(res);
    if (!pm.isNull())
        ui->wxIconLabel->setPixmap(pm.scaled(ui->wxIconLabel->size(),
                                             Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
}

/*---------------------------------------------------------------------------
 * 呼叫状态提示（首页底部黄条）
 *
 * ★ 2026-10-20 第二次精简：这个黄条是 home 的子控件，showCallHint() 里原来会
 *   raise() 到最上层 —— 对讲页全屏时会**压在画面底部**（用户明确要求删掉）。
 *   现在：只要在对讲页里（m_videoInCall），黄条一律不上屏；
 *   对讲页自己的最上方那一行状态文字负责给访客反馈。
 *-------------------------------------------------------------------------*/
void home::showCallHint(const QString &text, int autoHideMs)
{
    m_hint->setText(text);
    if (m_videoInCall) {                 /* 对讲页全屏：不许任何浮层压画面 */
        m_hint->hide();
        return;
    }
    m_hint->show();
    m_hint->raise();
    if (autoHideMs > 0) {
        m_hintTimer->start(autoHideMs);
    } else {
        m_hintTimer->stop();
    }
}

/*---------------------------------------------------------------------------
 * ★ 按需推流：启/停 av_stream
 *
 * 为什么用脚本而不是直接起进程：
 *   * av_stream_on.sh 里做了"操作锁 + 清 intentional_stop 标记 + wd_guard.pause"，
 *     保证和 wd_guard 不会抢跑出两个实例（历史上双实例抢 /dev/video0 导致 CPU 90%）
 *   * av_stream_off.sh 会写 /tmp/av_stream.intentional_stop，让 wd_guard 知道
 *     "这是正常停止，不是挂了"，绝不重新拉起
 * Qt 以 root 运行，startDetached 不需要额外权限。
 *-------------------------------------------------------------------------*/
void home::startAvStream()
{
    if (m_avStarted) {
        qInfo() << "[av] av_stream 已由本端启动过，忽略重复启动";
        return;
    }
    qInfo() << "[av] 启动板端推流：" << SCRIPT_AV_ON;
    const bool ok = QProcess::startDetached(QString::fromLatin1(SCRIPT_AV_ON),
                                            QStringList());
    if (!ok)
        qWarning() << "[av] av_stream_on.sh 启动失败（脚本不存在？）";
    m_avStarted = true;
}

void home::stopAvStream()
{
    qInfo() << "[av] 停止板端推流：" << SCRIPT_AV_OFF;
    const bool ok = QProcess::startDetached(QString::fromLatin1(SCRIPT_AV_OFF),
                                            QStringList());
    if (!ok)
        qWarning() << "[av] av_stream_off.sh 启动失败（脚本不存在？）";
    m_avStarted = false;
}

/*---------------------------------------------------------------------------
 * 视频页
 *-------------------------------------------------------------------------*/
void home::ensureVideoPage()
{
    if (m_video)
        return;
    m_video = new videodisplay(this);          /* 子控件：铺满首页，返回时直接 hide */
    m_video->setGeometry(rect());
    connect(m_video, &videodisplay::backToHome, this, &home::onVideoBack);
}

void home::openVideoPage()
{
    ensureVideoPage();
    m_tick->stop();                            /* 对讲页全屏，首页定时器先歇着省 CPU */
    m_video->setGeometry(rect());
    m_video->show();
    m_video->raise();
    m_video->setFocus();
    m_videoInCall = true;
}

/*---------------------------------------------------------------------------
 * 访客按「访客呼叫」：**只通知中转**，不开流、不进对讲页
 *-------------------------------------------------------------------------*/
void home::onCallClicked()
{
    if (m_call->state() == "ringing" || m_call->state() == "accepted") {
        qInfo() << "[home] 已经在呼叫/通话中，忽略重复按键";
        return;
    }
    qInfo() << "[home] 访客呼叫：通知中转，等待住户接听（此时板端不推流）";
    ui->callBtn->setEnabled(false);
    showCallHint("正在呼叫住户…");
    m_call->ring();
    tick();
    /* 15 秒还没接听就解除按钮锁定，允许重按（真正 30 秒超时由中转判定） */
    QTimer::singleShot(15000, this, [this]{
        if (m_call->state() == "ringing") {
            ui->callBtn->setEnabled(true);
        }
    });
}

/*---------------------------------------------------------------------------
 * ★ 呼叫状态变化 → 起/停推流 + 切页面
 *-------------------------------------------------------------------------*/
void home::onCallState(const QString &state)
{
    qInfo().noquote() << "[home] 呼叫状态变化:" << state;

    if (state == "accepted") {
        ui->callBtn->setEnabled(false);
        showCallHint("住户已接听，正在接通…");
        /* 先起推流，再进页（页内会每 1 秒重试 :8081，直到 av_stream 就绪） */
        startAvStream();
        openVideoPage();
        QTimer::singleShot(4000, this, [this]{
            if (m_videoInCall) showCallHint("通话中");
        });
        tick();
        return;
    }

    if (state == "rejected") {
        ui->callBtn->setEnabled(true);
        showCallHint("对方已拒绝", 3000);
        tick();
        return;
    }

    if (state == "timeout") {
        ui->callBtn->setEnabled(true);
        showCallHint("无人接听", 3000);
        tick();
        return;
    }

    if (state == "ringing") {
        ui->callBtn->setEnabled(false);
        showCallHint("正在呼叫住户…");
        tick();
        return;
    }

    if (state == "idle") {
        ui->callBtn->setEnabled(true);
        /* 通话结束（住户挂断 / 板端返回）：停推流 + 回首页 */
        if (m_videoInCall) {
            m_videoInCall = false;
            if (m_video) m_video->hide();
            stopAvStream();
            m_tick->start();
            refreshNet();
            if (m_hint->isVisible() && m_hintTimer->isActive() == false)
                m_hint->hide();
        } else if (m_avStarted) {
            stopAvStream();      /* 兜底：起过流但没进页（异常路径） */
        }
        m_hint->hide();
        tick();
        return;
    }
    tick();
}

/*---------------------------------------------------------------------------
 * 对讲页返回：挂断 + 停流 + 回首页
 *-------------------------------------------------------------------------*/
void home::onVideoBack()
{
    qInfo() << "[home] 从对讲页返回首页：挂断并停止推流";
    if (m_video) m_video->hide();
    m_videoInCall = false;
    stopAvStream();                 /* 立即停流（不等中转确认，隐私优先） */
    m_call->hangup();               /* 再通知中转 → idle（终端也停拉流） */
    m_hint->hide();
    ui->callBtn->setEnabled(true);
    tick();
    m_tick->start();
    refreshNet();                   /* 通话可能很久，回来顺手刷新一次时间/天气 */
}
