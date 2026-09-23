/* ============================================================
 * main.c —— 多总线数据采集终端主程序（寄存器裸机 + FreeRTOS）
 *
 * 【本文件与 CubeMX 生成版的根本区别】
 *   ① 不调用 HAL_Init()、不调用 HAL_RCC_OscConfig() / HAL_RCC_ClockConfig()
 *      → 时钟树是 clock_init_72mhz() 用寄存器一行行配的
 *   ② 不用 CMSIS-RTOS_V2 包装层（osKernelInitialize / osThreadNew）
 *      → 用原生 FreeRTOS API（vTaskStartScheduler）
 *   ③ 不用 HAL 的 TIM4 时基
 *      → SysTick 完全归 FreeRTOS 内核，专用于任务调度
 *
 *   所以简历上"不依赖 HAL 与现成驱动"这句话，在这个工程里是字面为真的。
 *
 * 【启动顺序为什么是这个顺序】
 *   clock_init_72mhz()   ← 一切的基础。后面所有东西都依赖它
 *        ↓
 *   delay_init()         ← 用 SystemCoreClock 标定 DWT，必须排在时钟之后
 *        ↓
 *   uart_dbg_init()      ← BRR 由 PCLK2 决定，同样必须排在时钟之后
 *        ↓
 *   app_tasks_start()    ← 建 IPC 对象与任务（此刻还不能跑，调度器没启动）
 *        ↓
 *   vTaskStartScheduler()← 从这里开始，控制权交给 FreeRTOS
 *
 *   顺序错的典型症状：串口全是乱码（BRR 按错频率算的）、
 *   I2C 超速不通（延时按错频率标定的），而且都"看起来像硬件坏了"。
 * ============================================================ */
#include "stm32f1xx.h"
#include "board.h"
#include "clock_init.h"
#include "delay_us.h"
#include "uart_dbg.h"
#include "app_tasks.h"
#include "app_config.h"
#include "self_test.h"
#include "recorder.h"
#include "crc16.h"

#include "FreeRTOS.h"
#include "task.h"

/* ============================================================
 * 启动横幅：把"我以为的"和"从寄存器读出来的"都打出来
 * ============================================================ */
