#ifndef VIDEODISPLAY_H
#define VIDEODISPLAY_H

#include <QWidget>
#include "v4l2.h"
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QByteArray>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include <QImageReader>
#include <QBuffer>

class DoorClient;
class QMessageBox;

namespace Ui {
class videodisplay;
}

/*---------------------------------------------------------------------------
 * videodisplay —— 门禁「可视对讲」页（★ 2026-10-20 按需推流版 + UI 精简版）
 *
 * 与原版的差别（都是用户明确要求的）：
 *   * **只有住户接听后才会进本页**：由 home 的 CallClient 看到 callstate=accepted
 *     时打开，本页不再负责"呼叫"这件事（呼叫/挂断都在 home 的 CallClient 里）
 *   * 进页只拉**本机预览端口** http://127.0.0.1:8081/live.mjpg
 *     （av_stream 由 home 通过 /userdata/av_stream_on.sh 刚拉起，需要 2~4 秒，
 *      所以这里每 1 秒重试，直到出画面）
 *   * 不再向 av_stream 发 /mode?fps=30|5（按需推流下 av_stream 只在通话期间存在，
 *     整个生命周期都按 /etc/default/av_stream 的 FPS=30 跑，没有"待机帧率"概念）
 *   * 不再通知中转"访客留影"（中转已经不做任何录像/留影，接口已删除）
 *   * 页面只有两块：**最上方一行状态文字**（statusLabel）+ **大画面**（displaytLabel）；
 *     「对讲」侧栏、通话计时、标题、底部提示条、调试行全部删除（2026-10-20 第二次精简）
 *   * 开门提示弹窗 **保持原样**（用户指示见 docs/24_修正_开门弹窗不改.md）
 *   * 删除：本机 V4L2 兜底（摄像头由 av_stream 独占，抢 /dev/video0 只会两边都黑屏）
 *-------------------------------------------------------------------------*/

/* FrameDecoder：把"最新一帧 JPEG"解码成 QImage（工作线程，只保留最新帧） */
class FrameDecoder : public QThread
{
    Q_OBJECT
public:
    explicit FrameDecoder(QObject *parent = nullptr) : QThread(parent) {}
    void submit(const QByteArray &jpg, const QSize &target) {
        QMutexLocker lk(&m_m);
        m_jpg = jpg; m_target = target; m_has = true;
        m_wait.wakeAll();
    }
    void stopThread() {
        { QMutexLocker lk(&m_m); m_quit = true; m_wait.wakeAll(); }
        wait(1000);
    }
signals:
    void decoded(const QImage &img);
protected:
    void run() override {
        forever {
            QByteArray jpg; QSize target;
            {
                QMutexLocker lk(&m_m);
                if (m_quit) return;
                while (!m_has && !m_quit) m_wait.wait(&m_m, 200);
                if (m_quit) return;
                jpg = m_jpg; target = m_target; m_has = false;
            }
            QBuffer b(&jpg); b.open(QIODevice::ReadOnly);
            QImageReader r(&b, "JPEG");
            const QSize src = r.size();
            if (!src.isValid()) continue;
            r.setScaledSize(src.scaled(target, Qt::KeepAspectRatio));   /* 解码即缩 */
            const QImage img = r.read();
            if (!img.isNull()) emit decoded(img);        /* 跨线程 → 队列连接，安全 */
        }
    }
private:
    QMutex m_m; QWaitCondition m_wait;
    QByteArray m_jpg; QSize m_target; bool m_has = false, m_quit = false;
};

class videodisplay : public QWidget
{
    Q_OBJECT

public:
    explicit videodisplay(QWidget *parent = nullptr);
    ~videodisplay();

signals:
    void backToHome();               /* 通知首页"我要回去了"（首页负责挂断+停流） */

public slots:
    void startStream();              /* 进入页面自动调用（只拉本机 :8081） */
    void stopStream();               /* 返回时调用：断流 */
    void onStreamReadyRead();
    void onStreamFinished();
    void onStreamError(QNetworkReply::NetworkError code);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private slots:
    void onBackClicked();
    void onDoorOpened(qint64 ts, int count);
    void onCallTick();
    /* 把 av_stream 的 /door JSON 渲染成顶部那一行「通话中」（没开关按钮） */
    void refreshTalkStatus(const QString &json);

private:
    void startCall();                /* 进页：起拉流 + 计时 + 开门轮询 */
    void stopCall();                 /* 离页：全停（幂等） */

    Ui::videodisplay *ui;
    pass_data pd;                    /* 保留（v4l2.cpp 仍参与编译，本页不再使用） */
    unsigned char *bufrgb = nullptr;

    /* --- 网络 MJPEG（本机预览端口） --- */
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buf;
    int  m_frames = 0;
    bool m_netMode = false;
    bool m_retryPending = false;
    int  m_retryCount = 0;           /* 连续重试次数（用来给用户提示） */
    QTimer *m_retryTimer = nullptr;

    /* --- 通话状态（只读展示；静音/免提/开门都在网页终端那侧） --- */
    bool m_active = false;
    int  m_callSec = 0;
    QTimer *m_callTimer = nullptr;
    DoorClient *m_door = nullptr;

    FrameDecoder *m_decoder = nullptr;
};

#endif // VIDEODISPLAY_H
