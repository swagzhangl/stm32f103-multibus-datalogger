# 多总线数据采集终端 · 固件工程

> STM32F103C8T6 ｜ 全程寄存器手写（无 HAL） ｜ I2C(BME280) + SPI(W25Q64) + UART ｜ FreeRTOS 三任务
> 生成日期：2026-09-17 ｜ 对应《执行计划 · 求职优先 4 周线（极限版 + SPI）》

---

## 0 · 先看这一节：三件必须先知道的事

### ⚠️ ① 工程状态：**已完整写完并通过编译**，但**尚未上板验证**

| 项 | 状态 |
|---|---|
| 代码完整性 | W1–W4 全部模块写完（含 SPI 四模式自检、32 B 双 CRC 记录闭环、PC 端校验工具） |
| **CubeIDE 工程** | ✅ **已生成 `.project` / `.cproject` / `.settings`，可直接 `File > Import` 打开**，见 `docs/09` |
| 编译（命令行） | ✅ `make` 实测通过，零错误零告警（`arm-none-eabi-gcc 11.3.1`） |
| 编译（CubeIDE） | ✅ 用 CubeIDE 托管构建的**原样参数**编了**全部 26 个文件** → 0 错误 0 告警，链接通过 |
| 资源占用 | 命令行 `-Og`：Flash **26.8 KB / 64 KB（42%）**；CubeIDE Debug `-O0`：**35.9 KB（56%）**；RAM 均 **11.4 KB / 20 KB（57%）** |
| 上板验证 | ❌ **还没做**。逻辑分析仪波形、实际温湿压读数、栈水位实测都要你上板跑 |
| 简历数字 | 凡带 **[实测后填]** 的地方**一个都没填** —— 必须你测完再填，不要用我给的示例值 |

### ⚠️ ② 你的项目其实不是"一行没写"

