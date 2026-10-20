#include "call_client.h"
#include "board_config.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QDebug>

/*---------------------------------------------------------------------------
 * CallClient 实现（★ 2026-10-20 按需推流）
 *
 * 轮询 1 秒一次 GET /api/callstate/<dev>，只关心 state 字段：
 *   ringing  → 继续等（住户终端可能还没接）
 *   accepted → 发 accepted 信号（home 启动 av_stream + 进对讲页）
 *   rejected → 发 rejected（提示"对方已拒绝"3 秒后回首页，全程没推流）
 *   timeout  → 发 timeout（提示"无人接听"）
 *   idle     → 发 idle（通话结束：home 停流 + 回首页，并停止轮询）
 *-------------------------------------------------------------------------*/
CallClient::CallClient(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);                  /* 用户要求：1 秒 */
    connect(m_timer, &QTimer::timeout, this, &CallClient::poll);
}

CallClient::~CallClient()
{
    /* ★ 2026-10-20 加固：先 disconnect 再 abort（abort 会同步发 finished，
       在析构里回调本对象的槽是未定义行为） */
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) { r->disconnect(this); r->abort(); r->deleteLater(); }
}

void CallClient::setState(const QString &s)
{
    if (s == m_state)
        return;
    const QString old = m_state;
    m_state = s;
    qInfo().noquote() << "[call] 呼叫状态" << old << "→" << s;
    emit stateChanged(s);
}

void CallClient::post(const QString &path, const char *what)
{
    QNetworkRequest req(QUrl(QString(BOARD_RELAY_BASE "/api/%1").arg(path)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    QNetworkReply *r = m_nam->post(req, QByteArray("{}"));
    connect(r, &QNetworkReply::finished, r, [r, what]{
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        if (r->error() == QNetworkReply::NoError)
            qInfo().noquote() << "[call]" << what << "成功 code=" << code
                              << QString::fromUtf8(body).left(160);
        else
            qInfo().noquote() << "[call]" << what << "失败:" << r->errorString()
                              << QString::fromUtf8(body).left(160);
        r->deleteLater();
    });
}

void CallClient::ring()
{
    m_active = true;
    m_fails = 0;
    m_seenRinging = false;                      /* ★ 重新计：等第一次确认 ringing */
    m_hangPending = false;
    m_ringAtMs = QDateTime::currentMSecsSinceEpoch();
    qInfo().noquote() << "[call] 访客按呼叫 → POST /api/call/start?dev=" BOARD_DEVICE_ID;
    post(QString("call/start?dev=%1").arg(BOARD_DEVICE_ID), "call/start");
    setState(QStringLiteral("ringing"));        /* 本地先置 ringing，界面立刻有反馈 */
    if (!m_timer->isActive())
        m_timer->start();
    poll();                                     /* 立刻查一次 */
}

void CallClient::accept()
{
    post(QString("call/%1/accept").arg(BOARD_DEVICE_ID), "accept");
}

void CallClient::reject()
{
    post(QString("call/%1/reject").arg(BOARD_DEVICE_ID), "reject");
}

void CallClient::hangup()
{
    if (!m_active && m_state == "idle")
        return;
    qInfo().noquote() << "[call] 板端挂断 → POST /api/call/" BOARD_DEVICE_ID "/hangup";
    m_hangPending = true;                       /* ★ 在真正见到 idle 之前，忽略 accepted */
    post(QString("call/%1/hangup").arg(BOARD_DEVICE_ID), "hangup");
    /* 不要立刻把状态改成 idle：真状态以中转为准（poll 会在一秒内更新），
       这样即使网络抖动也能看到真实结果。但界面要马上回到待机。 */
    m_active = false;
}

void CallClient::stopPolling()
{
    m_timer->stop();
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) { r->disconnect(this); r->abort(); r->deleteLater(); }
}

void CallClient::poll()
{
    if (m_reply)
        return;                                 /* 上一次还没回来就跳过 */
    QNetworkRequest req(QUrl(QString(BOARD_RELAY_BASE "/api/callstate/%1")
                             .arg(BOARD_DEVICE_ID)));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, &CallClient::onPollReply);
}

void CallClient::onPollReply()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (!r)
        return;
    if (r->error() == QNetworkReply::OperationCanceledError) { r->deleteLater(); return; }
    const QByteArray body = r->readAll();
    const bool ok = (r->error() == QNetworkReply::NoError);
    const QString err = r->errorString();
    r->deleteLater();

    if (!ok || body.isEmpty()) {
        if (++m_fails % 10 == 1)
            qInfo().noquote() << "[call] 查呼叫状态失败（继续重试）:" << err;
        return;
    }
    m_fails = 0;

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();
    const QString s = o.value("state").toString(QStringLiteral("idle"));

    if (s != m_lastPoll) {
        qInfo().noquote() << "[call] 轮询到状态:" << s
                          << QString("(active=%1 seenRing=%2 hangPending=%3)")
                                 .arg(m_active).arg(m_seenRinging).arg(m_hangPending);
        m_lastPoll = s;
    }

    /* ★ 竞态保护（2026-10-20 实测踩到）：
     *   ring() 刚 POST 出去时，可能还有一个"上一轮"的 GET 在途，它返回的是
     *   ringing 之前的 idle。旧代码直接把它当"通话结束"，停掉轮询 → 板端永远
     *   看不到后面的 accepted，推流当然起不来。
     *   ★ 但保护窗口必须是**有限的**：如果做成"没见过 ringing 就一直忽略 idle"，
     *     一旦 ring() 时中转其实已经处于 accepted（比如板端重启后又按了一次呼叫），
     *     就永远见不到 ringing，然后通话结束时 idle 被**永久忽略** —— 轮询停不下来、
     *     推流也停不掉（实测就卡成这样，日志里刷了几百行"忽略在途的 idle 响应"）。
     *     所以：只在 ring() 之后 5 秒内忽略在途 idle，超过就认账。 */
    if (s == "ringing") {
        m_seenRinging = true;               /* 确认本轮呼叫真的进了中转 */
        m_hangPending = false;
    }
    if (s == "idle" && m_active && !m_seenRinging
        && (QDateTime::currentMSecsSinceEpoch() - m_ringAtMs) < 5000) {
        qInfo() << "[call] 忽略在途的 idle 响应（ring 后 5 秒内的旧响应）";
        return;
    }
    if (s == "accepted" && m_hangPending) {
        qInfo() << "[call] 忽略在途的 accepted 响应（正在挂断，等 idle）";
        return;
    }

    if (s == "idle") {
        /* 通话结束（可能是住户挂断、也可能是我们刚发的 hangup 生效） */
        setState(QStringLiteral("idle"));
        m_active = false;
        m_hangPending = false;
        m_timer->stop();
        return;
    }
    if (!m_active)
        m_active = true;                        /* 板子重启后仍能接上已存在的通话 */
    setState(s);

    if (s == "accepted") {
        /* 已接听：状态会一直保持到挂断，轮询继续（1s 一次，很便宜） */
    }
}
