# W3 / W3.5 · 模块 7 —— FreeRTOS 三任务架构 + 栈水位实测

> 对应文件：`Core/Src/app_tasks.c`、`Core/Src/log.c`、`Core/Inc/FreeRTOSConfig.h`
> 人日：4 + 2 ｜ 验收：3 任务跑起来、队列能传数据、**去掉锁能复现问题加回锁能解决**、连续 24 h 不死机、有栈水位实测表

---

## 一、为什么要用 RTOS（白话版）

**不用 RTOS 的写法**（超级循环）：

```
一个人干三件事：
  读传感器 → 判断 → 存 Flash → 打印 →  读传感器 → 判断 → 存 Flash → 打印 → ...
```

问题在哪？**存 Flash 有时候要 400 毫秒**（擦扇区最坏情况）。
这 400 毫秒里，采集完全停摆 —— 而需求是"严格 1 秒一次"。
周期就这样被顶掉了，而且顶掉多少**完全不可预测**。

**用 RTOS 的写法**：

```
三个工人，各有各的活：
   采集工（最高优先级，严格 1 秒一次）  只管读传感器，读完往传送带上放
   处理工（中优先级）                  从传送带取，判断/打包/存 Flash
   通信工（最低优先级）                从传送带取，打印/收命令

关键：采集工放完东西就走，绝不等处理工。
     "存 Flash 慢"这件事，被【传送带（消息队列）】吸收了 ——
     传送带上有缓冲位，处理工慢一点没关系，别让传送带满了就行。
```

这就是 RTOS 的核心价值：**把"时间上互相牵扯"变成"通过队列解耦"**。

### 面试时的标准答法

> 问："你为什么用 RTOS？"
>
> 答：**因为采集周期和存储耗时差了两个数量级** —— 采集要求严格 1 秒，
> 而 Flash 擦一个扇区最坏 400 毫秒。放在一个循环里，擦除会把采集周期顶掉。
> 用消息队列把两者解耦后，采集只管往队列里放，存储慢一点由队列缓冲吸收。
> 同时通信任务在等队列时是**阻塞**的，不占 CPU ——
> 这是超级循环做不到的（超级循环只能空转轮询）。

---

## 二、任务划分（本工程的真实设计）

| 任务 | 优先级 | 触发方式 | 职责 | 用到的 IPC |
|---|---|---|---|---|
| `acq` | **4（最高）** | `vTaskDelayUntil` 严格 1 s | 读 BME280 → 写 `s_q_raw` | 互斥锁（I2C） |
| `proc` | 3 | 被 `s_q_raw` 唤醒 | 报警判定（回差）+ 打包 32 B + 写 Flash → 写 `s_q_rep` | 互斥锁（SPI）、二值信号量（报警事件） |
| `comm` | **2（最低）** | 20 ms 超时轮询 | 串口上报 + 收命令 + 定期报栈水位 | 互斥锁（UART / I2C / SPI） |

**优先级为什么这么定？**

- **采集最高**：因为它的周期是"硬"的（简历写了"1 s 周期不漂移"），
  被别的任务挡住就会漂。
- **通信最低**：串口打印晚几十毫秒，没人受伤。
  **反过来就糟了** —— 打印是阻塞式的（逐字节等 TXE），
  一条 70 字节的行在 115200 下要 6 ms。如果通信优先级比采集高，
  采集就会被打印顶掉。

> **面试点**："优先级怎么定的？"
> —— 不是"凭感觉"，而是按**时间约束的紧迫程度**：
> 谁有硬周期谁最高；谁只有"最终会做完"的要求谁最低。

### 为什么把存储放在处理任务里，而不是单独开第四个任务？

执行计划定的是 **3 任务**（简历也这么写）。存储（写 Flash）和"打包"是同一条数据流水线上的连续两步，
放一起逻辑最紧，也省一个任务栈（F103 只有 20 KB RAM，一个任务栈最少 512 B）。

---

## 三、队列 / 信号量 / 互斥锁：三者分工（**本工程最重要的知识点**）

新手最容易把这三样用混。一句话分清：

| 机制 | 传什么 | 有没有数据 | 典型用途（本工程） |
|---|---|---|---|
| **消息队列** | **数据** | 有内容、有长度、可取走 | 采集工 → 处理工传样本；处理工 → 通信工传结果 |
| **二值信号量** | **事件** | 没有内容，只有"发生过了" | 报警从"关"变"开"时通知通信任务 |
| **互斥锁** | **所有权** | 有"谁持有"的概念 | 保护 I2C / SPI / UART 这些共享资源 |