交接说明写着"计划已定稿，代码一行未写"。实际上 `D:\stm32workplace\rtos_logger\` 里
**已经有一个能编译、能跑 FreeRTOS 的阶段 0 工程**（LED 心跳 + 监控任务打堆余量/栈余量），
而且 i2c_soft / bme280 / spi_soft / w25q64 / crc16 / buzzer / recorder(16 B) 七个驱动都已写好。

所以这份交付的定位是：**在这个已有基础上补齐业务层、修正错误、把格式定稿成 32 B。**

本次实际改了什么、发现了什么 bug，见 [§5](#5--这次实际改了什么与计划书的偏差)。

### ⚠️ ③ 它与执行计划有 5 处事实性偏差，其中 1 处会直接影响你的验收方法

最重要的一处：**执行计划说"回环自测能复现 CPHA 配错导致的移位"，这个说法不成立。**
详见 [§5.1](#51-偏差-1重要spi-四模式的回环自测看不到-cpha-配错计划书这一条写错了)。

---

## 1 · 工程结构

```
firmware/
├── README.md                       ← 你正在看的文件
├── .project  .cproject             ← ★ CubeIDE 工程文件（可直接 File > Import 打开）
├── .settings/                      ← ★ CubeIDE 设置（已锁 UTF-8 编码）
├── Makefile                        ← 命令行构建
├── STM32F103C8TX_FLASH.ld          ← 链接脚本（64 KB Flash / 20 KB RAM）
│
├── Core/
│   ├── Inc/
│   │   ├── board.h                 ← ★ 引脚 / 时钟 / 阈值 总表（改硬件先改这里）
│   │   ├── app_config.h            ← ★ 编译期开关（互斥锁对照实验、栈档位等）
│   │   ├── FreeRTOSConfig.h        ← FreeRTOS 配置（原生 API 版，非 CMSIS 包装）
│   │   ├── clock_init.h
│   │   ├── delay_us.h              ← DWT 微秒延时 + 单调计时（超时判定用）
│   │   ├── uart_dbg.h              ← USART1 TX + RX 环形缓冲
│   │   ├── log.h                   ← 互斥锁保护的打印
│   │   ├── i2c_soft.h   bme280.h   ← I2C 总线 + 传感器
│   │   ├── spi_soft.h   w25q64.h   ← SPI 总线 + Flash
│   │   ├── crc16.h      recorder.h ← CRC + 32 B 记录格式
│   │   ├── buzzer.h     app_tasks.h  self_test.h
│   ├── Src/                        ← 与上面 Inc 一一对应的 .c
│   └── Startup/startup_stm32f103xb.s
│
├── Drivers/CMSIS/                  ← 从本机 STM32Cube_FW_F1_V1.8.7 复制（未改动）
├── Middlewares/Third_Party/FreeRTOS/  ← FreeRTOS V10.3.1 内核（未改动）
│
├── docs/
│   ├── 01_W1_时钟树与串口.md
│   ├── 02_W2_I2C与BME280.md
│   ├── 03_W3_FreeRTOS三任务.md
│   ├── 04_W4_SPI四模式与32B记录闭环.md
│   ├── 05_验收手册.md              ← ★ 上板时照着这个一步步做
│   ├── 06_面试问答.md              ← ★ 对着简历四条逐条准备
│   ├── 07_同步回CubeIDE工程.md      ← 把源码并进 rtos_logger（可选，一般不需要了）
│   ├── 08_怎么打开工程与烧录.md      ← ★ CubeIDE 打不开？烧录怎么烧？看这份
│   └── 09_用CubeIDE开发与调试.md     ← ★ 导入工程 / Ctrl+B / F11 调试 / 两套构建的差异
│
├── tools/                          ← PC 端校验工具（已用假日志跑通）
│   ├── 00_白话使用说明.md           ← ★ 不懂 Python 就先看这份
│   ├── run.py                      ← ★ 傻瓜菜单：python tools\run.py，然后选数字
│   ├── multibus.py                 ← 算法参考实现（CRC + BME280 补偿 + 记录格式）
│   ├── crc16_modbus.py             ← CRC 自检，和固件开机横幅对答案
│   ├── verify_bme280.py            ← 温湿压"PC 独立复算逐位一致"验证
│   ├── parse_records.py            ← 32 B 记录逐条双 CRC 校验 + 导出 CSV
│   ├── stack_budget.py             ← 栈水位 → 重分配 → 算压缩比
│   ├── serial_capture.py           ← 串口抓包 + 自动发命令
│   ├── make_sample_log.py          ← 造假日志，没板子也能先跑通工具链
│   └── README.md
│
├── logs/                           ← 验收证据（抓下来的日志都存这里，别删）
├── build/                          ← 【Makefile 产物】fw.elf / fw.hex / fw.bin / fw.map
└── Debug/                          ← 【CubeIDE 产物】firmware.elf / .map / .list（首次编译后生成）
```

---

## 2 · 编译

工具链就在你本机（CubeIDE 自带）：
```
D:\STM32CubeIDE_1.13.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.0.202305231506\tools\bin
```

### 方式 A：CubeIDE（★ 推荐，可直接打开 + 单步调试）

`firmware\` **本身就是一个标准 STM32CubeIDE 工程**，不用"转换"、不用"导入 Makefile 工程"：

```
File > Import > General > Existing Projects into Workspace
   root directory = D:\work\2026-09-17-15-24-45\firmware   → Finish
```

导入后 `Ctrl+B` 编译、`F11` 调试、`Ctrl+F11` 直接跑。
**细节和坑见 [`docs/09_用CubeIDE开发与调试.md`](docs/09_用CubeIDE开发与调试.md)。**

产物在 `Debug\`，注意 **CubeIDE 默认只产出 `.elf`，不产出 `.hex`**（这是正常的）。

### 方式 B：命令行（最快，适合批量/自动化验证）

```bash
cd firmware
make            # 产出 build/fw.elf + fw.hex + fw.bin
make size       # 看 Flash / RAM 占用
make check-src  # 检查"新增了 .c 但没加进 C_SOURCES"（两节都空 = 一致）
make symbols    # 导出符号表，确认中断函数没有重复定义
make flash      # 直接用 CubeProgrammer 烧进去
```

> **两套构建共用同一份源码，但产物目录不同**（`Debug\` vs `build\`），
> 而且 CubeIDE 自动扫描源文件、Makefile 是手写清单 —— 新增 `.c` 时只有 Makefile 需要手工维护。
> 完整差异对照见 [`docs/09`](docs/09_用CubeIDE开发与调试.md) 第 3 节。

### 编译期开关（`Core/Inc/app_config.h`）

| 开关 | 默认 | 作用 |
|---|---|---|
| `APP_MUTEX_ENABLE` | 1 | 改 **0** → 关掉互斥锁，用来复现"输出串台"，做 W3 D4 对照实验 |
| `APP_STACK_MEASURED` | 0 | 改 **1** → 用实测水位 ×1.5 的栈尺寸（**要你先把实测值填进去**） |
| `APP_UART_VERBOSE` | 1 | 改 **0** → 从"每条都打"变成"每 60 条打一行"，跑 24 h 长测时用 |
| `APP_SELFTEST_ON_BOOT` | 0 | 改 **1** → 开机自动跑 W4 自检 |

---

## 3 · 烧录

### ★ 最省事：让菜单帮你选

```bash
python tools\run.py      →  选 8
```

它会**自动找到 CubeProgrammer**、**列出所有编译产物让你选**（`Debug\firmware.elf` 和
`build\fw.hex` 都行）、并在"源码比固件新"时提醒你先重新编译。
**这一步完全不需要 CubeIDE。**

### 方式 A：CubeIDE 里点 Run
`Ctrl+F11`（Run）或 `F11`（Debug），第一次会让你选探头 → ST-LINK。

### 方式 B：命令行手工烧

```bash
# CubeIDE 编译的产物
STM32_Programmer_CLI.exe -c port=SWD freq=4000 -d Debug\firmware.elf -v -rst

