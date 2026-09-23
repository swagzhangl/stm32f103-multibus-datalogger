/* ============================================================
 * bme280.h —— BME280 温湿压传感器驱动（I2C）
 *
 * 【从机地址】
 *   0x76：模块上的 SDO 引脚接 GND（大多数模块默认）
 *   0x77：SDO 接 VCC
 *   读不到数据时，这是第一个要确认的地方。
 * ============================================================ */
#ifndef BME280_H
#define BME280_H

#include <stdint.h>

#define BME280_ADDR   0x76U

/* 物理量（定点整数，单位见注释）*/
typedef struct
{
    int32_t  temperature;   /* 0.01 ℃      （5123 = 51.23 ℃）*/
    uint32_t humidity;      /* 0.01 %RH    （4633 = 46.33 %RH）*/
    uint32_t pressure;      /* Pa          （101325 = 1013.25 hPa）*/
} bme280_data_t;

/* 33 个出厂校准系数（0x88–0xA1 共 26 B + 0xE1–0xE7 共 7 B）
 * PC 端复算温湿压时需要完整的一套，所以提供 getter。 */
typedef struct
{
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1, dig_H3;
    int16_t  dig_H2, dig_H4, dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

/* 原始量 + 补偿中间量 —— PC 端"逐位复用算"的全部输入与输出 */
typedef struct
{
    int32_t  adc_T, adc_P, adc_H;    /* 20 位原始 ADC（湿度是 16 位）*/
    int32_t  t_fine;                 /* 温度补偿中间量，气压/湿度都依赖它 */
    uint32_t comp_press_q24_8;       /* 气压补偿函数原始返回（/256 = Pa）*/
    uint32_t comp_hum_q22_10;        /* 湿度补偿函数原始返回（/1024 = %RH）*/
} bme280_raw_t;

/* 初始化：校验 chip id → 读 33 个校准系数 → 配置测量。
 * 返回 0 = 成功；10/11 = I2C 失败 / chip id 不对；2x = 校准系数读失败；
 * 30~32 = 配置寄存器写失败 */
uint8_t bme280_init(void);

/* 取一次测量结果（含合理性校验，不可信会自动重读一次）。
 * 返回 0 = 成功；1 = I2C 失败；2 = 连续两次数据都不可信（已计数）*/
uint8_t bme280_read(bme280_data_t *out);

/* 读取 chip id（0x60 = BME280，0x58 = BMP280 无湿度）*/
uint8_t bme280_get_chip_id(void);

/* 取校准系数 / 上一次成功读取的原始量与中间量（PC 复算用）*/
void bme280_get_calib(bme280_calib_t *out);
void bme280_get_raw(bme280_raw_t *out);

/* 被"合理性闸门"拦下并丢弃的样本总数 */
uint32_t bme280_get_reject_count(void);

#endif /* BME280_H */