**记法**：
- **队列是传送带**（能放东西）
- **信号量是门铃**（只告诉你"有人来了"，不告诉你是谁）
- **互斥锁是钥匙**（同一时刻只有一个人能拿）

### 代码里三者的实际用法

```c
/* ① 队列传数据：采集 → 处理 */
if (xQueueSend(s_q_raw, &s, 0) != pdPASS) { s_drop_raw++; }

/* ② 二值信号量传事件：报警状态由"关"变"开"的那一刻 */
if ((alarm_now != 0U) && (alarm_prev == 0U)) {
    (void)xSemaphoreGive(s_sem_alarm);
}

/* ③ 互斥锁保护共享总线 */
if (app_lock_spi(LOCK_TMO_MS) != 0U) {
    rc = recorder_append(&rec);
    app_unlock_spi();
}
```

### 为什么互斥锁要"带回差的 200 ms 超时"而不是 `portMAX_DELAY`？

`portMAX_DELAY` = 无限等。万一某处持锁后卡住（比如 Flash 死等），
**整个系统会静静地死锁在这里**，而且你完全看不到线索。

本工程用 200 ms 有限超时：

```c
#define LOCK_TMO_MS     200U

uint8_t app_lock_spi(uint32_t timeout_ms)
{
    if (s_mtx_spi == NULL) { return 1U; }
    if (xSemaphoreTake(s_mtx_spi, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        s_lock_timeout++;      // ★ 超时了要计数
        return 0U;
    }
    return 1U;
}
```

**超时次数会被打印出来**（命令 `s` 或 `#STK` 行里的 `lock_tmo=`）。
0 = 正常；非 0 = 有人持锁时间过长，值得查。

> **"有统计才叫工程实现"** —— 丢数据不可怕，丢了不知道才可怕。

---

## 四、`vTaskDelayUntil` vs `vTaskDelay`（**必问，也是简历原话**）

### 看图就懂

```
【vTaskDelay(1000)】—— "从现在起再等 1000 ms"

  时刻  0        1000      2000      3000
        │ 干活35ms │ 干活35ms │ 干活35ms │
        ├─────────┼─────────┼─────────┤
        │         │         │         │
        └─> 睡着  └─> 睡着  └─> 睡着
        每轮实际周期 = 1000 + 35 = 1035 ms
        → 误差【累积】！跑 100 轮就慢了 3.5 秒

【vTaskDelayUntil(&last, 1000)】—— "到 next 这个绝对时刻叫我"

  时刻  0        1000      2000      3000
        │ 干活35ms │ 干活35ms │ 干活35ms │
        ├─────────┼─────────┼─────────┤
        │         │         │         │
        └─ 到点就醒 └─ 到点就醒 └─ 到点就醒
        每轮实际周期 = 1000 ms，误差【不累积】
```

### 代码

```c
static void task_acq(void *arg)
{
    TickType_t last = xTaskGetTickCount();   // ★ 必须先取一次当前时刻
    ...
    for (;;)
    {
        /* ...干活... */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000U));
    }
}
```

### 一个必须理解的副作用

如果某一轮**真的超时**了（比如 Flash 擦除卡了 400 ms），
`vTaskDelayUntil` 会**立刻返回**以追赶进度 —— 也就是说那一轮的间隔会被压缩。

**这正是我们要的语义**：宁可某轮被压缩，也不让整体节拍往后漂。
因为"1 秒一次"这个承诺是面向整个运行期的，不是面向某一轮的。

> **面试点**："`vTaskDelayUntil` 参数里的 `last` 为什么要在循环外初始化？"
> —— 它是"上一次的目标唤醒时刻"。如果在循环内用 `xTaskGetTickCount()` 重新初始化，
> 就退化成 `vTaskDelay` 了，白用。

---

## 五、互斥锁的对照实验（W3 D4，**面试最有说服力的一段**）

### 怎么做

```c
/* Core/Inc/app_config.h */
#define APP_MUTEX_ENABLE   1      // ← 改成 0 再编译一次
```

`log_begin()` / `log_end()` 在 `APP_MUTEX_ENABLE = 0` 时变成**空操作**：

```c
void log_begin(void)
{
#if APP_MUTEX_ENABLE
    if (s_mtx != NULL) { (void)xSemaphoreTake(s_mtx, portMAX_DELAY); ... }
#endif
}
```