static void print_boot_banner(uint8_t clk_rc)
{
    uart_dbg_str("\r\n\r\n");
    uart_dbg_str("========================================================\r\n");
    uart_dbg_str(" 多总线数据采集终端  |  STM32F103C8T6 寄存器裸机 + FreeRTOS\r\n");
    uart_dbg_str(" I2C(BME280) + SPI(W25Q64) + UART(printf/命令) 三总线\r\n");
    uart_dbg_str("========================================================\r\n");

    if (clk_rc != 0U)
    {
        uart_dbg_str("!! 时钟树初始化失败 rc=");
        uart_dbg_u32((uint32_t)clk_rc);
        uart_dbg_str("  (1=HSE未起振 2=PLL未锁定 3=SYSCLK切换超时)\r\n");
        uart_dbg_str("!! 后面所有基于 72 MHz 的计算（BRR、延时）都会失准\r\n");
    }

    /* ★ 时钟自证：这些数字是从 RCC->CFGR 反算出来的，不是宏。
     *   把"实测"和"期望"两行并排打出来，一眼就能看出配没配对。 */
    uart_dbg_str("#CLK,got:  SYSCLK=");
    uart_dbg_u32(clock_get_sysclk_hz());
    uart_dbg_str(" HCLK=");
    uart_dbg_u32(clock_get_hclk_hz());
    uart_dbg_str(" PCLK1=");
    uart_dbg_u32(clock_get_pclk1_hz());
    uart_dbg_str(" PCLK2=");
    uart_dbg_u32(clock_get_pclk2_hz());
    uart_dbg_str("\r\n");

    uart_dbg_str("#CLK,expect: SYSCLK=72000000 HCLK=72000000 PCLK1=36000000 PCLK2=72000000\r\n");
    uart_dbg_str("#CLK,APB1上限=36000000 —— 上面 PCLK1 超过它就会外设时好时坏\r\n");

    uart_dbg_str("#UART,BRR=");
    uart_dbg_u32(UART_DBG_BRR_VAL);
    uart_dbg_str(" (expect 625 = 0x271, baud=115200 err=0%), DWT=");
    uart_dbg_str((delay_is_dwt_ok() != 0U) ? "ok" : "FAIL -> 延时已退化为空循环");
    uart_dbg_str("\r\n");

    /* 把编译期的开关状态打出来 —— 做对照实验时最容易忘的就是
     * "我到底烧的是哪个配置"，打出来就不会搞混。 */
    uart_dbg_str("#BUILD,APP_MUTEX_ENABLE=");
    uart_dbg_u32((uint32_t)APP_MUTEX_ENABLE);
    uart_dbg_str(",APP_STACK_MEASURED=");
    uart_dbg_u32((uint32_t)APP_STACK_MEASURED);
    uart_dbg_str(",APP_UART_VERBOSE=");
    uart_dbg_u32((uint32_t)APP_UART_VERBOSE);
    uart_dbg_str(",REC_SIZE=");
    uart_dbg_u32((uint32_t)REC_SIZE);
    uart_dbg_str(",TWO_STEP_WRITE=");
    uart_dbg_u32((uint32_t)REC_TWO_STEP_WRITE);
    uart_dbg_str("\r\n");

    /* ============================================================
     * CRC-16/MODBUS 自检向量
     *
     * 用 CRC 界公认的检查值（check value）：ASCII 字符串 "123456789"
     * 在 CRC-16/MODBUS 参数下应得到 0x4B37。
     * 这个值在任何 CRC 参数表里都能查到，所以它可以用来
     * 独立验证"固件算的和 PC 算的是不是同一套算法"。
     *
     * ★ 这是"PC 端逐字节校验闭环"能成立的前提：
     *   如果两边 CRC 算法不一致，后面所有校验都会全军覆没，
     *   而且看起来会像是"Flash 坏了"。所以先钉住算法，再谈数据。
     *   tools/crc16_modbus.py 的 --selftest 会打出同一个值。
     * ============================================================ */
    {
        static const char VEC[9] = {'1','2','3','4','5','6','7','8','9'};
        uint16_t c = crc16_modbus((const uint8_t *)VEC, 9U);

        uart_dbg_str("#CRC,check='123456789',got=0x");
        uart_dbg_hex8((uint8_t)(c >> 8));
        uart_dbg_hex8((uint8_t)(c & 0xFFU));
        uart_dbg_str(",expect=0x4B37,");
        uart_dbg_str((c == 0x4B37U) ? "PASS" : "FAIL");
        uart_dbg_str(" (存储时低字节先发)\r\n");
    }

    /* ---- CRC-16/CCITT-FALSE 自检：记录【尾 CRC】用的就是它 ----
     * 头尾两个 CRC 必须用不同多项式，否则尾 CRC 会退化成常数
     * （详细推导见 crc16.h，实测两次都验证了这个退化）。
     * 开机自证一次；PC 端跑 tools/crc16_modbus.py --selftest 也能看到 0x29B1。 */
    {
        static const char vec[9] = {'1','2','3','4','5','6','7','8','9'};
        uint16_t c = crc16_ccitt((const uint8_t *)vec, 9U);

        uart_dbg_str("#CRC2,tail_algo=CCITT-FALSE,check='123456789',got=0x");
        uart_dbg_hex8((uint8_t)(c >> 8));
        uart_dbg_hex8((uint8_t)(c & 0xFFU));
        uart_dbg_str(",expect=0x");
        uart_dbg_hex8((uint8_t)(CRC16_CCITT_CHECK >> 8));
        uart_dbg_hex8((uint8_t)(CRC16_CCITT_CHECK & 0xFFU));
        uart_dbg_str(",");
        uart_dbg_str((c == CRC16_CCITT_CHECK) ? "PASS" : "FAIL");
        uart_dbg_str("\r\n");
    }

    uart_dbg_str("#CMD,发送 h 查看命令表\r\n");
}