# 或 make 编译的产物
STM32_Programmer_CLI.exe -c port=SWD freq=4000 -d build\fw.hex -v -rst
```

**用 `-d` 而不是 `-w`**：`.hex` / `.elf` 自带地址信息，`-d`（download）会照着文件里的地址写；
`-w`（write）需要你自己另给起始地址。加上 `-v` 才会**回读比对**。

⚠️ **烧完没输出？先拔插一次 USB。**（调试器复位后内核可能停在暂停态，程序没被执行）

### 接线（见 `board.h` 的完整表）

| 功能 | 引脚 | 接到 |
|---|---|---|
| LED | PC13 | 板载，无需接线 |
| USART1 TX | PA9 | CH340 的 RXD |
| USART1 RX | PA10 | CH340 的 **TXD**（本版新增，用于下发命令） |
| I2C SCL / SDA | PB6 / PB7 | BME280 的 SCL / SDA（**模块自带的上拉必须接**） |
| SPI CS/SCK/MISO/MOSI | PA4 / PA5 / PA6 / PA7 | W25Q64 的 CS / CLK / DO / DI |
| 蜂鸣器 | PB0 | 有源蜂鸣器 I/O |

⚠️ **串口助手的编码要设成 UTF-8**，否则固件输出的中文提示会乱码
（机器可读的 `#TAG,...` 和 `REC,...` 行全是 ASCII，不受影响）。

---

## 4 · 验收流程（对照执行计划）

详细操作步骤见 **[`docs/05_验收手册.md`](docs/05_验收手册.md)**，这里是总览：

| 阶段 | 验收标准（来自执行计划） | 怎么看 |
|---|---|---|
| **W1** | 串口助手（115200 8N1）收到 printf 输出；逻辑分析仪抓到 UART 波形且 1 bit ≈ 8.68 µs；LED 能闪 | 开机横幅 + 抓 PA9 |
| **W1** | 时钟树真的跑到 72 MHz | 横幅 `#CLK,got:` 与 `#CLK,expect:` 对齐 |
| **W2** | chip id = 0x60；逻辑分析仪抓到完整 I2C 事务；温湿压合理；**PC 端独立复算逐位一致** | 命令 `i` / `c` / `m` + `verify_bme280.py` |
| **W3** | 3 个任务跑起来，队列能传数据；**去掉锁能复现问题、加回锁能解决** | `APP_MUTEX_ENABLE` 0↔1 对照 |
| **W3.5** | 连续 24 h 不死机；有栈水位实测表 | 命令 `s`，或每 60 s 自动一行 `#STK` |
| **W4** | 四种模式波形能指着讲清；**写入 100 条全读回一致**；PC 端逐条双 CRC 全过；跨页跨扇区不出错 | 命令 `t` / `r` / `v` + `parse_records.py` |

### 串口命令表（直接决定你能不能做完验收）

