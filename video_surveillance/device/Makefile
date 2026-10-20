#=============================================================================
# video_surveillance/device/Makefile —— 设备端推流 + 可视对讲程序
#
# 交叉编译（在 Ubuntu 虚拟机里执行）：
#   make            # 默认用 SDK 的 buildroot 工具链
#   make clean
#
# 依赖（板端 rootfs 已自带，编译时用 sysroot 里的头和库）：
#   libjpeg / libturbojpeg / alsa-lib / speexdsp / pthread / m
#   ★ libspeexdsp：M4 全双工 AEC 用。SDK 的 buildroot 已经编了 speexdsp
#     （sysroot/usr/lib/libspeexdsp.so + sysroot/usr/include/speex/speex_echo.h），
#     板子 rootfs 里也有 /usr/lib/libspeexdsp.so.1.5.2，所以直接 -lspeexdsp 即可，
#     **不需要**自己从源码交叉编译。
#=============================================================================
SDK      ?= $(HOME)/atk_dlrk3506_linux6.1_release_v1.3.1_20260326
HOSTDIR  ?= $(SDK)/buildroot/output/alientek_rk3506/host
CROSS    ?= $(HOSTDIR)/bin/arm-buildroot-linux-gnueabihf-
SYSROOT  ?= $(HOSTDIR)/arm-buildroot-linux-gnueabihf/sysroot

CC       = $(CROSS)gcc
CFLAGS   = -O2 -Wall -Wextra -std=gnu99 --sysroot=$(SYSROOT) \
           -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS  = --sysroot=$(SYSROOT) -lpthread -ljpeg -lturbojpeg -lasound -lspeexdsp -lm

TARGET   = av_stream

all: $(TARGET)

$(TARGET): av_stream.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "--- 产物 ---"
	@ls -l $(TARGET)

clean:
	rm -f $(TARGET) *.o

# 本机（x86）快速语法检查，需要 host 版 speexdsp（apt install libspeexdsp-dev）
host:
	gcc -O2 -Wall -std=gnu99 -o $(TARGET)_host av_stream.c \
	    -lpthread -ljpeg -lturbojpeg -lasound -lspeexdsp -lm

.PHONY: all clean host