### 现象

**`APP_MUTEX_ENABLE = 0`（关锁）** —— 两个任务同时打印，字节交错：

```
[0012s] seq=12 T=2531c H=4823c P=101[0012s] seq=12 store=ok
                     325Pa store=ok
```

```
!!! ALARM humidity >= 65.00 %RH a[0013s] seq=13 T=2532c H=...
```

**`APP_MUTEX_ENABLE = 1`（加锁）** —— 每行完整：

```
[0012s] seq=12 T=2531c H=4823c P=101325Pa store=ok
[0013s] seq=13 T=2532c H=4824c P=101326Pa store=ok
```

### 为什么会这样（原理）

`uart_dbg_str("abc")` 是**逐字节**发出去的，一整行要花几毫秒。
这期间如果高优先级任务抢占了 CPU 也去打印，
它发的字节就会**插在**前一条消息的中间。

**关键**：这不是"显示问题"，它揭示了一个真实风险 ——
任何"多步操作必须不可分割"的地方，都可能被任务切换打断。
串口打印只是**最容易看见**的那一个。

> **面试怎么说**：
> "我做了一个对照实验：把互斥锁去掉，串口输出立刻出现两行文字互相插队；
> 加回锁就恢复。这让我确认了临界区保护的必要性。
> 而且这个道理不只在打印上 —— 我们共享的 I2C 和 SPI 总线也是一样的，
> 一次事务被另一个任务插进来，从机侧就完全乱套了。"

### 日志里还能看到一个数：`uart_hold_us`

```c
void log_end(void)
{
    uint32_t held = delay_now_us() - s_hold_t0;
    if (held > s_max_hold) { s_max_hold = held; }
    (void)xSemaphoreGive(s_mtx);
}
```

这是"**这把锁被持有多久**"的历史最大值。

**为什么要盯它？**
临界区的长度**直接等于**所有等锁任务的等待时间。
超过 20 ms 就说明有人在持锁期间做了大量串口输出 ——
这种代码"功能是对的，但会拖累系统实时性"，属于典型的**能跑但不好**的写法。

> 这个数字在 `#STK` 行里输出，`stack_budget.py` 会给出告警。

---

## 六、栈水位实测（W3.5，简历那个 `[实测后填]` 的来源）

### ⚠️ 先纠正一个几乎人人都犯的错

`uxTaskGetStackHighWaterMark()` 返回的是 ——

> **【历史最小剩余量】**（单位：字），**不是"已用的峰值"**。

所以：

```
峰值已用 = 分配栈大小 − 剩余水位
推荐分配 = 峰值已用 × 1.5
```

**新手最容易搞反**：把"剩余"当成"已用"，于是算出的栈只剩一个零头，一跑就溢出。
（`tools/stack_budget.py` 里专门写了这段提醒。）

### 正确流程（不能颠倒）

```
① 先用宽裕档跑起来（APP_STACK_MEASURED = 0：acq 256 / proc 256 / comm 384 字）
       ↓
② 跑一段覆盖最坏情况的测试（越长越准；24 h 最佳）
       ↓
③ 读 #STK 行里的剩余水位（每 60 秒自动打一行，也可以发命令 s 立即打）
       ↓
④ 用 tools/stack_budget.py 算：峰值 × 1.5
       ↓
⑤ 把推荐值填进 app_tasks.c 的 APP_STACK_MEASURED 分支，改成 1，重新编译
       ↓
⑥ 再跑一遍，确认新的剩余水位【仍然 > 0】（有余量才算真的安全）
```

### 代码里的两档

```c
#if APP_STACK_MEASURED
/* ★★ 待填：把下面三个值换成【你实测出来的峰值 × 1.5】（单位：字）★★ */
#warning "APP_STACK_MEASURED=1：请先把 STK_ACQ / STK_PROC / STK_COMM 换成实测峰值 x1.5"
#define STK_ACQ     256U
#define STK_PROC    256U
#define STK_COMM    384U
#else
#define STK_ACQ     256U    /* 宽裕档：先保证不溢出，再谈节省 */
#define STK_PROC    256U
#define STK_COMM    384U
#endif
```

> **`#warning` 是故意留的**：只要你把开关改成 1 却没换数字，编译时会跳出一条醒目告警。
> 这是防"没实测就直接用"的机械保险。

### 压缩比怎么算（填简历前必须想清楚）

