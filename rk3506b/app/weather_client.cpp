#include "weather_client.h"
#include "board_config.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QDebug>
#include <QtGlobal>

#if defined(Q_OS_LINUX)
#  include <sys/time.h>
#  include <unistd.h>
#endif

WeatherClient::WeatherClient(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);

    /* 取数超时兜底：板端网络慢/中转没起来时 6 秒就放弃，绝不能把界面卡住
       （Qt 5.9 的 QNetworkRequest 还没有 setTransferTimeout，所以自己计时） */
    m_timeoutTime = new QTimer(this);
    m_timeoutTime->setSingleShot(true);
    m_timeoutTime->setInterval(6000);
    connect(m_timeoutTime, &QTimer::timeout, this, [this]{
        if (m_timeReply) { m_timeReply->abort(); }
    });
    m_timeoutWx = new QTimer(this);
    m_timeoutWx->setSingleShot(true);
    m_timeoutWx->setInterval(8000);
    connect(m_timeoutWx, &QTimer::timeout, this, [this]{
        if (m_wxReply) { m_wxReply->abort(); }
    });
}

QDateTime WeatherClient::nowAdj() const
{
    return QDateTime::currentDateTime().addSecs(m_offsetSec);
}

void WeatherClient::refresh()
{
    if (!m_timeReply) {
        m_timeReply = m_nam->get(QNetworkRequest(QUrl(QString(BOARD_RELAY_BASE "/api/time"))));
        m_timeoutTime->start();
        connect(m_timeReply, &QNetworkReply::finished, this, &WeatherClient::onTimeReply);
    }
    if (!m_wxReply) {
        m_wxReply = m_nam->get(QNetworkRequest(QUrl(QString(BOARD_RELAY_BASE "/api/weather"))));
        m_timeoutWx->start();
        connect(m_wxReply, &QNetworkReply::finished, this, &WeatherClient::onWeatherReply);
    }
}

void WeatherClient::onTimeReply()
{
    QNetworkReply *r = m_timeReply;
    m_timeReply = nullptr;
    m_timeoutTime->stop();
    if (!r) return;
    const QByteArray body = r->readAll();
    const QVariant code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    r->deleteLater();

    if (r->error() != QNetworkReply::NoError || code.toInt() != 200) {
        qInfo() << "[time] 取中转时间失败:" << r->errorString();
        return;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        qInfo() << "[time] 解析失败:" << pe.errorString();
        return;
    }
    const qint64 epoch = (qint64)doc.object().value("epoch").toDouble();
    if (epoch <= 0) return;
    applyTime(epoch);
}

void WeatherClient::applyTime(qint64 epoch)
{
    m_epoch = epoch;
    const qint64 localEpoch = QDateTime::currentDateTime().toSecsSinceEpoch();
    m_offsetSec = epoch - localEpoch;

    /* 偏移超过 2 秒才动系统时钟（避免每次刷新都 settimeofday 抖一下） */
#if defined(Q_OS_LINUX)
    if (getuid() == 0 && !m_clockSet && qAbs(m_offsetSec) > 2) {
        struct timeval tv;
        tv.tv_sec = (time_t)epoch;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) == 0) {
            m_clockSet = true;
            m_offsetSec = 0;
            qInfo() << "[time] 已把系统时钟校到中转时间" << epoch;
        } else {
            qInfo() << "[time] settimeofday 失败（继续用偏移显示）";
        }
    }
#endif
    qInfo() << "[time] 中转时间已同步 epoch=" << epoch << " 偏移=" << m_offsetSec << "s";
    emit timeSynced(epoch);
}

void WeatherClient::onWeatherReply()
{
    QNetworkReply *r = m_wxReply;
    m_wxReply = nullptr;
    m_timeoutWx->stop();
    if (!r) return;
    const QByteArray body = r->readAll();
    const QVariant code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    r->deleteLater();

    if (r->error() != QNetworkReply::NoError || code.toInt() != 200) {
        qInfo() << "[weather] 取中转天气失败:" << r->errorString();
        emit weatherUpdated("--", -999, -1, "");
        return;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        emit weatherUpdated("--", -999, -1, "");
        return;
    }
    const QJsonObject o = doc.object();
    if (!o.value("ok").toBool(false)) {
        emit weatherUpdated("--", -999, -1, "");
        return;
    }
    const QString text = o.value("text").toString("--");
    const int temp = o.contains("temp_c") && !o.value("temp_c").isNull()
                     ? (int)o.value("temp_c").toDouble() : -999;
    const int humi = o.contains("humidity") && !o.value("humidity").isNull()
                     ? (int)o.value("humidity").toDouble() : -1;
    emit weatherUpdated(text, temp, humi, o.value("city").toString());
}
