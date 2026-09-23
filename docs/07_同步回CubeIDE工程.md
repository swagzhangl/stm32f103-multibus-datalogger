# 如何同步回 CubeIDE 工程

> 本文解决一个问题：**这份独立工程（Makefile）怎么搬进你已有的 CubeIDE 工程 `D:\stm32workplace\rtos_logger\`？**
> 或者你更想直接在命令行用 `make`，那就不需要看这篇。

---

## 方案对比：先选一条路

| 方案 | 适合 | 代价 |
|---|---|---|
| **A. 独立 Makefile 工程（本仓库现状）** | 只想改代码、编译、烧录；不喜欢 IDE | 单步调试要另配（可用 Ozone / openocd+gdb） |
| **B. 搬进现有 `rtos_logger`** | 想在 CubeIDE 里单步调试、看寄存器 | 要做一次性的工程设置（本文重点） |
| **C. 在 CubeIDE 里新建 "Makefile Project with Existing Code"** | 想用 IDE 但不想改现有工程 | 调试配置要手工建一次 |

**推荐：先用 A 把功能跑通，再考虑 B。** 理由：
搬进 CubeIDE 涉及"排除 HAL 驱动、排除 CMSIS_RTOS_V2"这些设置，
一旦哪里没排干净，报的错是 `multiple definition` 之类，看起来很吓人但其实是配置问题 ——
**先把功能验证通过，再折腾工程配置**，排查范围小得多。

---

## 方案 B：搬进现有 `rtos_logger`

### B.1 文件对应表（**照这张表操作**）

#### ✅ 新增（`rtos_logger\Core\` 里没有的）

| 文件 | 作用 |
|---|---|
| `Inc/board.h` | 引脚 / 时钟 / 报警阈值总表 |
| `Inc/app_config.h` | 编译期开关（互斥锁对照实验、栈档位、串口详略） |
| `Inc/clock_init.h` + `Src/clock_init.c` | 寄存器版 72 MHz 时钟树 |
| `Inc/delay_us.h` + `Src/delay_us.c` | DWT 微秒延时 + 单调计时 |
| `Inc/log.h` + `Src/log.c` | 互斥锁保护的打印 |
| `Inc/self_test.h` + `Src/self_test.c` | W4 自检（四模式 / 记录闭环 / 断电三情况） |

#### 🔄 直接替换（同名，覆盖）

| 文件 | 改了什么（要点） |
|---|---|
| `Src/main.c` | **彻底重写**：去掉 `HAL_Init()` / `HAL_RCC_*` / `cmsis_os`；改成 `clock_init_72mhz()` + `delay_init()` + `vTaskStartScheduler()`；FreeRTOS 钩子搬到这里 |
| `Src/system_stm32f1xx.c` | 自写版：不依赖 HAL 的宏 |
| `Src/stm32f1xx_it.c` | 只留 fault handler；**新增 HardFault 现场提取**；⚠️ **不再定义 SysTick/PendSV/SVC** |
| `Inc/FreeRTOSConfig.h` | 原生 API 版：静态分配关、定时器关、`configASSERT` 可见、优先级档数 8 |
| `Inc/uart_dbg.h` + `Src/uart_dbg.c` | **新增 RX + 环形缓冲 + USART1 中断**；BRR 从 8 MHz 改成 72 MHz |
| `Inc/i2c_soft.h` + `Src/i2c_soft.c` | 延时改用 DWT；**新增总线复位 `i2c_soft_bus_recover()`** 与诊断计数 |
| `Inc/bme280.h` + `Src/bme280.c` | **修掉 `uart1_send_byte` 断链**；**新增 `measuring` 位前置检查**；新增校准系数/原始量 getter |
| `Inc/spi_soft.h` + `Src/spi_soft.c` | 新增 `transfer_ex`（收发可分别指定模式）、位延时接口 |
| `Inc/w25q64.h` + `Src/w25q64.c` | 超时改成 DWT 计时；新增 `w25q64_is_busy()` |
| `Inc/crc16.h` + `Src/crc16.c` | 新增 `crc16_modbus_update()`（流式续算，尾 CRC 用） |
| `Inc/recorder.h` + `Src/recorder.c` | **16 B 单 CRC → 32 B 头尾双 CRC**；**两步写**；扫描逻辑适配新格式 |
| `Inc/buzzer.h` + `Src/buzzer.c` | 新增带回差的 `buzzer_alarm_update()` |
| `Inc/app_tasks.h` + `Src/app_tasks.c` | **阶段 0 测试任务 → 业务三任务**（采集/处理/通信 + 两队列 + 两锁 + 一信号量 + 命令处理） |

#### ❌ 排除（不要再编译）

| 文件 | 为什么 |
|---|---|
| `Src/freertos.c` | 里面的两个钩子已搬到 `main.c`；留着会 **multiple definition** |
| `Src/stm32f1xx_hal_msp.c` | HAL 的外设初始化回调，本项目不用 HAL |
| `Src/stm32f1xx_hal_timebase_tim.c` | HAL 用 TIM4 做时基；本项目 SysTick 完全归 FreeRTOS |
| `Inc/main.h` / `Inc/stm32f1xx_it.h` / `Inc/stm32f1xx_hal_conf.h` | HAL 相关头文件，我的代码不 include 它们 |

> `Src/syscalls.c` / `Src/sysmem.c` **可以保留**（提供 `_sbrk` 等桩函数）。
> 如果排除掉，就要在链接选项里加 `-specs=nosys.specs`。

### B.2 ⚠️ 必做：排除 HAL 驱动与 CMSIS_RTOS_V2

这是搬进 CubeIDE **最容易翻车**的一步。

#### ① 排除 `Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2`

**为什么**：`cmsis_os2.c` 里会定义 `SysTick_Handler`，
而 `port.c` 通过 `FreeRTOSConfig.h` 的宏也定义了 `SysTick_Handler`
→ **链接报 `multiple definition of 'SysTick_Handler'`**。

CubeMX 那版是靠 `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION = 1` 这个宏绕开的；
我用的是原生 API，干脆不用这层包装 —— **整个 `CMSIS_RTOS_V2` 目录从编译里排除掉**。

操作：工程树里右键 `CMSIS_RTOS_V2` → **Resource Configurations → Exclude from Build…** → 全选 → OK。

#### ② （推荐）排除 `Drivers/STM32F1xx_HAL_Driver`

**为什么**：简历写的是"**不依赖 HAL**"。把 HAL 驱动从链接里排除掉，
这句话才是字面为真的（`make size` 出来的 Flash 占用也才是诚实的）。

操作：同样用 Exclude from Build。

> 如果排除后报某个 `HAL_xxx` 未定义，说明还有源文件在用 HAL ——
> 那就是 `freertos.c` / `stm32f1xx_hal_msp.c` / `stm32f1xx_hal_timebase_tim.c` 没排干净。

#### ③ 保留 `Drivers/CMSIS`（**必须保留**）

`stm32f103xb.h` / `core_cm3.h` 这些是**寄存器级编程的基础**，不是 HAL。
本项目大量使用 `RCC->CFGR`、`GPIOA->CRH` 这些结构体指针，全靠它。

> **别搞混**：`Drivers/CMSIS` = 芯片寄存器定义（要用）；
> `Drivers/STM32F1xx_HAL_Driver` = HAL 库（不用）。

#### ④ FreeRTOS 内核源码保留

`Middlewares/Third_Party/FreeRTOS/Source/` 下这些要保留：
`list.c` / `queue.c` / `tasks.c` / `portable/GCC/ARM_CM3/port.c` / `portable/MemMang/heap_4.c`
+ `include/` 全部 + `portable/GCC/ARM_CM3/portmacro.h`

**不需要**的：`croutine.c`、`event_groups.c`、`stream_buffer.c`、`timers.c`
（`configUSE_TIMERS = 0` 时 `timers.c` 可以不编；留着也不报错，只是多占几 KB Flash）

### B.3 头文件路径（必须齐全）

工程属性 → **C/C++ Build → Settings → Tool Settings → MCU GCC Compiler → Include paths**。
必须有这几条（**缺一条就报"找不到 xxx.h"**）：

```
Core/Inc
Drivers/CMSIS/Include
Drivers/CMSIS/Device/ST/STM32F1xx/Include
Middlewares/Third_Party/FreeRTOS/Source/include
Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3
```

> 最后那条**最容易漏** —— `portmacro.h` 就在里面，
> 漏了会报 `#include "portmacro.h"` 找不到。

