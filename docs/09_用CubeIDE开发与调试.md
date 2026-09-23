# 09 · 用 STM32CubeIDE 开发与调试

> `firmware\` 现在**就是一个标准的 STM32CubeIDE 工程**了。
> 本文讲：怎么导入、第一次该做什么、两套构建方式怎么选、以及一个能让你翻车的坑。

---

## 0. 先给结论

| 你想做的事 | 怎么做 |
|---|---|
| 在 CubeIDE 里打开工程 | 见下面第 1 节（3 步，1 分钟） |
| 编译 | `Ctrl+B`（或 Project → Build Project） |
| 烧录并调试 | `F11`（Debug As → STM32 C/C++ Application） |
| 烧录后直接跑 | `Ctrl+F11`（Run As） |
| 不进 CubeIDE 只想烧固件 | `python tools\run.py` → 选 8 |
| 改代码 | 直接改 `Core\Src\*.c` / `Core\Inc\*.h` |

---

## 1. 导入工程（3 步）

```
① File → Import…
② 选 General → Existing Projects into Workspace → Next
③ Select root directory 填：D:\work\2026-09-17-15-24-45\firmware
   → Finish
```

导入后左边 Project Explorer 里会出现一个叫 **`firmware`** 的工程。

> **也可以用** `File → Open Projects from File System…`，效果一样。
>
> ⚠️ **别改项目名。** `.cproject` 里有一处写死了 `${workspace_loc:/firmware}/Debug`（编译输出目录）。
> 改名工程会让编译产物找不到地方放。真要改，得同时把 `.cproject` 里那两处 `firmware` 一起改掉。

---

## 2. 第一次进来，先做这三件事

### ① 编译一遍（`Ctrl+B`）

**预期：零错误、零告警。**

这不是我猜的 —— 我是用 CubeIDE 托管构建的**原样参数**（`-std=gnu11 -O0 -Wall`，且把源文件夹下**全部 26 个文件**都编进去）在命令行跑过一遍的结果：

```
编译 26 个文件 → 错误 0，告警 0
链接 → 退出码 0
（链接期只有 4 条 newlib 的 _close/_lseek/_read/_write "not implemented" 告警，
  这是 -specs=nosys.specs 的正常现象，CubeIDE 里也一样会出现，不影响运行）
```

### ② 看一眼体积

编译日志末尾会有 `arm-none-eabi-size` 的输出。预期：

```
   text	   data	    bss	    dec	    hex	filename
  36752	     24	  11696	  48472	   bd58	Debug/firmware.elf
```

| 项 | CubeIDE Debug | 手写 Makefile |
|---|---|---|
| Flash（text+data） | **35.9 KB / 64 KB（56%）** | **26.8 KB / 64 KB（42%）** |
| RAM（data+bss） | **11.4 KB / 20 KB（57%）** | **11.4 KB / 20 KB（57%）** |

Debug 比 Makefile 大 9 KB，原因是 **`-O0` vs `-Og`**，不是文件多编了。
想省 Flash 就切 Release 配置（`-Os`）。

### ③ 烧录 + 调试（`F11`）

第一次按 `F11` 会弹出调试配置窗口：

- 它会**自动检测到你的 ST-LINK**（SN `57FF68068389495630590867`）
- 确认后点 Debug，就会烧录并停在 `main()` 入口
- 想不调试直接跑：`Ctrl+F11`

> 我在 `.cproject` 里把 **CPU 时钟设成了 72 MHz**（`cpuclock=72`）。
> 这个值影响调试器的时间显示和 SysTick 相关视图 —— 设成 8（CubeMX 的复位默认）会让计时显示不准。

> ⚠️ **如果烧完程序没有任何输出，先拔插一次 USB。**
> 调试器复位后内核可能停在暂停态，程序根本没被执行 —— 这不是代码问题。
> 详见 `docs\08_怎么打开工程与烧录.md` 第 6 节。

---

## 3. 两套构建方式的关系（这一节最重要）

这个工程**同时支持两种构建方式**，它们共用同一份源码：

| | CubeIDE 托管构建 | 命令行 Makefile |
|---|---|---|
| 怎么触发 | `Ctrl+B` | 在 firmware 目录执行 `make` |
| 配置放在哪 | `.cproject` | `Makefile` |
| 输出目录 | `Debug\`（或 `Release\`） | `build\` |
| 产出文件 | **只有 `.elf` / `.map` / `.list`** | `.elf` + **`.hex`** + `.bin` |
| 源文件清单 | **自动扫描** Core / Middlewares / Drivers 下所有 `.c`、`.s` | `C_SOURCES` **手写**清单 |
| 优化等级 | Debug `-O0` / Release `-Os` | `-Og` |
| 实测 Flash | 35.9 KB | 26.8 KB |

### ★ 差异一：新增 `.c` 文件时，两边行为不同

- **CubeIDE**：放进 `Core\Src\` 就自动编进去了，**不用改任何配置**
- **Makefile**：必须手工把文件加进 `C_SOURCES` 清单，否则不编

后果是：你在 CubeIDE 里加了个新文件、编译通过、运行正常；
哪天用 `make` 编译却报「找不到符号 `xxx`」——**看起来像代码坏了，其实只是清单漏了**。

**检查办法（我加的一个目标）：**

```bash
make check-src
```

它会打印两节：磁盘上有但没进 `C_SOURCES` 的、以及 `C_SOURCES` 里写了但磁盘上没有的。
**两节都空 = 一致。**

### ★ 差异二：CubeIDE 不产出 `.hex`

我把 `rtos_logger` 的 `Debug\` 翻了一遍，CubeIDE 默认**只生成** `.elf` / `.list` / `.map`，
**不生成** `.hex` 和 `.bin`（那两个 objcopy 后处理步骤默认是关的）。

这意味着：**如果你在 CubeIDE 里编译完，再手工去烧 `build\fw.hex`，烧进去的是旧版本。**

为了堵这个坑，`tools\run.py` 的第 8 项做了两件事：

1. **自动列出所有编译产物**，按新旧排序，让你选（默认选最新的）
2. **检查源码是否比固件新** —— 如果新，会警告你 "先重新编译"

```
  找到以下编译产物（按新旧排序）：

    #  来源                  修改时间          文件
    -  ------------------  ----------------  ------------------------
    1  CubeIDE Debug 配置   2026-09-17 22:30  Debug\firmware.elf
    2  命令行 Makefile 构建  2026-09-17 18:21  build\fw.hex