`stack_budget.py` 默认拿"**每任务统一 512 字**"当对照基准 ——
因为那正是"拍脑袋写法"的典型代表。

```bash
python tools/stack_budget.py log.txt --alloc acq=256,proc=256,comm=384 --baseline 512
```

输出会直接给你可以粘进简历的句子：

```
用 uxTaskGetStackHighWaterMark 实测各任务栈水位并重新分配栈空间
（压缩 XX%）—— 相对「每任务统一 512 字」的经验值方案，
总栈从 1536 字降至 NNN 字（6144 B → NNNN B）。
```

> **⚠️ 三条硬规矩**：
> ① `--alloc` 必须和你**当时烧录的固件一致**（开机横幅的 `#BUILD,APP_STACK_MEASURED=` 可佐证）
> ② 压缩比**必须说明是相对哪个基准**，否则面试官会追问到底，答不出就露馅
> ③ **不许用我给的示例值** —— 这个数字必须是你自己跑出来的

### 稳定性保障（已开）

```c
#define configCHECK_FOR_STACK_OVERFLOW   2   // 栈尾填 0xA5A5A5A5 magic，切换时校验
#define configUSE_MALLOC_FAILED_HOOK     1   // 堆耗尽时报警
```

溢出/堆耗尽时会调用 `main.c` 里的钩子，**先打一行字再停住**：

```
!!! STACK OVERFLOW in task: proc
!!! 对策: 读该任务的 uxTaskGetStackHighWaterMark，按峰值x1.5 加大栈
```

> **为什么钩子里"只打一行就死循环"？**
> 因为它跑在**异常上下文**，此刻内核状态已不可信。
> 在里面再调用 FreeRTOS API 会二次破坏状态，把水搅得更浑。
> **死于"知道死在哪"，比"活着但行为诡异"好得多。**

---

## 七、没有 CubeMX，移植 FreeRTOS 要自己接哪些东西

执行计划里写的"三件事"，在本工程的具体落点：

| # | 要接什么 | 本工程在哪 | 不接会怎样 |
|---|---|---|---|
| **1** | **SysTick 归 FreeRTOS** | 不用 HAL 的 `HAL_Init()`，所以**没有 TIM4 时基**；`FreeRTOSConfig.h` 里 `SystemCoreClock` 已由 `clock_init_72mhz()` 更新 | tick 长度错 → `vTaskDelay(1000)` 变成 111 ms |
| **2** | **PendSV / SVC 优先级设最低** | `port.c` 内部用 `configKERNEL_INTERRUPT_PRIORITY` 设置；`FreeRTOSConfig.h` 里那三个宏把内核函数改名成 CMSIS 标准名 | 调度器起不来，或**偶发**任务切换异常 |
| **3** | **链接脚本里的栈与堆** | `STM32F103C8TX_FLASH.ld`：`_Min_Heap_Size = 0x200`、`_Min_Stack_Size = 0x400` | 链接报 RAM overflow；或运行时栈互相踩 |

### ⚠️ 第 2 件事有个经典翻车点

`FreeRTOSConfig.h` 里有这三行：

```c
#define vPortSVCHandler      SVC_Handler
#define xPortPendSVHandler   PendSV_Handler
#define xPortSysTickHandler  SysTick_Handler
```

于是 `port.c` 会以 `SVC_Handler` / `PendSV_Handler` / `SysTick_Handler` 三个名字定义函数。

**所以 `stm32f1xx_it.c` 里绝对不能定义这三个函数**，否则链接报
`multiple definition of 'PendSV_Handler'`。

**为什么 CubeMX 那版没这个问题？** 因为它是靠 `cmsis_os2.c` 里的一堆宏
（`USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION` 之类）绕开的。
本工程走**原生 API**，绕开机制不存在了，就必须手动记住"这三个别写"。
（`stm32f1xx_it.c` 文件头有大字警告。）

### 与 CubeMX 版 FreeRTOSConfig.h 的关键差异

| 配置 | CubeMX 版 | 本工程 | 为什么 |
|---|---|---|---|
| `configSUPPORT_STATIC_ALLOCATION` | 1 | **0** | 为 1 时内核会调用 `vApplicationGetIdleTaskMemory()` 要静态内存。CubeIDE 里是靠 `cmsis_os2.c` 提供的；走原生 API 就没人提供 → **链接报未定义** |
| `configUSE_TIMERS` | 1 | **0** | 不用软件定时器，省一个任务（约 1 KB RAM）。**20 KB RAM 上这不是可选项** |
| `configMAX_PRIORITIES` | 56 | **8** | 只用 2/3/4 三档。每个优先级都要占就绪链表节点 |
| `configUSE_TRACE_FACILITY` | 1 | **0** | 不用 `vTaskList()`，省 Flash |
| `configASSERT` | 关中断死循环 | **先打一行字再死** | 断言失败时"死在哪能看到"比"程序不动了"强太多 |
| CMSIS 相关宏 | 一堆 | 全删 | 走原生 API 用不到 |