### B.4 宏定义

**C/C++ Build → Settings → Preprocessor** 里确认有：

```
STM32F103xB
```

它决定 `stm32f1xx.h` 里 include 哪一份具体型号头文件（`stm32f103xb.h`）。
**写错或漏了会报一堆外设寄存器未定义。**

### B.5 不要再用 CubeMX 重新生成代码

`.ioc` 文件留着当"引脚配置的记录"就好，**但不要点"Generate Code"** ——
它会重新生成 `main.c`（覆盖掉我的寄存器版）和 `freertos.c`（钩子重复定义）。

如果非要生成，生成后**必须**：
1. 用 git 或备份把 `main.c` / `stm32f1xx_it.c` / `FreeRTOSConfig.h` / `freertos.c` 恢复回来；
2. 重新做一次 B.2 的排除操作（CubeMX 会把它们加回编译）。

> **更省事的做法**：把 `.ioc` 删掉或改名（比如 `rtos_logger.ioc.bak`），
> 从物理上杜绝"手滑生成"。

### B.6 链接脚本

把工程根目录的 `STM32F103C8TX_FLASH.ld` 换成 `firmware/STM32F103C8TX_FLASH.ld`。

差异：我的版本去掉了 `/DISCARD/ { libc.a(*) ... }` 这一段
（ST 模板里那段会把 libc 全丢掉，而结构体赋值可能生成 `memcpy` 调用 → 链接报未定义）。

