/* ============================================================
 * clock_init.h —— 系统时钟树初始化（纯寄存器，不经 HAL）
 * ============================================================ */
#ifndef CLOCK_INIT_H
#define CLOCK_INIT_H

#include <stdint.h>

/* 把 SYSCLK 从复位的 HSI 8 MHz 提到 HSE 8 MHz × PLL9 = 72 MHz。
 * 返回 0 = 成功；1 = HSE 起振超时；2 = PLL 锁定超时；3 = 切换 SYSCLK 超时。
 *
 * ★ 调用顺序：必须早于任何依赖时钟的外设初始化（串口、I2C、SPI 延时标定）。
 *   因为 BRR、I2C 延时、DWT 每微秒周期数都依赖真实频率。 */
uint8_t clock_init_72mhz(void);

/* 回读当前 SYSCLK 频率（通过 RCC->CFGR 的 SWS 位与倍频/分频位现算），
 * 用于自检：把结果和 72000000 比对，证明"真的跑在 72 MHz 上"，
 * 而不是"我写了 72 MHz 就当它是 72 MHz"。 */
uint32_t clock_get_sysclk_hz(void);

/* 同上，回读 AHB / APB1 / APB2 的实际频率。
 * ★ APB1 必须 ≤ 36 MHz，这条上限一旦超了外设会时好时坏，
 *   把 PCLK1 打出来是最直接的确认方式。 */
uint32_t clock_get_hclk_hz(void);
uint32_t clock_get_pclk1_hz(void);
uint32_t clock_get_pclk2_hz(void);

#endif /* CLOCK_INIT_H */
