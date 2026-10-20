# USB 音频（snd-usb-audio）交叉编译与部署交付包

RK3506B / ATK-DLRK3506，内核 **6.1.118**（板端 `uname -r` 实测），armv7l。
目标：让板子认出 USB 麦克风（ALSA card 2），板载 ES8323（card 0）不受影响。

## 1. 版本对齐（最关键的一步，已核对一致）

| 项目 | 板端 | SDK |
|---|---|---|
| 内核版本 | `6.1.118`（`uname -a`） | `kernel-6.1/Makefile`：VERSION 6 / PATCHLEVEL 1 / SUBLEVEL 118 |
| 编译时间 | `Thu Oct 1 20:22:08 CST 2026` | `vmlinux`/`System.map`/`Module.symvers` 时间戳 = 10 月 1 日 20:22 |
| 构建号 | `#3` | `.version` = `3` |
| 编译器 | `arm-none-linux-gnueabihf-gcc 10.3.1 20210621`（`/proc/version`） | `CONFIG_CC_VERSION_TEXT` 同一串 |
| 主机 | `cjh@cjh-virtual-machine`（`/proc/version`） | 就是这台 VM 编的 |
| vermagic | `6.1.118 SMP preempt mod_unload ARMv7 thumb2 p2v8`（取自板端已有 `uvcvideo.ko`） | 本次编出的 4 个 .ko 全部为同一串 |

SDK 里 `kernel-6.1/Module.symvers`（619 KB）、`vmlinux`、`System.map` 都在 —— 就是**板端当前内核那一次编译**的产物，因此不需要重编内核，直接对它编模块即可。

工具链（必须用这个，不是 buildroot 的 `arm-buildroot-linux-gnueabihf-`）：
```
SDK/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
```

## 2. 内核配置改动（只改了 5 行，原文件备份为 `.config.bak_usbaudio`）

```
> CONFIG_SND_HWDEP=m                                        (新增，被 select)
> CONFIG_SND_RAWMIDI=m                                      (新增，被 select)
< # CONFIG_SND_USB_AUDIO is not set
> CONFIG_SND_USB_AUDIO=m
> CONFIG_SND_USB_AUDIO_USE_MEDIA_CONTROLLER=y
```
`CONFIG_SND=y / SND_PCM=y / SND_USB=y / BITREVERSE=y / MEDIA_CONTROLLER=y` 原本就满足；`CONFIG_MODVERSIONS` 未开（所以符号 CRC 不校验，但仍用 SDK 现成的 `Module.symvers` 做 modpost）。

命令：
```bash
cd $SDK/kernel-6.1
cp -a .config .config.bak_usbaudio
./scripts/config --file .config --module SND_USB_AUDIO --module SND_HWDEP --module SND_RAWMIDI
make ARCH=arm CROSS_COMPILE=<tc-> olddefconfig        # kernelrelease 仍为 6.1.118
make ARCH=arm CROSS_COMPILE=<tc-> modules_prepare
make ARCH=arm CROSS_COMPILE=<tc-> M=sound/core modules
make ARCH=arm CROSS_COMPILE=<tc-> M=sound/usb KBUILD_EXTRA_SYMBOLS=$PWD/sound/core/Module.symvers modules
```
（没有编整个内核，也没有替换板端内核镜像。）

## 3. 产物

| 文件 | 大小 | vermagic | depends | md5 |
|---|---|---|---|---|
| `snd-hwdep.ko` | 175,684 | 6.1.118 SMP preempt mod_unload ARMv7 thumb2 p2v8 | — | 584b314b…01bfc9 |
| `snd-rawmidi.ko` | 298,432 | 同上 | — | ee64a473…447ebf |
| `snd-usbmidi-lib.ko` | 245,396 | 同上 | snd-rawmidi | c0ff6370…568842 |
| `snd-usb-audio.ko` | 3,285,196 | 同上 | snd-usbmidi-lib, snd-hwdep | 317cbe86…c0b2a0 |

4 个 .ko 的全部 undefined 符号都在 `Module.symvers + sound/core + sound/usb` 的导出集合里（`comm -23` 比对，缺失 0 个）。

