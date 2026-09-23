/* ============================================================
 * spi_soft.h —— bit-bang SPI 主机（支持四种模式）
 *
 * 【四种模式的本质】
 *   CPOL 只决定一件事：SCLK 空闲时是高还是低。
 *   CPHA 只决定一件事：在"第几个边沿"采样。
 *
 *   模式  CPOL CPHA  空闲  采样沿
 *   ----  ---- ----  ----  ----------------------
 *   Mode0   0    0    低    上升沿（第一个边沿）  ← W25Q64 用它
 *   Mode1   0    1    低    下降沿（第二个边沿）
 *   Mode2   1    0    高    下降沿（第一个边沿）
 *   Mode3   1    1    高    上升沿（第二个边沿）  ← W25Q64 也支持
 *
 * 【bit-bang 做四模式的优势】
 *   时钟和数据都是你用 GPIO 一个边沿一个边沿翻出来的，
 *   所以"四种模式"在代码里只差两件事：先翻谁、在哪个边沿读。
 *   写一个带 mode 参数的发送函数就全覆盖了。
 *   若用硬件 SPI，对应的是 SPI_CR1 的 CPOL(bit1) 和 CPHA(bit0)。
 * ============================================================ */
#ifndef SPI_SOFT_H
#define SPI_SOFT_H

#include <stdint.h>

#define SPI_MODE0   0U
#define SPI_MODE1   1U
#define SPI_MODE2   2U
#define SPI_MODE3   3U

/* 初始化：PA4=CS / PA5=SCK / PA6=MISO / PA7=MOSI，默认 Mode0、片选拉高 */
void spi_soft_init(void);

/* 切换模式。★ 切换后会立刻把 SCK 摆到新模式的空闲电平，
 *   否则 SCK 停在旧电平，从机看到的第一个边沿方向就是错的。 */
void spi_soft_set_mode(uint8_t mode);
uint8_t spi_soft_get_mode(void);

/* 位延时（µs）。0 = 全速（约 1~2 MHz，够 W25Q64 用）。
 * 用逻辑分析仪看波形时设成 2~5，把时钟降到几百 kHz，波形才看得清。
 * ★ 这也解释了"为什么 bit-bang 反而更适合教学"：速度可控，波形可读。 */
void spi_soft_set_bit_delay_us(uint8_t us);

void spi_cs_low(void);
void spi_cs_high(void);

/* 收发一个字节（MSB 先行，SPI 默认位序）。
 * 同时读写是 SPI 的固有特性：主机每发一位，从机就回一位。 */
uint8_t spi_soft_transfer(uint8_t tx);

/* 按指定模式发、按另一模式收 —— 用来演示"主从模式不匹配"会读到移位值。
 * 正常使用请不要调用它，它是教学/自检用的。 */
uint8_t spi_soft_transfer_ex(uint8_t tx, uint8_t tx_mode, uint8_t rx_mode);

/* 空闲一小段（给从机准备时间），单位 µs */
void spi_soft_idle_us(uint32_t us);

#endif /* SPI_SOFT_H */
