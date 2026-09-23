/* ============================================================
 * FreeRTOSConfig.h —— 本项目自己的 FreeRTOS 配置
 *
 * 【和 CubeMX 生成版本的区别】
 *   CubeMX 那版是为 CMSIS-RTOS_V2 包装层准备的：
 *     · configSUPPORT_STATIC_ALLOCATION = 1
 *       → 会要求应用实现 vApplicationGetIdleTaskMemory /
 *         vApplicationGetTimerTaskMemory 两个回调来提供静态内存。
 *         CubeIDE 工程里是靠 cmsis_os2.c 提供的，所以看不出来。
 *         本项目【直接用原生 FreeRTOS API】（简历上写的
 *         vTaskDelayUntil / xQueueSend / xSemaphoreTake 都是原生 API），
 *         不需要包装层，也就没人提供那两个回调 ——
 *         所以这里必须把静态分配关掉，否则链接会报未定义。
 *     · 带一堆 CMSIS 相关的宏（CMSIS_device_header、
 *       USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 等），
 *       那是因为 cmsis_os2.c 和 port.c 会同时定义 SysTick_Handler
 *       而互相冲突。
 *   自己写一版，把不用的全关掉，Flash 和 RAM 都省一点 ——
 *   这在只有 20 KB RAM 的 F103 上不是可选项。
 * ============================================================ */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

/* ============================================================
 * 调度器基本行为
 * ============================================================ */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
/* 用通用 C 实现选下一个任务，不用 CLZ 汇编优化。
 * 代价是切换慢几个周期，好处是行为完全透明、好调试。
 * 优先级只有 5 档，这点开销完全不值得优化掉。 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      ( SystemCoreClock )

/* 1 ms 一个 tick。
 * ★ 这个值决定 vTaskDelay(1) 的最小粒度，也决定 tick 中断频率。
 *   1000 Hz → 每秒 1000 次 SysTick 中断。
 *   本项目最短周期是 1 s，其实 100 Hz 就够；
 *   但 1000 Hz 让 pdMS_TO_TICKS() 恒等于毫秒数，可读性更好、
 *   也不会出现"1 ms 被四舍五入成 0 tick"的坑（100 Hz 下会）。 */
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )

/* 优先级档数：本项目只用到 2/3/4，给 8 档足够且省 RAM
 * （每个任务的就绪列表都要占 configMAX_PRIORITIES 个链表节点）*/
#define configMAX_PRIORITIES                    ( 8 )

/* Idle 任务栈：128 字 = 512 B。idle 任务本身很轻，但它里面会跑
 * 我们的 configCHECK_FOR_STACK_OVERFLOW 检查逻辑，别给太小。 */
#define configMINIMAL_STACK_SIZE                ( ( unsigned short ) 128 )

/* ============================================================
 * 内存
 * ============================================================ */
/* heap_4 用的堆总大小。F103C8T6 只有 20 KB RAM，
 * 这里给 9216 B，预算明细见 docs/05_验收手册.md 的 RAM 账本。
 * ★ 给多了主栈不够，给少了创建任务失败 —— 这个数字是要算的，不能拍。 */
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) 9216 )
#define configAPPLICATION_ALLOCATED_HEAP        0

/* 只用动态分配。见文件头的说明。 */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* ============================================================
 * 内核对象开关（只开用得到的，省 Flash）
 * ============================================================ */
#define configUSE_MUTEXES                       1   /* 保护 I2C / SPI / UART 共享资源 */
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_QUEUE_SETS                    0   /* 用不到它，省几百字节 Flash */
#define configUSE_TASK_NOTIFICATIONS            1   /* 内核内部依赖它 */
#define configUSE_TIMERS                        0   /* 不用软件定时器，省一个任务约 1 KB RAM */
#define configUSE_CO_ROUTINES                   0

/* ============================================================
 * 调试与诊断（这一节是本项目的重点）
 * ============================================================ */
/* 任务名最大长度："acq"/"proc"/"comm" 够用 */
#define configMAX_TASK_NAME_LEN                 ( 8 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TRACE_FACILITY                0   /* 不用 vTaskList，省 Flash */

/* ★★ configCHECK_FOR_STACK_OVERFLOW = 2 ★★
 *   0 = 不检查（最快，但栈溢出了你不知道，只会看到莫名其妙的跑飞）
 *   1 = 任务切换时检查栈指针是否越界
 *   2 = 1 + 在任务栈末尾填 0xA5A5A5A5 magic，切换时校验 magic 有没有被改写
 *       —— 能抓到"曾经溢出但已经退回来"的隐蔽情况，代价是切换稍慢。
 *   栈溢出在 20 KB RAM 上是最常见的死机原因，这个开销非常值。
 *   溢出时会调用 vApplicationStackOverflowHook（实现在 main.c）。 */