| 命令 | 作用 |
|---|---|
| `h` | 打印命令表 |
| `s` | 打印一次栈水位 / 堆余量 / 丢包计数 |
| `c` | 打印 33 个出厂校准系数（`#CALIB` 行，PC 复算用） |
| `m` | 触发一次测量，打印 `#RAW` / `#CMP` / `#OUT`（PC 复算用） |
| `r` | 把已存记录按 `REC,<idx>,<64位HEX>` 全倒出来（PC 校验用） |
| `v` | 全量回读已存记录并重算双 CRC，打印分类统计 |
| `t` | 跑完整 W4 自检（SPI 四模式 / W25Q64 分模式 ID / 记录闭环 / 断电三情况） |
| `i` | 读 BME280 chip id（故意和采集任务抢 I2C 锁） |
| `e` | 清空记录区（**要连发两次、且两次间隔 ≥2 秒**；第二次才执行。间隔不足会回 `#ERASE,TOO_SOON` 并保持待确认） |
| `p` | 打印运行状态汇总 |

### PC 端验证 —— **不懂 Python 也能做**

最简单的方式：一句话启动菜单，然后选数字。

```bash
cd firmware
python tools\run.py
```

菜单左侧的选项对应 `docs/05_验收手册.md` 的各个章节，跑完会告诉你「该看哪一行、什么算通过」，
并且**不需要你写任何 Python**。零基础说明见 [`tools/00_白话使用说明.md`](tools/00_白话使用说明.md)。

> **你本机环境已就绪**：`python` → `C:\Python311\python.exe`（3.11.9），**pyserial 3.5 已装**，
> 所以不用装任何东西。

想手动敲命令的话（等菜单用熟了，或要写进文档里）：

```bash
cd firmware/tools
python crc16_modbus.py --selftest                                  # 应输出 PASS (0x4B37)
python make_sample_log.py ../logs/demo.txt --records 40 --with-powerloss  # 没板子先演练
python verify_bme280.py ../logs/demo.txt
python serial_capture.py --port COM5 --out ../logs/log.txt --seq t --stop-marker "W4 自检结束"
python serial_capture.py --port COM5 --out ../logs/log.txt --seq c,m,m,m,r --stop-marker "#DUMP,end"
python parse_records.py ../logs/log.txt --csv ../logs/records.csv
python stack_budget.py ../logs/log.txt --alloc acq=256,proc=256,comm=384 --baseline 512
```

---

## 5 · 这次实际改了什么，与计划书的偏差

### 5.1 偏差 1（**重要**）：SPI 四模式的"回环自测"看不到 CPHA 配错，计划书这一条写错了

**计划书原文**（第 3.2 节，第 2 步）：
> 回环自测：用一根杜邦线把 MOSI 与 MISO 短接，四种模式各发同一个字节。
> 模式配对正确 → 收到原值；**CPHA 配错 → 收到移位一拍的错值**（如 0xA5 → 0x4B / 0x52）

**问题**：单主机回环时，**收发用的是同一个模式**，"什么时候把数据摆上线"和
"什么时候去读线"是同一个人控制的，两者永远自洽。
所以**四种模式都会读回 0xA5**，CPHA 配错这件事根本不会出现。

**它证明了什么、没证明什么**（这点必须分清，否则你会以为自己测错了）：

| 测试 | 能证明 | 不能证明 |
|---|---|---|
| 杜邦线回环（四种模式） | **位引擎正确**：移位方向、边沿计数、MISO 通路都对 | ❌ 模式配对正确性 |
| W25Q64 分模式读 ID | ✅ **模式配对正确性**（Mode0/3 通，Mode1/2 错） | — |
| 逻辑分析仪看波形 | ✅ SCLK 空闲电平（CPOL）、数据稳定沿（CPHA） | — |

**本工程的处理**（已实现，见 `self_test.c`）：
1. 回环自测照做，四种模式都打印 `tx/rx/echo=MATCH`，**并明确注释说明"全 MATCH 才是预期"**；
2. 额外提供 `spi_soft_transfer_ex(tx, tx_mode, rx_mode)`，可让**发送侧与接收侧用不同模式**，
   从而真的看到 0xA5 被读成移位值 —— 这才是"主从模式不匹配"的等价演示；
3. 最硬的证据仍是 **W25Q64 分模式读 JEDEC ID**（Mode0/3 得 `EF 40 17`，Mode1/2 得错值）。

另外，计划书举的 `0x4B / 0x52` 只是示意：实际读回值取决于进入本次传输前 MOSI 的残留电平，
本工程只断言"不等于原值"，并把实际值打出来。

---

### 5.2 偏差 2：用户手册口径 —— 4 MB 还是 4 MiB