int main(void)
{
    uint8_t rc;

    /* ============ W1 第一步：把时钟从复位的 HSI 8 MHz 提到 72 MHz ============ */
    rc = clock_init_72mhz();

    /* ============ 延时标定（依赖 SystemCoreClock）============ */
    delay_init();

    /* ============ 串口（BRR 依赖 PCLK2）============ */
    uart_dbg_init();

    print_boot_banner(rc);

    /* ============ 建立任务与 IPC 对象 ============ */
    rc = app_tasks_start();
    if (rc != 0U)
    {
        uart_dbg_str("!! app_tasks_start() 失败 rc=");
        uart_dbg_u32((uint32_t)rc);
        uart_dbg_str("\r\n!! 1/2/3 = IPC 对象创建失败，4/5/6 = 任务创建失败\r\n");
        uart_dbg_str("!! 多半是 configTOTAL_HEAP_SIZE 给少了\r\n");
        for (;;) { }
    }

    uart_dbg_str("[boot] 记录区已存 ");
    uart_dbg_u32(recorder_count());
    uart_dbg_str(" 条 / 容量 ");
    uart_dbg_u32(recorder_capacity());
    uart_dbg_str(" 条\r\n");

#if APP_SELFTEST_ON_BOOT
    /* 开机自检放在调度器启动【之前】：
     * 此刻是单线程，自检可以放心地用阻塞方式跑，
     * 也不用担心和 1 Hz 采集任务抢总线。 */
    uart_dbg_str("[boot] 开机自检中（APP_SELFTEST_ON_BOOT=1）...\r\n");
    selftest_run_all();
#endif

    uart_dbg_str("[boot] 启动 FreeRTOS 调度器 ...\r\n\r\n");

    vTaskStartScheduler();

    /* ============================================================
     * 正常情况下永远执行不到这里。
     * 能走到这一行只有一种可能：空闲任务都没能创建起来 ——
     * 几乎必然是 configTOTAL_HEAP_SIZE 不够。
     * ============================================================ */
    uart_dbg_str("!! vTaskStartScheduler() 返回了 —— 堆内存不足，空闲任务创建失败\r\n");
    for (;;) { }
}

/* ============================================================
 * FreeRTOS 钩子函数
 *
 * 【钩子函数里能做什么、不能做什么】
 *   能：往串口打一行（纯轮询、不碰内核对象）、关中断、死循环停住
 *   不能：调用任何 FreeRTOS API、再打印一堆东西、试图"恢复现场"
 *
 *   因为它们是在【调度器内部 / 异常上下文】被调起的，
 *   此刻内核状态已经不可信；在里面再动内核，等于把水搅得更浑。
 *   死于"知道死在哪"，比"活着但行为诡异"好得多。
 * ============================================================ */

/* 栈溢出（configCHECK_FOR_STACK_OVERFLOW = 2 时由内核调用）*/
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_dbg_str("\r\n!!! STACK OVERFLOW in task: ");
    uart_dbg_str((pcTaskName != NULL) ? pcTaskName : "?");
    uart_dbg_str("\r\n!!! 对策: 读该任务的 uxTaskGetStackHighWaterMark，按峰值x1.5 加大栈\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

/* 堆耗尽（configUSE_MALLOC_FAILED_HOOK = 1 时由 pvPortMalloc 调用，
 * 触发于 xTaskCreate / xQueueCreate / xSemaphoreCreate 申请内存失败）*/
void vApplicationMallocFailedHook(void)
{
    uart_dbg_str("\r\n!!! pvPortMalloc FAILED (heap exhausted) !!!\r\n");
    uart_dbg_str("!!! 对策: 调大 FreeRTOSConfig.h 的 configTOTAL_HEAP_SIZE，");
    uart_dbg_str("或检查有没有漏删的队列/任务\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}
