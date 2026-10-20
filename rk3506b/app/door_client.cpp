#include "door_client.h"
#include "board_config.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QDebug>

DoorClient::DoorClient(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setInterval(500);            /* 用户要求：每 0.5s 轮询一次 */
    connect(m_timer, &QTimer::timeout, this, &DoorClient::poll);
}

DoorClient::~DoorClient()
{
    /* ★ 2026-10-20 加固：先 disconnect 再 abort。
       abort() 会**同步**发 finished → onDoorReply()（本对象的槽）会在析构过程里
       被回调，属于未定义行为；断开连接就没有这条路径了。 */
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) { r->disconnect(this); r->abort(); r->deleteLater(); }
}

void DoorClient::startPolling()
{
    m_haveBaseline = false;               /* 每次进页面重新取基线（历史开门不弹窗） */
    m_lastCount = 0;
    m_timer->start();
    poll();                               /* 立刻取一次，基线早点建立 */
}

void DoorClient::stopPolling()
{
    m_timer->stop();
    /* 同上：先摘指针 + 断连接，避免 abort() 的同步 finished 重入 */
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (r) { r->disconnect(this); r->abort(); r->deleteLater(); }
}

void DoorClient::poll()
{
    if (m_reply) return;                  /* 上一次还没回来就跳过（不堆请求） */
    QNetworkRequest req(QUrl(QString("http://%1/door").arg(BOARD_STREAM_LOCAL)));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, &DoorClient::onDoorReply);
}

void DoorClient::onDoorReply()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (!r) return;
    /* 主动 abort（离开页面/弹窗时）不要再去读设备，否则 Qt 会打印
       "QIODevice::read: device not open" */
    if (r->error() == QNetworkReply::OperationCanceledError) { r->deleteLater(); return; }
    const QByteArray body = r->readAll();
    const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = (r->error() == QNetworkReply::NoError) && code == 200;
    r->deleteLater();
    if (!ok || body.isEmpty())
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return;
    const QJsonObject o = doc.object();
    const int count = o.value("count").toInt(0);
    const qint64 ts = (qint64)o.value("last_ts").toDouble(0);

    emit doorStatus(QString::fromUtf8(body));      /* 对讲页刷新只读状态 */

    if (!m_haveBaseline) {                /* 第一次：只记基线 */
        m_haveBaseline = true;
        m_lastCount = count;
        qInfo() << "[door] 基线 count=" << count << " last_ts=" << ts;
        return;
    }
    if (count > m_lastCount) {
        m_lastCount = count;
        qInfo() << "[door] 检测到开门事件 count=" << count << " ts=" << ts;
        emit doorOpened(ts, count);
    }
}