| | 计划书 | 本实现 |
|---|---|---|
| 记录区 | 十进制 4 MB = 4 000 000 B | 二进制 4 MiB = 4 194 304 B |
| 可存条数 | 125 000 条 | **131 072 条** |
| 可存时长（32 B / 1 s） | 约 1.45 天 | **约 1.52 天** |

差 4.8%，纯粹是"MB 有两种含义"。本实现按真实存储器边界取 4 MiB
（正好 1024 个 4 KB 扇区，擦除逻辑边界干净）。
**写论文/简历时统一一种口径就行**，别一处 125 000 一处 131 072。

Flash 寿命口径同理会变（扇区轮换模型下 396 年 → 约 415 年），**两者都不是约束**。

---

### 5.3 偏差 3：断电续写/写指针扫描**其实已经实现**，而且 8.35 小时那次长跑**真的跑过**

- 计划书说"W4 明确不做：断电续写、写指针扫描"。
- 但原 `recorder.c` **本来就带 `recorder_scan()`**（写指针扫描 + 页级粗扫 + 回退一页的 bug 修复），
  而且 `bme280.c` 的注释里白纸黑字写着：

  > 【读数合理性校验（2026-09-15 长跑实测发现，后加的防线）】
  > 【问题现象】连续运行 8.35 小时、30062 条记录中，有 2 条出现温度 +15.6 ℃、气压 +25.2 hPa 的孤立尖峰…

  这说明 **8.35 小时 / 30062 条那次长跑实测确实发生过**（在 16 B 格式上）。

**所以简历那条"断电续写…8.35 小时 / 30062 条"可能已经可以做实了** —— 但你得自己确认：
1. 那次长跑的数据还在吗（有没有留日志/截图）？
2. 记录已经是 32 B 了，要不要**用 32 B 重新跑一次**？重跑一次最干净（几小时而已）。

我的建议：**用 32 B 重跑一次 8 小时**，然后就可以把那条恢复进简历。
这比"基于 16 B 的旧实测"更有说服力，也避免面试官追问"你简历写 32 B，跑的是 16 B？"。
**这件事需要你决策，我没有替你改简历。**

---

### 5.4 偏差 4：发现一个真实的断链 bug，以及 `main.c` 的 HAL 残留

**① 断链 bug（已修）**：原 `bme280.c` 第 51–55 行声明并调用了
`uart1_send_byte()` / `uart1_send_udec()`，但**全工程没有任何地方定义它们**
（那是 `bus_logger` 时代 main.c 里的函数，搬到 rtos_logger 后 uart 功能挪进了 `uart_dbg.c`，
这两个符号被漏掉了）。

它之所以还能编译链接通过，是靠链接器的 `--gc-sections` 把没被调用的
`bme280_dump_calib()` / `bme280_dump_raw()` 那两个函数整段丢掉了 ——
**这种"侥幸通过"最危险**：哪天你为了调试去调一下 `bme280_dump_calib()`，链接立刻炸，
而且报错位置（bme280.o 里 undefined reference to uart1_send_byte）会让你以为是 uart 的问题。
→ **已改为使用 `uart_dbg_*`。**

**② `main.c` 还是 CubeMX/HAL 生成的（已重写）**：
原来的 `SystemClock_Config()` 是 `RCC_OSCILLATORTYPE_HSI` + `PLL_NONE` +
`FLASH_LATENCY_0` → **跑在 8 MHz HSI，根本没到 72 MHz**。
这与三件事同时冲突：W1 要求 72 MHz、简历写"不依赖 HAL"、`uart_dbg.h` 里
`UART_DBG_PCLK2_HZ` 若按 72 MHz 算 BRR 就会与实际的 8 MHz 不符（串口乱码）。

→ 已重写为纯寄存器版：`clock_init_72mhz()` + DWT 延时 + 寄存级 USART1，
不再调用 `HAL_Init()` / `HAL_RCC_*`，也不再使用 CMSIS-RTOS_V2 包装层。

---

### 5.5 偏差 5：BME280 那个"确定性数据污染"，我改了修法（比原注释里的方案更小）

原 `bme280.c` 注释里把根因分析得很清楚（8 字节突发读可能跨过从机寄存器更新时刻，
读到"半新半旧"的一组数据），但给的方案是"改用 Forced 模式"。

Forced 模式要重写整个测量流程（触发 → 等 measuring → 读），
而且会把单次测量时间拉到 ~46 ms，改动面大、风险高。

