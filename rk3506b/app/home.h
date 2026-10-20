#ifndef HOME_H
#define HOME_H

#include <QWidget>
#include <QDateTime>

class WeatherClient;
class videodisplay;
class CallClient;
class QTimer;
class QLabel;

namespace Ui {
class home;
}

/*---------------------------------------------------------------------------
 * home —— 门禁可视对讲「首页」（M1 + ★ 2026-10-20 按需推流改造）
 *
 * 内容（1280x720）：
 *   左上：时间（秒级刷新）+ 日期星期      右上：天气 / 温度 / 湿度
 *   中间：一个大的「访客呼叫」按钮
 *   底部：一行状态提示（呼叫中 / 对方已拒绝 / 无人接听 / 通话中）
 *
 * ★ 新的呼叫流程（用户 2026-10-20 需求，优先级最高）：
 *   按「访客呼叫」→ **不直接开流**，只通知中转把状态置 ringing
 *      → 住户终端网页弹【接听】【拒绝】（30 秒超时）
 *      → 接听(accepted)：启动 /userdata/av_stream_on.sh（板端开始推流）
 *                        + 进 videodisplay 对讲页（终端同时开始拉流）
 *      → 拒绝(rejected)：只提示"对方已拒绝"3 秒 → 回首页（**全程不推流**）
 *      → 超时(timeout) ：提示"无人接听"3 秒 → 回首页（**全程不推流**）
 *      → 挂断/返回(idle)：/userdata/av_stream_off.sh 停止推流 + 回首页
 *
 * 时间/天气全部来自中转代理（板端不直连外网）：
 *   /api/time 每 10 分钟取一次（顺便校准本机时钟偏移）
 *   /api/weather 同样 10 分钟一次，失败显示 "--"
 *-------------------------------------------------------------------------*/
class home : public QWidget
{
    Q_OBJECT

public:
    explicit home(QWidget *parent = nullptr);
    ~home();

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void onCallClicked();          /* 访客呼叫 → 通知中转（ringing），等住户接听 */
    void onVideoBack();            /* 对讲页返回 → 回首页（并挂断） */
    void tick();                   /* 1s：刷新时间/日期 */
    void refreshNet();             /* 10min：向中转取时间+天气 */
    void onWeather(const QString &text, int tempC, int humidity, const QString &city);
    void onTimeSynced(qint64 epoch);

    void onCallState(const QString &state);   /* ★ 呼叫状态变化（accepted/idle/…） */

private:
    void openVideoPage();
    void ensureVideoPage();
    void applyWeatherIcon(const QString &text);
    void showCallHint(const QString &text, int autoHideMs = 0);  /* 底部状态提示 */

    /* ★ 按需推流：执行板端脚本启/停 av_stream（root 身份，QProcess::startDetached） */
    void startAvStream();
    void stopAvStream();

    Ui::home *ui;
    WeatherClient *m_wx = nullptr;
    videodisplay *m_video = nullptr;
    CallClient *m_call = nullptr;
    QTimer *m_tick = nullptr;
    QTimer *m_net = nullptr;
    QTimer *m_hintTimer = nullptr;
    QLabel *m_hint = nullptr;      /* 动态创建的底部状态条（不改进 .ui 文件） */
    bool m_timeOk = false;
    bool m_videoInCall = false;    /* true=当前在对讲页（决定要不要停流） */
    bool m_avStarted = false;      /* true=本端启动过 av_stream */
};

#endif // HOME_H