## 4. 部署与加载顺序

板端 `/userdata/modules/`（不要放 rootfs，rootfs 只剩 2.6 MB）：

```
insmod /userdata/modules/snd-hwdep.ko
insmod /userdata/modules/snd-rawmidi.ko
insmod /userdata/modules/snd-usbmidi-lib.ko
insmod /userdata/modules/snd-usb-audio.ko
```
或一条命令：`/userdata/modules/load_usb_audio.sh`（幂等，日志 `/userdata/modules/load_usb_audio.log`）。

结果：`/proc/asound/cards` 出现 `2 [Device]: USB-Audio - USB PnP Sound Device`（C-Media 08bb:2902，UAC1，单声道 S16_LE，仅 44100/48000）。

## 5. 持久化

`/etc/init.d/S98av_stream` 的 `start_stream()` 里、`setsid` 之前插入两行：

```sh
    # ★ USB 音频模块（snd-usb-audio + 依赖）：不加载的话 USB 麦克风不会被识别
    [ -x /userdata/modules/load_usb_audio.sh ] && /userdata/modules/load_usb_audio.sh
```
备份：`/etc/init.d/S98av_stream.bak_20261020_110719`、`/userdata/modules/S98av_stream.bak_20261020_110719`。
（`S98av_stream.orig/.new/.patch` 是本目录里的同一份改动，便于比对/回滚。）

验证：把 4 个模块 `rmmod` 掉后执行 `/etc/init.d/S98av_stream restart`，加载日志显示 4 个 `insmod ... OK`、card 2 重新出现，`arecord -l` 仍能看到 USB 麦。

## 6. 录音实测（板端 arecord → VM ffmpeg）

```
arecord -D hw:2,0     -f S16_LE -c 1 -r 48000 -d 3 /tmp/usbmic.wav   # 原生率
arecord -D plughw:2,0 -f S16_LE -c 1 -r 16000 -d 3 /tmp/usbmic.wav   # 任务里的 16k 写法(需插值)
```
| 场景 | mean_volume | max_volume |
|---|---|---|
| USB 麦，Mic 增益 0 | -63.5 dB | -45.3 dB |
| USB 麦，Mic 增益 100%（+23.81 dB）@48000 | -48.9 dB | -36.5 dB |
| USB 麦，Mic 增益 100% @16000(plughw) | -49.2 dB | -35.1 dB |
| 板载 ES8323 基线（历史，ALC 开+增益满） | -20.5 dB | -1.3 dB |
| 板载 ES8323 基线（历史，ALC 关） | -31.7 dB | -24.2 dB |

`astats`：DC offset ≈ 0.00015、Flat factor 0.000000 → 是真实变化的信号，不是数字静音/直流。
注意：测试时房间里没人说话，录到的是环境底噪；增益从 0 拉到满，电平跟着涨 15 dB，说明模拟通路是通的。
**要对人声做最终验收**：让一个人对着麦说话再录 3 秒，正常应到 -25 dB 以上（max > -12 dB）。

> 板载麦在 av_stream 运行期间被独占（`arecord -D hw:0,0` → `Device or resource busy`），所以板载麦用的是历史基线，没有做同时刻 A/B。

## 7. 已知坑

- **`/etc/init.d/S98av_stream restart` 与 `S99wd_guard` 抢跑**：stop 之后 wd_guard 5 秒内会把 av_stream 拉起来，restart 的 start 又起一个 → 两个实例，第二个一直报「摄像头被其它进程占用」。
  用 `/userdata/safe_restart_av_stream.sh`（先 touch `wd_guard.pause`，重启完再删）可避免。
- USB 麦卡号不固定（本机是 card 2）。换口/换机器后先 `arecord -l` 确认，再决定 `-D hw:N,0`。
- 该 USB 声卡只支持 44100/48000，`hw:2,0 -r 16000` 会报 `Sample format non available`；要 16 kHz 必须用 `plughw:2,0`。
- 本包里的 `.ko` 只能配 **6.1.118 + 同一份 .config（SMP/preempt/thumb2…）** 的内核；换内核必须重编。
