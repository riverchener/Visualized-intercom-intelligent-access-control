#ifndef CALL_CLIENT_H
#define CALL_CLIENT_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/*---------------------------------------------------------------------------
 * CallClient —— 门禁「按需推流」的呼叫状态机客户端（★ 2026-10-20 新增）
 *
 * 为什么需要它：新需求是"平时完全不出流，只有通话期间有视频和声音"。
 * 板端不再常开 av_stream，而是由**住户接听**这个事件来驱动：
 *
 *   访客按「访客呼叫」 → 本类 POST /api/call/start?dev=X
 *        中转状态 → ringing（终端网页弹【接听】【拒绝】，30 秒超时）
 *   住户点【接听】     → 中转状态 → accepted
 *        本类 1 秒轮询 GET /api/callstate/<dev> 看到 accepted
 *        → home 启动 av_stream（/userdata/av_stream_on.sh）并进对讲页
 *   挂断 / 返回首页 / 板端主动结束
 *        → 本类 POST /api/call/<dev>/hangup → idle
 *        → home 停 av_stream（/userdata/av_stream_off.sh）并回首页
 *
 * 状态取值：idle / ringing / accepted / rejected / timeout
 * 也支持板端调试：accept()/reject() 可直接代替住户点按钮（现场无浏览器时用）。
 *-------------------------------------------------------------------------*/
class CallClient : public QObject
{
    Q_OBJECT
public:
    explicit CallClient(QObject *parent = nullptr);
    ~CallClient();

    QString state() const { return m_state; }
    bool isActive() const { return m_active; }

    /* 访客按「访客呼叫」→ POST /api/call/start（成功后开始 1s 轮询） */
    void ring();
    /* 板端主动结束（点"返回"/开门后回首页）→ POST /api/call/<dev>/hangup */
    void hangup();
    /* 调试用：代替住户点【接听】/【拒绝】 */
    void accept();
    void reject();

    void stopPolling();

signals:
    /* 状态变化（只在**真的变化**时发一次，避免重复触发开关流） */
    void stateChanged(const QString &state);
    /* 日志/诊断（写 qInfo，也便于现场抓 app.log） */
    void logText(const QString &text);

private slots:
    void poll();
    void onPollReply();

private:
    void post(const QString &path, const char *what);
    void setState(const QString &s);

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_reply = nullptr;   /* 轮询在途（避免堆请求） */
    QString m_state = QStringLiteral("idle");
    QString m_lastPoll;                 /* 上一次轮询到的原始状态（只用于打印变化） */
    bool m_active = false;              /* 是否处于一次呼叫流程中（ring 之后） */
    bool m_seenRinging = false;         /* 本轮是否已确认过 ringing（防在途 idle 误判） */
    bool m_hangPending = false;         /* 已发 hangup，等 idle（期间忽略 accepted） */
    qint64 m_ringAtMs = 0;              /* ring() 的时刻：只在它之后 5 秒内忽略"在途 idle" */
    int  m_fails = 0;
};

#endif // CALL_CLIENT_H
