# Arm-linux-Smart-Community - ARM/Linux智慧社区系统

基于Qt框架开发的ARM/Linux平台智慧社区管理系统，集成多种社区服务功能。

## 项目简介

本项目是一个运行在ARM/Linux嵌入式平台上的智慧社区管理系统，使用Qt框架开发。系统集成了社区管理、物业服务、视频监控、天气预报、RFID门禁等多种功能，适用于智能社区、智慧小区等场景。

## 功能特性

### 核心功能模块

#### 1. 用户管理
-  **登录系统** - 用户登录验证
-  **居民管理** - 添加、修改、删除居民信息
-  **个人信息** - 查看和修改个人资料

#### 2. 社区服务
-  **社区新闻** - 发布和查看社区公告
-  **便民服务** - 提供各类便民服务功能
-  **商业中心** - 社区商业信息管理
-  **商户管理** - 商户信息维护
-  **广告发布** - 社区广告展示

#### 3. 智能硬件
-  **RFID读取** - ISO14443A标准RFID卡读取
-  **LED控制** - LED设备控制
-  **视频监控** - V4L2摄像头视频采集和显示
-  **天气信息** - 实时天气查询和显示

#### 4. 实用工具
-  **日历功能** - 日历查看和事件管理
-  **记事本** - 便签和笔记功能
-  **图片查看** - 图片浏览功能
-  **软键盘** - 虚拟键盘输入
-  **自定义按钮** - 自定义按键控件

## 项目结构

```
Arm-linux-Smart-Community/
│
├── 主程序
│   ├── main.cpp                       # 程序入口
│   ├── intelligent_community.cpp/h/ui # 主窗口
│   └── login.cpp/h/ui                # 登录界面
│
├── 功能模块
│   ├── new_owner.cpp/h/ui            # 居民管理
│   ├── mange_owner.cpp/h/ui          # 居民信息管理
│   ├── selfinfo.cpp/h/ui             # 个人信息
│   ├── news.cpp/h/ui                  # 社区新闻
│   ├── community_service.cpp/h/ui    # 社区服务
│   ├── business_center.cpp/h/ui       # 商业中心
│   ├── for_business.cpp/h/ui         # 商户功能
│   ├── mange_business.cpp/h/ui       # 商户管理
│   ├── advertisement.cpp/h/ui        # 广告发布
│   ├── caular.cpp/h/ui               # 日历功能
│   ├── nodepad.cpp/h/ui              # 记事本
│   ├── photoview.cpp/h/ui            # 图片查看
│   └── runningtime.cpp/h/ui          # 运行时间显示
│
├── 硬件接口
│   ├── readRfidId.cpp/h              # RFID读取
│   ├── ISO14443A.h                   # RFID标准头文件
│   ├── LED.cpp/h/ui                  # LED控制
│   ├── v4l2.cpp/h                    # V4L2视频采集
│   └── videodisplay.cpp/h/ui         # 视频显示
│
├── 数据处理
│   ├── connect_database.h            # 数据库连接
│   ├── database_table.h              # 数据库表定义
│   ├── countdata.h                   # 数据统计
│   ├── edit_table_model.cpp/h        # 表格模型
│   └── weatherinfo.cpp/h/ui          # 天气信息
│
├── 界面增强
│   ├── character_calc_mainwindow.cpp/h/ui  # 字符计算主窗口
│   ├── result_mainwindow.cpp/h/ui          # 结果显示窗口
│   ├── keybutton.cpp/h              # 自定义按键
│   ├── softkey.cpp/h/ui             # 软键盘
│   ├── mylabel.cpp/h                # 自定义标签
│   └── qss/                         # QSS样式文件
│
├── 资源文件
│   ├── pic/                          # 图片资源
│   ├── weather/                      # 天气图标
│   ├── res.qrc                       # Qt资源文件
│   └── *.png                         # 界面图片
│
└── intelligent_community.pro         # Qt项目文件
```

## 技术栈

- **开发框架**: Qt 5.x
- **编程语言**: C++
- **操作系统**: ARM/Linux (嵌入式Linux)
- **硬件接口**:
  - V4L2 (Video4Linux2) - 摄像头驱动
  - RFID - 射频识别
  - GPIO - LED控制
- **数据存储**: 数据库 (SQLite/MySQL)
- **网络功能**: HTTP请求 (天气查询)

## 开发环境搭建

### 主机开发环境
- Ubuntu/Linux 或 Windows
- Qt 5.x 或更高版本
- Qt Creator IDE
- 交叉编译工具链 (ARM)

### ARM目标平台
- ARM开发板 (如RK3399, i.MX6等)
- Linux内核 3.x或更高
- 摄像头、RFID读卡器、LED等硬件设备

### 编译步骤

#### 1. 主机编译 (x86)
```bash
# 生成Makefile
qmake intelligent_community.pro

# 编译项目
make

# 运行
./intelligent_community
```

#### 2. ARM交叉编译
```bash
# 使用ARM版本的qmake
/path/to/arm-qmake intelligent_community.pro

# 编译项目
make

# 拷贝到ARM开发板运行
```

## 使用说明

### 1. 系统登录
- 启动程序后进入登录界面
- 输入用户名和密码进行身份验证
- 不同用户有不同的操作权限

### 2. 居民管理
- 管理员可以添加、修改、删除居民信息
- 支持RFID卡绑定
- 居民可以查看和修改个人信息

### 3. RFID门禁
- 使用RFID读卡器读取居民卡片
- 验证身份后控制门锁或记录出入信息

### 4. 视频监控
- 使用V4L2接口采集摄像头视频
- 实时显示监控画面
- 支持多路视频切换

### 5. 社区服务
- 查看社区新闻和公告
- 使用便民服务功能
- 浏览社区商业信息

### 6. 天气查询
- 自动获取当地天气信息
- 显示温度、湿度、天气状况等

## 硬件连接

### RFID读卡器
- 接口: USB或串口
- 标准: ISO14443A
- 功能: 读取Mifare S50/S70卡片

### 摄像头
- 接口: USB
- 驱动: V4L2
- 分辨率: 支持多种分辨率

### LED指示灯
- 接口: GPIO
- 功能: 状态指示、报警提示

## 数据库设计

系统使用数据库存储以下信息：
- 居民信息表
- 商户信息表
- 新闻公告表
- 出入记录表
- 系统配置表

## 项目特色

1. **跨平台** - 支持x86和ARM平台
2. **硬件集成** - 集成多种硬件设备
3. **功能完善** - 涵盖智慧社区主要功能
4. **界面友好** - 触摸屏优化，操作简单
5. **可扩展** - 模块化设计，便于功能扩展

## 注意事项

- ARM平台需要配置正确的交叉编译环境
- 摄像头和RFID设备需要对应的Linux驱动支持
- 数据库需要根据实际平台进行配置
- 天气查询需要网络连接和API支持

## 许可证

本项目仅供学习交流使用。
