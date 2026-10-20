#ifndef DOOR_CLIENT_H
#define DOOR_CLIENT_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/*---------------------------------------------------------------------------
 * DoorClient —— 板端「开门事件」轮询器
 *
 * av_stream 收到中转下发的 T_CONTROL{cmd:"open"} 后记一笔本地事件，并在
 * 本机 :8081 上暴露 GET /door → {"count":N,"last_ts":T,"last_iso":"..."}
 * Qt 不直连中转（门口机只信任本机推流程序），每 0.5s 轮询，count 变大就弹窗。
 *
 * ★ 2026-10-20：原来的 notifyVisitStart()/notifyVisitEnd()（中转访客留影）
 *   已删除 —— 中转不再做任何录像/留影，相关接口（api/visit 那组）也删了。
 *   通话生命周期改由 CallClient（call_client.h）驱动 api/call 那组接口。
 *-------------------------------------------------------------------------*/
class DoorClient : public QObject
{
    Q_OBJECT
public:
    explicit DoorClient(QObject *parent = nullptr);
    ~DoorClient();

    void startPolling();        /* 进入对讲页时调 */
    void stopPolling();         /* 返回/离开时调 */

signals:
    /* 发现**新的**一次开门（真实事件，不是历史记录） */
    void doorOpened(qint64 ts, int count);
    /* 每次轮询拿到的原始 /door JSON（对讲页用来刷新"对讲状态"只读文本） */
    void doorStatus(const QString &json);

private slots:
    void poll();
    void onDoorReply();

private:
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_reply = nullptr;
    bool m_haveBaseline = false;   /* 第一次轮询只记基线，不弹历史事件 */
    int  m_lastCount = 0;
};

#endif // DOOR_CLIENT_H