**本实现选了一个更小的等价修法**：在读 8 字节之前，**先看一眼状态寄存器的 measuring 位**
（`0xF3` bit3）—— 它是 0 才读。
因为转换只占 1000 ms 周期里的约 46 ms，所以"看到 measuring=0"就说明
接下来的 0.7 ms 突发读**必然落在不会被改写的窗口里**。
代价是一次单字节 I2C 读，收益是把"事后拦截"变成"事前避免"。

原有的**物理量闸门（绝对范围 + 变化率）保留**，退化成第二道防线 —— 纵深防御。

> Forced 模式作为备选方案写在 `bme280.c` 的注释里，将来要换也能找到依据。

---

## 6 · 简历四条 ↔ 代码落地位置对照

| 简历项目经历 | 落地在 |
|---|---|
| **① 基于 FreeRTOS 构建 3 任务采集架构**（`vTaskDelayUntil` 1 s 不漂移 / 消息队列 / 互斥锁保护 I2C·SPI / `uxTaskGetStackHighWaterMark` 实测栈水位） | `app_tasks.c`（三任务 + 两队列 + 两互斥锁 + 一二值信号量）、`log.c`（打印互斥锁）<br>**压缩比 = `stack_budget.py` 算出来的值，必须实测** |
| **② 不依赖 HAL 与现成驱动**，手写 bit-bang I2C / SPI 主机时序（START/STOP/Repeated Start、四种 SPI 模式），逻辑分析仪逐帧验证 | `i2c_soft.c`、`spi_soft.c`（含 `transfer_ex` 支持四模式）<br>`main.c` 已彻底移除 HAL |
| **③ 实现 BME280 完整驱动**：33 个出厂校准系数 + Bosch 整数补偿；气压 **64 位**避免溢出；PC 端复算逐位一致 | `bme280.c`（`int64_t` 气压补偿）、`tools/verify_bme280.py`（独立复算） |
| **④ 设计 32 字节定长记录格式**（含头尾双 CRC-16/MODBUS），打通 I2C 采集 → SPI 存储 → UART 上传 → PC 逐字节校验闭环 | `recorder.c`（32 B + 双 CRC + 两步写）、`w25q64.c`、`self_test.c`、`tools/parse_records.py` |

### 投递前必查（执行计划第 5.2 节）
- [ ] 项目经历里有没有**还没做实**的条目？有就删。
- [ ] RTOS 那行是否已从"了解"升级（**W3/W3.5 做完才能升**）。
- [ ] 每个数字是否能当场推导：`BRR=0x271`、`chip id=0x60`、33 个校准系数、
      **32 B 布局**、131 072 条 / 1.52 天、气压 0.01 hPa 余量。
- [ ] 简历里"32 字节"是否统一（不要一处 16 一处 32）。

---

## 7 · 还需要你做的（红线：不能编数字）

0. 🔴 **刷本版固件后，先发两次 `e` 清空记录区。**
   尾 CRC 已从 MODBUS 换成 CCITT-FALSE（**破坏性格式变更**，原因见 `docs/11`），
   旧记录的尾 CRC 会被判成"未收尾"，不清空则计数混乱。
   开机横幅新增一行 `#CRC2,tail_algo=CCITT-FALSE,...,expect=0x29B1,PASS`，先看它对不对。
1. **上板把 W1–W4 跑通**，按 `docs/05_验收手册.md` 逐条打勾。
2. **填 [实测后填]**：跑完拿 `tools/stack_budget.py` 算压缩比，再回填简历。
   **我给的示例值一律不许用。**
3. **决定 8.35 小时那条要不要恢复**（见 §5.3），以及是否用 32 B 重跑一次。
4. **跑 24 小时稳定性测试**（`APP_UART_VERBOSE=0`）。
5. **日常开发就在 `firmware\` 里做** —— CubeIDE 直接 `File > Import` 打开即可，见 `docs/09_用CubeIDE开发与调试.md`。
   只有想把源码并进旧的 `rtos_logger` 工程时才需要 `docs/07`。

---

## 8 · 一条别丢的原则

> **简历上写的必须是已经做完的。**

这份工程把"能做的"都做到位了，但"做完"的最后一步（上板跑通 + 实测）
只能你来做。**在跑通之前，简历里带 `[实测后填]` 的那条要按如实措辞写，或者先撤下来。**
