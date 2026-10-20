/******************************************************************************
 * board_config.h  —— 板级适配唯一入口（RK3506B / ATK-DLRK3506B）
 *
 * 原工程是跑在 GEC6818(x86/ARM 教学板) 上的，硬件相关的东西散落在各个文件里：
 *   - LED   : /dev/gz13_led_drv   (教学板自带的字符设备驱动)
 *   - RFID  : /dev/ttySAC1        (教学板串口名)
 *   - 摄像头: /dev/video7
 *   - 数据库: ./intell_com.db     (相对当前工作目录)
 *
 * 移植到 RK3506B 后，这些全部集中到本文件，其它 .cpp 只引用宏。
 * 以后再换板子，只改这一个文件即可。
 *
 * 注意：本文件必须只包含"可以被 C/C++ 同时预处理"的内容，
 *       因为它会被 readrfidid.h(纯 C 风格) 包含。
 ******************************************************************************/
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ---------------------------------------------------------------------------
 * 1. 运行期数据目录
 *    RK3506B 出厂固件的根文件系统是 NAND 上的 UBIFS，虽然可读写但只剩几 MB，
 *    而且整机升级会覆盖它。所以所有"运行期生成的文件"（数据库、截图、日志）
 *    统一放到 /userdata（独立分区，约 245MB 空闲）。
 * ------------------------------------------------------------------------- */
#define BOARD_DATA_DIR        "/userdata/smart_community"

/* 数据库与"是否已建表"标记文件 */
#define BOARD_DB_DIR          BOARD_DATA_DIR
#define BOARD_DB_FILE         BOARD_DATA_DIR "/intell_com.db"
#define BOARD_PHOTO_DB_FILE   BOARD_DATA_DIR "/photolist.db"
#define BOARD_CREATE_TABLE_FLAG BOARD_DATA_DIR "/createTable.txt"
#define BOARD_PHOTO_DIR       BOARD_DATA_DIR "/photo"

/* ---------------------------------------------------------------------------
 * 2. 屏幕 / 显示
 *    720×1280 竖装面板，weston 通过 transform=rotate-90 转成逻辑 1280×720。
 *    见 rk3506b/scripts/board_setup_display.sh
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * 3. RFID 读卡器（ISO14443A 串口读头，9600 8N1）
 *    ATK-DLRK3506B 底板 A 座的 UART2：TX=GPIO1_B3(36脚)、RX=GPIO1_C2(27脚)
 *    对应 Linux 设备 /dev/ttyS2。若用 USB 转串口模块，改成 /dev/ttyUSB0。
 * ------------------------------------------------------------------------- */
#define BOARD_RFID_DEV        "/dev/ttyS2"

/* ---------------------------------------------------------------------------
 * 4. USB 摄像头（UVC，V4L2）
 *    插上 USB 摄像头后通常枚举为 /dev/video0；用
 *        v4l2-ctl --list-devices
 *    确认后再改这里。
 * ------------------------------------------------------------------------- */
#define BOARD_VIDEO_DEV       "/dev/video0"
#define BOARD_VIDEO_W         640
#define BOARD_VIDEO_H         480

/* ---------------------------------------------------------------------------
 * 4.1 视频监控页数据源（新增）
 *
 * 摄像头被推流程序 av_stream 独占，本机再开 V4L2 会失败（画面变空白）。
 * 所以监控页默认向**流媒体中转服务**拉 MJPEG，这样：
 *     * 板子和网页看到的是同一路画面
 *     * 不再抢摄像头，和推流程序共存
 * 连不上中转服务时自动回退本机 V4L2 预览。
 *
 *   BOARD_STREAM_HOST：中转服务地址（"IP:HTTP端口"）
 *   BOARD_STREAM_DEV ：设备号（与板端 /etc/default/av_stream 里的 DEVICE_ID 一致）
 * ------------------------------------------------------------------------- */
#define BOARD_STREAM_HOST     "192.168.137.94:8080"
#define BOARD_STREAM_DEV      "rk3506b-01"

/*  板子本机预览端口：推流程序 av_stream 自带（--http-port，默认 8081）。
 *  摄像头只能被一个进程采集，所以由推流程序统一对外分发；板子自己优先
 *  取本机这一路（不经网络 → 延迟最低），取不到再退回中转服务。 */
#define BOARD_STREAM_LOCAL    "127.0.0.1:8081"

/* ---------------------------------------------------------------------------
 * 4.2 门禁可视对讲（M1~M7 改造新增）
 *
 *   BOARD_DEVICE_ID   : 与 /etc/default/av_stream 的 DEVICE_ID 一致
 *   BOARD_RELAY_BASE  : 中转服务 HTTP 根（时间/天气/通话开始结束都走它）
 *   BOARD_DOOR_KEY    : 开门 HMAC-SHA256 的共享密钥（与中转 SC_DOOR_KEY 一致）
 *                       注：真正的"开门"只在**住户终端（网页）**发起，板端不做签名请求
 *
 *   流水线：网页"开门" → POST 中转 /api/door（nonce+ts+HMAC-SHA256）
 *           → 中转下发 T_CONTROL{"cmd":"open"} → av_stream 记录事件
 *           → 本机 :8081/door → Qt 每 0.5s 轮询到 → 弹窗"已开门" → 确认回首页
 * ------------------------------------------------------------------------- */
#define BOARD_DEVICE_ID       "rk3506b-01"
#define BOARD_RELAY_BASE      "http://" BOARD_STREAM_HOST
#define BOARD_DOOR_KEY        "rk3506b-door-2026"

/* ---------------------------------------------------------------------------
 * 5. 网络 / 天气
 *
 * 原工程用的是聚合数据(juhe.cn)接口，需要自己申请 KEY，而且工程里带的那把
 * KEY 早已失效——界面上"温度/湿度"一直是空的。移植后换成 wttr.in：
 *   http://wttr.in/<英文城市名>?format=j1
 * 优点：**纯 HTTP + 免 KEY**（板子的 busybox wget 不支持 https，Qt 也不用带
 * SSL 依赖），板子上 curl/wget 都能直接验证通不通。
 *
 * 想换城市：BOARD_WEATHER_CITY 改成英文城市名（Beijing/Shanghai/Guangzhou…），
 *           BOARD_WEATHER_CITY_CN 改成界面上显示的中文名。
 * ------------------------------------------------------------------------- */
#define BOARD_WEATHER_URL      "http://wttr.in/"
#define BOARD_WEATHER_CITY     "Shenzhen"     /* wttr.in 用的英文城市名 */
#define BOARD_WEATHER_CITY_CN  "深圳"          /* 界面上显示的中文名 */

/* ---------------------------------------------------------------------------
 * 6. WiFi 默认连接（板端脚本用，见 rk3506b/board/wifi_connect.sh）
 *    Qt 程序本身不负责连 WiFi，连网由系统脚本在开机时完成。
 * ------------------------------------------------------------------------- */
#define BOARD_WIFI_SSID        "cjh"
#define BOARD_WIFI_PASS        "12345678"

#endif /* BOARD_CONFIG_H */
