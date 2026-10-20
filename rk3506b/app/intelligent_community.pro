#-------------------------------------------------
# 门禁可视对讲 / 访客呼叫系统 —— 板端 Qt 应用（M1~M7 改造版）
#
#   原「智慧社区」的业务页面（登录/社区服务/新闻/业主管理/记事本/秒表/
#   天气页/软键盘/相册/IME 插件…）已整体移到 archive/ 下，不再参与编译，
#   详见 docs/20_改造方案_门禁可视对讲.md 与 docs/21_门禁对讲改造进度.md
#
#   RK3506B 交叉编译用 SDK buildroot 里的 host qmake，见 ../../scripts/build_app.sh
#-------------------------------------------------

QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = intelligent_community
TEMPLATE = app

# 板级宏集中在这个头文件里
HEADERS += board_config.h

DEFINES += QT_DEPRECATED_WARNINGS

CONFIG += c++11

# ---- 应用本体：首页 + 可视对讲页 + 三个网络客户端 ----
SOURCES += \
        main.cpp \
        home.cpp \
        videodisplay.cpp \
        v4l2.cpp \
        weather_client.cpp \
        door_client.cpp \
        call_client.cpp

HEADERS += \
        home.h \
        videodisplay.h \
        v4l2.h \
        weather_client.h \
        door_client.h \
        call_client.h

FORMS += \
        home.ui \
        videodisplay.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    res.qrc
