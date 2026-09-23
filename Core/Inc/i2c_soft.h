/* ============================================================
 * i2c_soft.h —— 软件模拟（bit-bang）I2C 主机
 * ============================================================ */
#ifndef I2C_SOFT_H
#define I2C_SOFT_H

#include <stdint.h>

/* 初始化：PB6=SCL / PB7=SDA，配成【开漏输出】并释放总线 */
void i2c_soft_init(void);

/* 动态调整半周期延时（µs）。排查不过时按 4 → 6 → 10 逐级降速。 */
void i2c_soft_set_delay_us(uint8_t us);

/* 读从机寄存器：先写寄存器地址（Repeated Start 不产生 STOP），再连续读 len 字节。
 * 返回 0 = 成功；1 = 设备地址 NACK；2 = 寄存器地址 NACK；3 = 读方向地址 NACK */
uint8_t i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len);

/* 写从机寄存器：一次写入 len 字节数据。
 * 返回 0 = 成功；1 = 设备地址 NACK；2 = 寄存器地址 NACK；3 = 数据 NACK */
uint8_t i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint8_t len);

/* 总线复位：从机在通信中途被打断时可能把 SDA 一直拉着不放，
 * 表现为后续所有事务都失败。发 9 个 SCL 脉冲把它"顶"出来。
 * 返回 1 = 复位后总线空闲（SDA/SCL 都是高）；0 = 总线仍被占用 */
uint8_t i2c_soft_bus_recover(void);

/* 诊断计数：NACK 累计 / 总线复位累计 */
uint32_t i2c_soft_nack_count(void);
uint32_t i2c_soft_recover_count(void);

#endif /* I2C_SOFT_H */