```

`.elf` 和 `.hex` 都能直接烧（都自带地址信息），所以两种构建方式产出的固件都能用。

---

## 4. 调试怎么用

| 想干什么 | 在哪 |
|---|---|
| 下断点 | 行号左边双击 |
| 看变量当前值 | 鼠标悬停在变量上；或 Variables 视图 |
| 长期盯一个变量 | Expressions 视图（可手输表达式） |
| 看外设寄存器 | **Peripherals → SFRs**（能直接看到 RCC / GPIO / USART 每一位置） |
| 看调用栈 | Debug 视图的调用栈窗口 |
| 看内存 | Window → Show View → Memory，输 `0x20000000` 看 RAM |

**这个项目的调试重点：**

- **`.debug` 段很全**（`-g3`），所有变量都能看，包括结构体逐字段
- **看 `RCC->CFGR`** 能亲眼确认时钟真的切到 PLL 了（比只看串口打印更硬）
- **看 `GPIOA->ODR` / `GPIOB->BSRR`** 能确认软件 SPI 的位有没有按时序翻
- **`_user_heap_stack` 段**在 `.map` 里，能确认 FreeRTOS 堆和主栈没打架

**关于串口：** CubeIDE 内置的 Terminal 会**独占 COM8**。开着它的时候，
`tools\run.py` 的串口功能会报"打开失败"—— 反过来也一样。**同一时间只能一个用。**

---

## 5. 一个能让你翻车的坑：重复定义的中断处理函数

如果你打算把 `firmware\` 的代码**搬进你原来的 `rtos_logger` 工程**，或者反过来把
`rtos_logger` 的某些文件加进来，会撞上这个：

> **`CMSIS_RTOS_V2` 和 FreeRTOS 原生 API 都会定义
> `SysTick_Handler` / `PendSV_Handler` / `SVC_Handler`。**

`rtos_logger` 用的是 CubeMX 生成的 **CMSIS-RTOS2 包装层**（`cmsis_os2.c`）；
`firmware\` 用的是 **FreeRTOS 原生 API**（直接调 `xTaskCreate` 等）。
两套一起编译 → **重复定义报错**，而且报错信息指向启动文件，看起来像启动文件坏了。

**处理办法**：在 CubeIDE 里

```
右键 Middlewares\Third_Party\FreeRTOS\Source\CMSIS_RTOS_V2
  → Exclude from build