**或者**：保留原脚本，但在**链接器选项**里加 `-specs=nosys.specs`，效果类似。

---

## 方案 C：在 CubeIDE 里用现有 Makefile

1. `File → New → Project… → C/C++ → Makefile Project with Existing Code`
2. **Location** 指向 `D:\work\2026-09-17-15-24-45\firmware`
3. Toolchain 选 **Cross ARM GCC**
4. `Project Properties → C/C++ Build → Build command` 保持 `make`
5. **Environment** 里加一条（让 make 能找到工具链；IDE 自带的工具链路径一般已在 PATH）：
   ```
   TOOLCHAIN = D:/STM32CubeIDE_1.13.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.0.202305231506/tools/bin
   ```

要做单步调试，再建一个 **Debug Configuration → GDB OpenOCD Debugging**，
配置 ST-Link + `build/fw.elf`。

---

## 常见链接/编译错误对照表

| 报错 | 原因 | 修法 |
|---|---|---|
| `multiple definition of 'SysTick_Handler'` | `CMSIS_RTOS_V2/cmsis_os2.c` 和 `port.c` 都定义了 | 排除 `CMSIS_RTOS_V2`（见 B.2 ①） |
| `multiple definition of 'vApplicationStackOverflowHook'` | `freertos.c` 和 `main.c` 都有 | 排除 `Src/freertos.c`（见 B.1 排除表） |
| `multiple definition of 'PendSV_Handler'` / `SVC_Handler` | `stm32f1xx_it.c` 里定义了这三个 | 删掉 `stm32f1xx_it.c` 里的这三个函数（我的版本已经没有） |
| `undefined reference to 'HAL_Init'` / `HAL_RCC_OscConfig'` | 某个源文件还在用 HAL，但 HAL 驱动被排除了 | 找出那个文件（多半是 `freertos.c` / `hal_msp.c`）并排除 |
| `undefined reference to 'uart1_send_byte'` | 用了旧版 `bme280.c` | 换成我的版本（已改用 `uart_dbg_*`） |
| `undefined reference to 'vApplicationGetIdleTaskMemory'` | `configSUPPORT_STATIC_ALLOCATION = 1` 但没人提供回调 | 换成我的 `FreeRTOSConfig.h`（把静态分配关了），或自己实现那两个回调 |
| `#include "portmacro.h"` 找不到 | include 路径缺 `portable/GCC/ARM_CM3` | 补上（见 B.3） |
| `'RCC' undeclared` | 宏 `STM32F103xB` 没定义 | 补上（见 B.4） |
| `region RAM overflowed` | RAM 超了 20 KB | `make size` 看哪块大；调小 `configTOTAL_HEAP_SIZE` 或任务栈 |
| `undefined reference to '_sbrk'` / `_write'` | 排除了 `syscalls.c` 但没加 nosys | 加 `-specs=nosys.specs`，或保留 `syscalls.c` |
| `undefined reference to 'memcpy'` | 链接脚本里 `/DISCARD/` 把 libc 丢了 | 用我的链接脚本（已去掉那段） |

---

## 迁移后的自检

搬完之后，**先别急着点烧录**。确认这几件事：

1. **编译零错误零告警**（CubeIDE 的 Console 里搜 `warning`）。
2. **`fw.elf` 的 `.bss` 大小合理**（我的实测是 10152 B；如果明显更大，说明有多个版本的源文件同时在编）。
3. **`nm` 或 Map 文件里这三个符号各只有一个定义**：
   `SysTick_Handler`、`PendSV_Handler`、`SVC_Handler`。
4. **烧录后看开机横幅**，`#CLK,got` 与 `#CLK,expect` 必须一致。
5. 横幅里的 `#BUILD,...APP_STACK_MEASURED=0` —— 确认你烧的是宽裕档（第一次）。

> **第 3 条最有用**：`multiple definition` 这类问题在编译期就会拦下来，
> 但如果两个同名符号分别在**不同的 .o** 里并且其中一个没被引用，
> 链接器可能静默选一个 —— 那就变成"行为诡异"而不是"报错"了。
