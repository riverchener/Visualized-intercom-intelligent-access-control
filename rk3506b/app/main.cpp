/******************************************************************************
 * main.cpp —— 门禁可视对讲 / 访客呼叫系统（RK3506B）
 *
 * 产品形态（M1 改造后）：
 *   开机 → 直接进「首页」（时间 / 日期 / 天气 / 温度 / 湿度 + 一个大的访客呼叫按钮）
 *   按「访客呼叫」→ 可视对讲页（自动开流 + 双向对讲 + 开门）
 *   无登录、无社区业务模块（那些已归档到 archive/community_business/）
 *
 * 板级移植点（保留原工程的结论）：
 *   1) 工作目录切到 /userdata/smart_community（rootfs 的 UBIFS 太小）
 *   2) 皮肤从资源里加载（qss/mystyle.qss）
 *   3) 时间/天气/温湿度都问中转代理（板子不直连外网，见 weather_client.cpp）
 ******************************************************************************/
#include "home.h"
#include "videodisplay.h"
#include "board_config.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <cstdlib>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QDir().mkpath(BOARD_DATA_DIR);
    QDir::setCurrent(BOARD_DATA_DIR);

    /* 定制皮肤在程序运行之前设置 */
    QFile file(":/qss/mystyle.qss");
    if (file.open(QIODevice::ReadOnly)) {
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
        file.close();
    }

    /*------------------------------------------------------------------------
     * 调试用"直接进某一页"（BOARD_START_PAGE=home|video），不带就进首页。
     *------------------------------------------------------------------------*/
    const char *page = getenv("BOARD_START_PAGE");
    const QString startPage = page ? QString::fromLocal8Bit(page) : QString();

    if (startPage == "video") {
        videodisplay *w = new videodisplay();
        w->show();
        return a.exec();
    }

    home *h = new home();
    h->show();
    return a.exec();
}