#define configCHECK_FOR_STACK_OVERFLOW          2

/* 堆耗尽钩子。xTaskCreate / xQueueCreate 失败时调用。
 * 对只有 9 KB 堆的工程来说，这是最该盯的一声警报。 */
#define configUSE_MALLOC_FAILED_HOOK            1

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_POSIX_ERRNO                   0

/* ============================================================
 * 按需裁剪 API（用不到的关掉，每个能省几十到几百字节 Flash）
 * ============================================================ */
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 1   /* ★ 采集任务靠它保证周期不漂移 */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          0
#define INCLUDE_xTaskGetCurrentTaskHandle       0
#define INCLUDE_uxTaskGetStackHighWaterMark     1   /* ★ 栈水位实测靠它 */
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xQueueGetMutexHolder            0
#define INCLUDE_xTaskAbortDelay                 0

/* ============================================================
 * Cortex-M3 中断优先级
 *
 * ARM 规定 NVIC 优先级是"数值越小、优先级越高"。
 * configKERNEL_INTERRUPT_PRIORITY        —— PendSV / SysTick 用的（最低）
 * configMAX_SYSCALL_INTERRUPT_PRIORITY   —— 能调用 FromISR API 的最高优先级门槛
 *
 * ★ 铁律：任何要调用 xxxFromISR() 的中断，其优先级数值
 *   【必须 ≥ configMAX_SYSCALL_INTERRUPT_PRIORITY】。
 *   比它更高的中断里一旦调用内核 API，会破坏内核临界区保护，
 *   结果是"偶发、难复现"的数据损坏 —— 这类 bug 能耗掉一整周。
 *
 * 本项目 USART1 中断优先级设为 6（≥5，合规），
 * 而且它内部【不调用任何 FreeRTOS API】，所以更安全。
 * ============================================================ */
#ifdef __NVIC_PRIO_BITS
  #define configPRIO_BITS                       __NVIC_PRIO_BITS
#else
  #define configPRIO_BITS                       4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* ============================================================
 * 断言
 * ★ 与 CubeMX 那版不同的是：这里让断言"可见"。
 *   配置错误时先往串口打一行再停住 —— 死在哪里能看到，
 *   比"程序不动了"强太多。串口函数在异常上下文里纯轮询发送，
 *   不碰任何内核对象，所以这样用是安全的。
 *
 *   这里用 portDISABLE_INTERRUPTS() 而不是 taskDISABLE_INTERRUPTS()：
 *   前者由 portmacro.h 提供，而 portmacro.h 是 FreeRTOS.h 直接
 *   包含进来的；task.h 不是。configASSERT 会在 queue.c / list.c /
 *   heap_4.c / port.c 等多处展开，用 port 层的宏才处处可用。
 * ============================================================ */
extern void uart_dbg_str(const char *s);
#define configASSERT( x )                                          \
    if( ( x ) == 0 )                                               \
    {                                                              \
        uart_dbg_str("\r\n!!! configASSERT FAILED: " #x "\r\n");    \
        portDISABLE_INTERRUPTS();                                  \
        for( ;; );                                                 \
    }

/* ============================================================
 * 把内核的异常处理函数映射到 CMSIS 的标准名字
 *
 * ★ 这三行必须和启动文件（startup_stm32f103xb.s）里的弱符号对上：
 *   port.c 里定义的是 vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler，
 *   靠下面三个宏改名成 SVC_Handler / PendSV_Handler / SysTick_Handler，
 *   才能覆盖启动文件里的弱定义。
 *
 * ⚠️ 反过来说：stm32f1xx_it.c 里【绝对不能再定义】这三个函数，
 *    否则链接期报 multiple definition —— 这是移植时最常见的错误。
 *
 * 这也是"没 CubeMX 帮忙要自己接的三件事"里的第 2 件：
 *   ① SysTick 时基归 FreeRTOS（本项目不用 HAL 的 HAL_Init，
 *      没有 TIM4 时基，SysTick 完全由内核接管）
 *   ② PendSV / SVC 优先级设为最低（port.c 内部用
 *      configKERNEL_INTERRUPT_PRIORITY 设置，即上面那个宏）
 *   ③ 链接脚本里的栈与堆（见 STM32F103C8TX_FLASH.ld）
 * ============================================================ */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