---

## 八、20 KB RAM 预算账本（必须算，不能拍）

| 项 | 占用 | 说明 |
|---|---|---|
| FreeRTOS 堆（`ucHeap`） | **9216 B** | `configTOTAL_HEAP_SIZE`，在 `.bss` 里 |
| ↳ 其中 3 个任务栈 | 256+256+384 字 = **3584 B** | 从堆里分配 |
| ↳ 其中 3 个 TCB + 2 队列 + 2 互斥锁 + 1 信号量 + Idle 任务 | 约 **1750 B** | |
| ↳ 堆剩余余量 | 约 **3900 B** | 留白，防止 `xTaskCreate` 失败 |
| 其他 `.bss`（`s_buf`、自检批缓冲、日志缓冲、校准系数…） | 约 **936 B** | |
| `._user_heap_stack` | **1536 B** | 链接脚本保留：C 库堆 512 B + 主栈 1 KB |
| **合计** | **11712 B ≈ 11.4 KB / 20 KB（57%）** | 实测值，见 `make size` |

**主栈（MSP）那 1 KB 是干什么的？**
只用于两件事：① 复位到 `vTaskStartScheduler()` 之前的启动代码；
② **所有中断服务程序**。任务栈是另一回事（从 FreeRTOS 堆里分）。

> **面试点**："你有 20 KB RAM，怎么分配？"
> 答：把 FreeRTOS 堆当"任务栈与 IPC 对象的池子"，
> 主栈只服务中断，两笔账分开算 —— 这就是为什么链接脚本里要留 `_Min_Stack_Size`。

---

## 九、验收怎么做

| 验收项 | 怎么测 | 判据 |
|---|---|---|
| 3 任务跑起来 | 看串口 | 每秒一行 `[Ns] seq=.. T=..c H=..c P=..Pa store=ok` |
| 队列能传数据 | 看 `store=ok` | `ok` = 处理工成功写进 Flash |
| 周期不漂移 | 逻辑分析仪接 **PC13** | 每 **10 秒**跳变一次；量 100 个间隔，累积误差应 < 10 ms |
| **去锁复现** | `APP_MUTEX_ENABLE=0` 编译烧录 | 30 秒内必看到**两行文字互相插队** |
| **加锁恢复** | 改回 1 | 输出每行都完整 |
| 栈水位实测 | 命令 `s` 或等自动打印 | `#STK,...` 行，各任务剩余水位**都 > 0** |
| 堆没耗尽 | 同上 | `heap_free` 为正且**稳定**（持续下降 = 泄漏） |
| 24 小时稳定 | `APP_UART_VERBOSE=0` 长跑 | 连续 24 h 不死机、不溢出、记录数线性增长 |

### 24 小时长跑前必做的一件事

```c
#define APP_UART_VERBOSE   0    // ← 必须改！
```

不改的话，一天 86400 行日志（每行 70 字节 ≈ 6 MB），
串口助手会卡死、日志文件也会大到没法看。

---

## 十、这一周结束时你应该能回答

1. 为什么用 `vTaskDelayUntil` 不用 `vTaskDelay`？那个 `last` 为什么要放循环外？
2. 队列、信号量、互斥锁分别传什么？本工程各自用在哪？
3. 优先级怎么定的？为什么通信任务优先级最低？
4. 取锁为什么不用 `portMAX_DELAY`？超时计数有什么用？
5. 互斥锁和信号量的区别是什么？**优先级继承**是怎么回事？
6. `uxTaskGetStackHighWaterMark` 返回的是"已用"还是"剩余"？（**这题会刷掉很多人**）
7. 为什么先给宽裕栈、实测后再收紧？反过来会怎样？
8. 栈溢出的钩子函数里为什么只能打一行就死循环？
9. 没有 CubeMX 移植 FreeRTOS，要自己接哪三件事？
10. 你的 20 KB RAM 是怎么分配的？主栈和任务栈为什么要分开算？