```

顺带说：`firmware\` **自己带的 `Middlewares` 里没有 CMSIS_RTOS_V2**，我一开始就没复制它。
所以你直接导入 `firmware\` 是不会有这个问题的 —— 只有做上面那种"混合搬运"时才会遇到。

详细搬运步骤见 `docs\07_同步回CubeIDE工程.md`（那份是"往 rtos_logger 里搬"的版本）。

---

## 6. 为什么这个工程没有 `.ioc`

`.ioc` 是 CubeMX 的图形配置文件。没有它，就意味着你**不能**在 CubeIDE 里点开
`.ioc`、用图形界面改引脚/时钟、然后点 "Generate Code"。

**这是刻意的，不是漏了。** 原因：

> 这个工程全程**寄存器手写、不用 HAL**。
> 一旦放一个 `.ioc` 进去并点了 Generate Code，
> CubeMX 会按图形配置**重新生成 `main.c`**，把 `HAL_Init()`、`SystemClock_Config()` 塞回来，
> **你的手写代码会被覆盖掉。**

**要改配置就改 `Core\Inc\board.h`** —— 所有引脚号、时钟频率、波特率都集中在那一个文件里：

```c
#define BOARD_UART_BAUD     115200UL
#define BOARD_UART_BRR      ((BOARD_PCLK2_HZ + (BOARD_UART_BAUD / 2UL)) / BOARD_UART_BAUD)
```

这比图形界面更清楚（一眼能看到公式），也是面试时能讲的一个点：
「我没用 CubeMX 生成，是为了让每一处引脚和时钟都能追溯到寄存器」。

---

## 7. 常见问题

| 现象 | 原因 | 怎么办 |
|---|---|---|
| 导入后工程图标带红叉 | C/C++ 索引还没建好 | 等它跑完（右下角进度条）；或 Project → C/C++ Index → Rebuild |
| 打开 `.c` 中文乱码 | 工作区文本编码不是 UTF-8 | 我已经在 `.settings\org.eclipse.core.resources.prefs` 里锁了 UTF-8。若还乱：Window → Preferences → General → Workspace → Text file encoding → UTF-8 |
| `Build` 报找不到 `arm-none-eabi-gcc` | 工具链路径丢了 | 右键工程 → Properties → C/C++ Build → Settings → Toolchains 选 "GNU Tools for STM32 (11.3.rel1)" |
| 报 `No ST-LINK detected` | 调试会话被别的程序占着 | 关掉 `tools\run.py` 的串口抓取、STM32CubeProgrammer，或上一次没退干净的调试会话 |
| 调试时程序跑飞 | 时钟配置和你以为的不一致 | 在 Debug 里看 `RCC->CFGR` 的 `SWS` 位，确认真的是 PLL |
| `make` 报 `mkdir -p 已经存在` | 这条 bug 已修（旧版本遗留） | 确认 `Makefile` 里那行是 `-$(MKDIR_CMD) $@` |
| `make` 报找不到 `rm` | cmd 里没有 `rm` | `clean` 默认用 cmd 语法；在 Git Bash 里跑要加：`make clean CLEAN_CMD="rm -rf"` |

---

## 8. 一页速查

```
导入工程            File > Import > General > Existing Projects into Workspace
                      root = D:\work\2026-09-17-15-24-45\firmware

编译                Ctrl+B
烧录+调试           F11        （第一次会问你选哪个探头 → ST-LINK）
烧录后直接跑        Ctrl+F11

产物位置            Debug\firmware.elf     (CubeIDE，只有 elf/map/list)
                    build\fw.hex           (Makefile，有 elf/hex/bin)

新增了 .c 之后       CubeIDE 不用管；Makefile 要手工加进 C_SOURCES
                     用 make check-src 检查有没有漏

不进 CubeIDE 烧固件  python tools\run.py  → 选 8

改引脚/时钟          改 Core\Inc\board.h（不要指望 .ioc，这个工程没有）
```

---

## 附：这个工程文件是怎么来的（可追溯）

`.project` / `.cproject` **不是我凭空编的 XML**，是照你那个**已经证明能编译能烧**的
`D:\stm32workplace\rtos_logger\` 工程改的，只动了这几处：

| 改动 | 从 | 到 |
|---|---|---|
| 项目名 | `rtos_logger` | `firmware`（与文件夹同名，才能直接 Import） |
| MCU | STM32F103C8Tx | 不变 |
| include 路径 | 含 HAL 与 CMSIS_RTOS_V2，共 8 条 | 去掉 HAL/CMSIS_RTOS_V2，共 5 条 |
| 宏定义 | `DEBUG`、`USE_HAL_DRIVER`、`STM32F103xB` | 去掉 `USE_HAL_DRIVER`（本工程无 HAL） |
| 源文件夹 | Core / Middlewares / Drivers | 不变 |
| 链接脚本 | `${ProjName}/STM32F103C8TX_FLASH.ld` | 不变（工程根目录已有此文件） |
| CPU 时钟 | 8 | **72** |
| 新增 | — | `.settings\org.eclipse.core.resources.prefs`（锁 UTF-8 编码） |

改完之后做过这些检查（全部通过）：

- `.project` / `.cproject` / `.settings\*.xml` XML 可解析、纯 UTF-8 无 BOM
- `.cproject` 里 5 条 include 路径、3 个源文件夹、链接脚本 —— **在磁盘上都真实存在**
- 关键元素数量与模板逐项比对：`cconfiguration` 4/4、`toolChain` 4/4、`<tool>` 26/26、
  `inputType` 8/8、`option` 29/29、`scannerConfigBuildInfo` 4/4
- 29 个 option/tool 的 `id` **无重复**
- 用 CubeIDE 托管构建的原样参数编了全部 26 个文件 → **0 错误 0 告警**，链接通过
