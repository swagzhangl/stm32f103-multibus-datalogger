/* ============================================================
 * delay_us.c —— DWT 周期计数器实现的微秒延时
 * ============================================================ */
#include "stm32f1xx.h"
#include "delay_us.h"

/* 复数形式存"每微秒的内核周期数"与"是否可用 DWT" */
static uint32_t s_cycles_per_us = 8U;
static uint8_t  s_dwt_ok         = 0U;

/* ============================================================
 * 退化路径：DWT 不可用时的空循环延时
 *
 * 什么情况下 DWT 会不可用：极少数 STM32 兼容芯片/内核里
 * CoreDebug->DEMCR 的 TRCENA 位写不进去，CYCCNT 就永远停在 0。
 * 与其让整个程序静默失效，不如先探测、再退化、并留下标记，
 * 让上层能在自检里把这件事报出来 —— 问题可见比问题消失重要。
 * ============================================================ */
static void busy_wait_us(uint32_t us)
{
    while (us != 0U)
    {
        /* 这个系数是按 72 MHz / -Og 估的，只用于退化场景，
         * 精度要求不高（I2C 只要在 100 kHz 附近即可，容错 ±50%）。 */
        for (volatile uint32_t i = 0U; i < 6U; i++) { }
        us--;
    }
}

void delay_init(void)
{
    uint32_t probe;

    /* TRCENA = 1：使能跟踪与调试模块（DWT 属于其中） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* ---- 探测：计数器到底走不走 ---- */
    probe = DWT->CYCCNT;
    for (volatile uint32_t i = 0U; i < 200U; i++) { }

    s_dwt_ok = (DWT->CYCCNT != probe) ? 1U : 0U;

    if (s_dwt_ok != 0U)
    {
        /* 每微秒的周期数 = 主频 / 1e6。72 MHz → 72 */
        s_cycles_per_us = SystemCoreClock / 1000000UL;
        if (s_cycles_per_us == 0U) { s_cycles_per_us = 1U; }
    }
}

uint8_t delay_is_dwt_ok(void)
{
    return s_dwt_ok;
}

void delay_us(uint32_t us)
{
    if (s_dwt_ok == 0U)
    {
        busy_wait_us(us);
        return;
    }

    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * s_cycles_per_us;

    /* ★ 这里必须用无符号减法：CYCCNT 是 32 位，约 59.6 秒就会溢出回绕一次。
     *   (now - start) 在无符号语义下天然正确跨越回绕点，
     *   写成 (now >= start + ticks) 就会在回绕瞬间卡死或提前退出。 */
    while ((DWT->CYCCNT - start) < ticks) { }
}

void delay_ms(uint32_t ms)
{
    while (ms != 0U)
    {
        delay_us(1000U);
        ms--;
    }
}

uint32_t delay_now_us(void)
{
    if (s_dwt_ok == 0U) { return 0U; }
    return DWT->CYCCNT / s_cycles_per_us;
}

uint32_t delay_us_period(void)
{
    if (s_dwt_ok == 0U) { return 0U; }
    /* CYCCNT 是 32 位，每 2^32 个【周期】回绕；换算成微秒要再除一次。
     * 72 MHz → 4294967296 / 72 = 59652323 µs ≈ 59.65 s。 */
    return (uint32_t)(0x100000000ULL / (uint64_t)s_cycles_per_us);
}

uint32_t delay_elapsed_us(uint32_t t0)
{
    uint32_t now;
    uint32_t period;

    if (s_dwt_ok == 0U) { return 0U; }

    now    = delay_now_us();
    period = delay_us_period();

    if (now >= t0)
    {
        return now - t0;               /* 没跨回绕点 */
    }

    /* 跨了回绕点：真实间隔 = (周期 - t0) + now。
     * 直接写 (now - t0) 会得到 ~4.2e9 的假值，见 delay_us.h 的说明。 */
    return (period - t0) + now;
}
