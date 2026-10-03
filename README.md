# AP3216C-Linux-Driver

基于 Linux Input 子系统的 AP3216C 接近与环境光传感器驱动，适配正点原子 i.MX6ULL 开发板及其 Linux 4.1.15 环境。

驱动通过 I²C 读取传感器数据，将接近状态作为 `EV_SW / SW_FRONT_PROXIMITY` 事件上报，并通过 sysfs 提供参数配置和原始数据读取。项目包含设备树片段和用户态监视程序，可用于类手机通话场景的贴近检测实验。

## 功能特性

- **标准 Input 事件**：接近状态通过 `/dev/input/eventX` 提供给用户程序；`1` 表示靠近，`0` 表示远离。
- **中断与工作队列**：GPIO 中断处理函数只调度 workqueue，I²C 访问、数据解析和事件上报在可休眠上下文中完成。
- **运行时电源管理**：靠近时使用仅 PS 的高频采样模式；远离后经过 500 ms autosuspend 延迟，切换为低频 PS+ALS 巡检模式。
- **系统休眠支持**：挂起时关闭中断并让芯片进入 power-down 模式，恢复时重建寄存器配置。
- **并发与资源管理**：Mutex 保护共享状态及并发 I²C 访问；私有数据和 Input 设备由 devm 管理。
- **sysfs 配置**：支持 PS 阈值、测量间隔和 LED 电流比例设置，并可读取 PS、ALS 原始值。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 开发板 | 正点原子 i.MX6ULL，或按原理图调整设备树的兼容板卡 |
| 传感器 | AP3216C，示例接在 I²C1 地址 `0x1e` |
| 内核 | 与开发板运行版本及配置匹配的 Linux 4.1.15 源码 |
| 工具链 | ARM 交叉编译工具链，默认前缀为 `arm-linux-gnueabihf-` |

目标内核需要启用对应的 I²C 控制器、Input、evdev 和 PM 功能。

## 快速开始

### 1. 配置设备树

将 [imx6ull-ap3216c.dtsi](./imx6ull-ap3216c.dtsi) 放入内核的 `arch/arm/boot/dts/` 目录，并在实际启动的板级 DTS 中包含：

```dts
#include "imx6ull-ap3216c.dtsi"
```

该片段使用 I²C1、`GPIO1_IO01` 下降沿中断和 `0x1e` 设备地址。合入前确认 I²C1 的 SCL/SDA 已配置，`GPIO1_IO01` 未被 `GBC_KEY` 等节点占用，且 `0x1e` 地址没有重复的设备节点。随后重新编译并部署实际使用的板级 DTB。

### 2. 编译

`KDIR` 指向与开发板运行内核匹配、且已完成配置的内核源码顶层目录：

```sh
make KDIR=/path/to/linux-imx-rel_4.1.15_2.1.0_alientek
```

构建完成后得到 `ap3216c_drv.ko` 和 `ap3216c_monitor`。工具链前缀不同时，可在命令行覆盖 `CROSS_COMPILE`。清理构建产物：

```sh
make KDIR=/path/to/linux-imx-rel_4.1.15_2.1.0_alientek clean
```

### 3. 部署与验证

将新 DTB、内核模块和应用程序部署到开发板，从新 DTB 启动后执行：

```sh
depmod
modprobe ap3216c_drv.ko
cat /proc/bus/input/devices
./ap3216c_monitor
```

监视程序会按 Input 设备名查找 `/dev/input/eventX`。也可以手动指定节点：

```sh
./ap3216c_monitor /dev/input/eventX
```

输出示例（事件编号和时间戳以实际运行结果为准）：

```text
Device: /dev/input/eventX (ap3216c-proximity)
Initial state: FAR (0)
Monitoring proximity events. Press Ctrl+C to exit.
[123.456789] Proximity: NEAR (1)
[125.123456] Proximity: FAR (0)
```

程序启动时使用 `EVIOCGSW` 读取当前开关状态，之后持续监听事件；按 `Ctrl+C` 退出。

## 实现说明

接近事件的数据路径为：`GPIO 中断 → IRQ 处理函数 → workqueue → I²C 读取 → Input 事件`。工作队列读取并清除中断标志，解析 PS 数据，仅在接近状态变化时上报事件。sysfs、工作队列和 PM 回调中的共享状态与寄存器访问由 Mutex 保护。

Runtime PM 的 `active` 状态对应仅 PS 的高频采样；`suspended` 状态对应低频 PS+ALS 巡检，**不表示芯片物理断电**。靠近时 ALS 不再更新；系统级挂起才让芯片进入 power-down 模式。

## sysfs 接口

驱动 probe 成功后，属性位于 `/sys/bus/i2c/devices/X-001e/`；`X` 是实际 I²C 总线编号。

### `proximity/`

| 属性 | 权限 | 说明 |
| --- | --- | --- |
| `ps_raw_data` | 只读 | 10 位 PS 原始值；数据无效时返回 `Invalid data.` |
| `ps_threshold_low` | 读写 | PS 低阈值，范围 0–1023，不能高于高阈值 |
| `ps_threshold_high` | 读写 | PS 高阈值，范围 0–1023，不能低于低阈值 |
| `ps_measure_interval` | 读写 | 远离时的测量周期，输入范围 113–7200 ms，由驱动换算为寄存器档位 |
| `ps_led_current_ratio` | 读写 | 红外 LED 电流比例档位，范围 0–3 |

### `ambient_light/`

| 属性 | 权限 | 说明 |
| --- | --- | --- |
| `als_raw_data` | 只读 | 16 位 ALS 原始值 |

## 项目文件

| 文件 | 用途 |
| --- | --- |
| [ap3216c_drv.c](./ap3216c_drv.c) | 内核驱动，包含 I²C、IRQ、Input、sysfs 和 PM 回调 |
| [ap3216c_monitor.c](./ap3216c_monitor.c) | 用户态 Input 事件监视程序 |
| [imx6ull-ap3216c.dtsi](./imx6ull-ap3216c.dtsi) | i.MX6ULL 设备树片段 |
| [Makefile](./Makefile) | 内核外部模块与应用程序构建规则 |

## 许可证

代码文件采用 `GPL-2.0-only`，以各文件中的 SPDX 标识为准。
