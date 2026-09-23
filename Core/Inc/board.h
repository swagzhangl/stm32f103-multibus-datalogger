/* ============================================================
 * board.h —— 板级配置总表（引脚 / 时钟 / 总线频率）
 *
 * 【为什么要有这个文件】
 *   本项目是"寄存器手写 + 三总线"，引脚和时钟数字散落在各个 .c 里
 *   最容易出的事就是"改了时钟忘了改串口 BRR"或"两个模块抢同一个引脚"。
 *   把它们集中到一处，改一处即可全工程生效 —— 这是工程化的第一步。
 *
 * 【接线表】（STM32F103C8T6 蓝板）
 *   功能        引脚     说明
 *   ----------  -------  ------------------------------------------
 *   LED         PC13     板载 LED，低电平点亮
 *   USART1_TX   PA9      接 CH340 的 RXD
 *   USART1_RX   PA10     接 CH340 的 TXD（本版新增，用于 PC 下发命令）
 *   I2C_SCL     PB6      开漏 + 上拉，接 BME280 SCL
 *   I2C_SDA     PB7      开漏 + 上拉，接 BME280 SDA
 *   SPI_CS      PA4      接 W25Q64 CS
 *   SPI_SCK     PA5      接 W25Q64 CLK
 *   SPI_MISO    PA6      接 W25Q64 DO
 *   SPI_MOSI    PA7      接 W25Q64 DI
 *   BUZZER      PB0      有源蜂鸣器，低电平触发（见 buzzer.c 说明）
 * ============================================================ */
#ifndef BOARD_H
#define BOARD_H

/* board.h 里用到了 GPIOC / GPIOB / GPIOA 这些 CMSIS 的外设结构体指针，
 * 所以必须把设备头文件带进来 —— 这样任何 #include "board.h" 的文件
 * 都不需要再单独 include 一次 stm32f1xx.h。 */
#include "stm32f1xx.h"

/* ---------- 主时钟 ---------- */
#define BOARD_HSE_HZ        8000000UL     /* 板上晶振 8 MHz */
#define BOARD_SYSCLK_HZ     72000000UL    /* HSE × PLL9 = 72 MHz */
#define BOARD_HCLK_HZ       72000000UL    /* AHB  = SYSCLK / 1 */
#define BOARD_PCLK1_HZ      36000000UL    /* APB1 = HCLK / 2（上限就是 36 MHz）*/
#define BOARD_PCLK2_HZ      72000000UL    /* APB2 = HCLK / 1 */

/* ---------- 调试串口 ---------- */
#define BOARD_UART_BAUD     115200UL

/* ============================================================
 * BRR 计算（USART 过采样 16 倍时，BRR 的数值就等于 fCK / Baud）
 *
 *   BRR = 72e6 / 115200 = 625 = 0x271
 *   反推：USARTDIV = 625 / 16 = 39.0625
 *         实际波特率 = 72e6 / (16 × 39.0625) = 115200 → 误差 0.00%
 *
 * ★ 这两个数字（625 与 0%）面试可以直接讲，是可当场推导的。
 * ============================================================ */
#define BOARD_UART_BRR      ((BOARD_PCLK2_HZ + (BOARD_UART_BAUD / 2UL)) / BOARD_UART_BAUD)

/* ---------- LED ---------- */
#define LED_PORT_GPIO       GPIOC
#define LED_PIN             13U

/* ---------- 蜂鸣器 ---------- */
#define BUZZER_PORT_GPIO    GPIOB
#define BUZZER_PIN          0U

/* ---------- 软件 I2C ---------- */
#define I2C_PORT_GPIO       GPIOB
#define I2C_SCL_PIN         6U
#define I2C_SDA_PIN         7U

/* ---------- 软件 SPI ---------- */
#define SPI_PORT_GPIO       GPIOA
#define SPI_CS_PIN          4U
#define SPI_SCK_PIN         5U
#define SPI_MISO_PIN        6U
#define SPI_MOSI_PIN        7U

/* ---------- 报警阈值（湿度，单位 0.01 %RH）----------
 * 65% 触发 / 62% 回差解除 —— 回差 3% 足以吃掉 BME280 的 ±3%RH 精度抖动，
 * 避免实测值在阈值附近来回跳导致蜂鸣器"哒哒哒"地抖。
 * 这两个数和你另一版 BME280 项目保持一致。 */
#define ALARM_HUMI_ON       6500U         /* 65.00 %RH 触发 */
#define ALARM_HUMI_OFF      6200U         /* 62.00 %RH 解除 */

#endif /* BOARD_H */
