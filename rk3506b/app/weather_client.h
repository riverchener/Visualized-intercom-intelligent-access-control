#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#include <QObject>
#include <QString>
#include <QDateTime>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/*---------------------------------------------------------------------------
 * WeatherClient —— 板端取"时间 + 天气"的唯一出口
 *
 * 设计要点（用户已确认）：
 *   * 板子**不直连外网**：所有取数都问中转代理（VM 有外网）
 *       GET http://<中转>/api/time     → {epoch, date, time, weekday, iso, tz}
 *       GET http://<中转>/api/weather  → {ok, text, temp_c, humidity, city, cached}
 *   * 中转侧天气 10 分钟缓存，板端也就 10 分钟才问一次（见 home.cpp 的定时器）
 *   * 失败一律不弹窗、不阻塞界面：时间保持本机自走，天气显示 "--"
 *   * 板子没有 RTC 电池（重启回到 1970），所以拿到中转 epoch 后：
 *       - 算出"本机时钟偏移量"，界面显示用偏移修正后的时间（秒级刷新，不额外请求）
 *       - 有 root 权限时顺手 settimeofday 一次，让 :8081/door 的时间戳/日志也可信
 *-------------------------------------------------------------------------*/
class WeatherClient : public QObject
{
    Q_OBJECT
public:
    explicit WeatherClient(QObject *parent = nullptr);

    /* 立刻取一次（时间 + 天气）；内部异步，随时可调 */
    void refresh();

    /* 中转 epoch（0 = 还没同步到）与"本机时钟→中转时钟"的偏移秒数 */
    qint64 serverEpoch() const { return m_epoch; }
    qint64 offsetSec()   const { return m_offsetSec; }
    bool   timeOk()      const { return m_epoch > 0; }

    /* 用偏移修正后的当前时间（界面上所有时间都用它，保证和中转一致） */
    QDateTime nowAdj() const;

signals:
    void timeSynced(qint64 epoch);
    void weatherUpdated(const QString &text, int tempC, int humidity, const QString &city);

private slots:
    void onTimeReply();
    void onWeatherReply();

private:
    void applyTime(qint64 epoch);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_timeReply = nullptr;
    QNetworkReply *m_wxReply = nullptr;
    QTimer *m_timeoutTime = nullptr;
    QTimer *m_timeoutWx = nullptr;
    qint64 m_epoch = 0;        /* 中转 epoch 秒 */
    qint64 m_offsetSec = 0;    /* 中转时钟 - 本机时钟 */
    bool   m_clockSet = false;
};

#endif // WEATHER_CLIENT_H
