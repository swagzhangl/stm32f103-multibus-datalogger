/* ============================================================
 * bme280.c —— BME280 驱动实现（寄存器级 I2C + Bosch 整数补偿）
 *
 * 【为什么必须读校准系数】
 *   BME280 输出的是 20 位 ADC 原始值，它随每颗芯片的工艺偏差而不同。
 *   芯片出厂时把补偿参数（共 33 个系数）烧在 NVM 里，必须读出这些系数、
 *   代入补偿公式，才能算出真实的温湿压。不读校准系数只能得到"原始值"，
 *   不同芯片之间没有可比性，换一颗芯片数值就全变了。
 *
 * 【为什么用整数而不是浮点】
 *   F103 没有硬件浮点单元（FPU），浮点靠软件模拟，慢且占 Flash。
 *   Bosch 提供了官方"整数版"补偿算法（结果已按定点放大），
 *   精度足够且快几十倍 —— 嵌入式里这是标准做法。
 *
 * 【温湿压必须一起算，不能分开】
 *   温度补偿会算出中间量 t_fine，气压和湿度的补偿公式都用它。
 *   所以顺序是固定的：先温度 → 再气压/湿度。
 *   这也是"读数据必须一次连读 8 字节"的原因：三个量来自同一批转换。
 * ============================================================ */
#include "bme280.h"
#include "i2c_soft.h"
#include "uart_dbg.h"
#include "delay_us.h"

/* ---------- 寄存器地址（BME280 数据手册 5.x 节）---------- */
#define REG_CALIB00     0x88U   /* 校准数据第 1 段：0x88~0xA1（26 B）*/
#define REG_CHIP_ID     0xD0U   /* 芯片 ID：BME280 = 0x60，BMP280 = 0x58 */
#define REG_STATUS      0xF3U   /* 状态寄存器：bit3 = measuring */
#define REG_CALIB26     0xE1U   /* 校准数据第 2 段：0xE1~0xE7（7 B）*/
#define REG_CTRL_HUM    0xF2U   /* 湿度过采样（必须先于 ctrl_meas 写）*/
#define REG_CTRL_MEAS   0xF4U   /* 温度/气压过采样 + 工作模式 */
#define REG_CONFIG      0xF5U   /* 待机时间 / IIR 滤波 */
#define REG_DATA_START  0xF7U   /* 数据区起始：0xF7~0xFE（8 B）*/

#define STATUS_MEASURING    0x08U   /* bit3：1 = 正在转换 */

/* ---------- 校准系数 ---------- */
static bme280_calib_t s_cal;

/* t_fine：温度补偿的中间结果，气压和湿度都要用它 —— 所以它是文件级状态 */
static int32_t t_fine;

/* ---------- 最近一次成功读取的原始量（诊断 / PC 复算用）---------- */
static bme280_raw_t s_raw;

/* ---------- chip id 缓存 ---------- */
static uint8_t s_chip_id = 0U;

/* ============================================================
 * 补偿算法（Bosch 官方整数版，数据手册 4.2.3）
 * ============================================================ */

/* 温度补偿：返回 0.01 ℃，同时算出全局 t_fine */
static int32_t compensate_temperature(int32_t adc_T)
{
    int32_t var1, var2;

    var1 = ((((adc_T >> 3) - ((int32_t)s_cal.dig_T1 << 1))) * ((int32_t)s_cal.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)s_cal.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)s_cal.dig_T1))) >> 12) *
            ((int32_t)s_cal.dig_T3)) >> 14;

    t_fine = var1 + var2;               /* ★ 气压/湿度补偿都要用它 */

    return (t_fine * 5 + 128) >> 8;
}

/* 气压补偿：返回 Q24.8 定点（/256 = Pa，如 24674867 → 96386.2 Pa）
 *
 * ★★ 中间量必须用 64 位（int64_t）★★
 * Bosch 官方的气压补偿有两个版本：
 *   · 32 位版：全用 int32，靠移位控制中间量。精度低，且在部分校准系数上会
 *     明显跑偏（本项目实测过：32 位版输出 1164 hPa，真值 1011 hPa，偏 +15%）
 *   · 64 位版：中间量最长到 2^47 量级，必须用 int64 才不会溢出，精度高
 * 根因就是 `p << 31` 这一步：在 int32 下直接溢出成负数，结果完全不可信。 */
static uint32_t compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)s_cal.dig_P6;
    var2 = var2 + ((var1 * (int64_t)s_cal.dig_P5) << 17);
    var2 = var2 + (((int64_t)s_cal.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_cal.dig_P3) >> 8) + ((var1 * (int64_t)s_cal.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)s_cal.dig_P1)) >> 33;

    if (var1 == 0) { return 0U; }                       /* 防止除零 */

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;             /* ★ 此行必须 64 位 */

    var1 = (((int64_t)s_cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_cal.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)s_cal.dig_P7) << 4);

    return (uint32_t)p;                                 /* Q24.8：/256 = Pa */
}

/* 湿度补偿：返回值 /1024 = %RH（47445 → 46.33 %RH），前提是 t_fine 已算好 */
static uint32_t compensate_humidity(int32_t adc_H)
{
    int32_t v;

    v = (t_fine - ((int32_t)76800));
    v = (((((adc_H << 14) - (((int32_t)s_cal.dig_H4) << 20) - (((int32_t)s_cal.dig_H5) * v))
            + ((int32_t)16384)) >> 15)
         * (((((((v * ((int32_t)s_cal.dig_H6)) >> 10)
                * (((v * ((int32_t)s_cal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10)
              + ((int32_t)2097152)) * ((int32_t)s_cal.dig_H2) + 8192) >> 14));

    v = (v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)s_cal.dig_H1)) >> 4));

    /* 限幅：湿度不可能小于 0 或大于 100%（100% 对应 419430400）*/
    if (v < 0)         { v = 0; }
    if (v > 419430400) { v = 419430400; }

    return (uint32_t)(v >> 12);
}

/* ============================================================
 * 读数合理性校验（物理量闸门）
 *
 * 【它要解决的问题】
 *   长跑实测（连续 8.35 小时 / 30062 条）里出现过 2 条孤立尖峰：
 *   温度 +15.6 ℃、气压 +25.2 hPa，下一条立刻恢复。
 *   两次偏移量几乎完全相同（+15.62/+15.61 ℃，+25.20/+25.20 hPa）——
 *   随机噪声不可能重复出同一幅度，说明是【确定性的读取污染】。
 *
 *   用实测校准系数反推：单一 t_fine 偏移 Δtf≈79975 恰好同时产生
 *   +15.62 ℃ 与 +25.21 hPa，与观测量吻合 ⇒ 温度 ADC 被污染，
 *   气压补偿依赖 t_fine，于是被同步带偏。
 *
 * 【为什么 CRC 抓不到它】
 *   CRC 保护的是"打包 → 存储 → 传输"这条链路，它只能证明
 *   "存进去的字节 = 读回来的字节"，【不能证明"从传感器读到的字节是对的"】。
 *   这是两个完全不同的断言 —— 所以必须在驱动层再加一道闸门。
 *
 * 【判据】
 *   ① 绝对范围：超出芯片规格（-40 ~ +85 ℃）判无效
 *   ② 变化率  ：1 Hz 采样下 BME280 热惯性很大，相邻两次温差不应超过 2.00 ℃
 *   两次读数都不可信 → 返回错误码，由上层跳过该条，
 *   【绝不把已知错误的数据写进 Flash】。
 * ============================================================ */
#define VALID_T_ABS_MIN   (-4000)   /* -40.00 ℃ */
#define VALID_T_ABS_MAX   ( 8500)   /* +85.00 ℃ */
#define VALID_T_MAX_STEP  (  200)   /* 相邻两次最大允许变化 2.00 ℃ */

static int32_t  s_last_temp  = 0;
static uint8_t  s_have_last  = 0U;
static uint32_t s_reject_cnt = 0U;

static uint8_t temp_is_plausible(int32_t t)
{
    int32_t diff;

    if ((t < VALID_T_ABS_MIN) || (t > VALID_T_ABS_MAX)) { return 0U; }

    if (s_have_last != 0U)
    {
        diff = t - s_last_temp;
        if (diff < 0) { diff = -diff; }
        if (diff > VALID_T_MAX_STEP) { return 0U; }
    }

    return 1U;
}

/* ============================================================
 * 等待"不在转换中"
 *
 * 【这是对上面那个污染问题的根因修复】
 *
 * 上面的物理量闸门是"事后拦截"——拦得住，但那条数据还是丢了。
 * 真正的根因在另一处：
 *   本芯片配的是 Normal 连续模式（待机 1 s），而本机也恰好 1 s 轮询一次。
 *   两颗时钟各自独立，8 字节突发读（100 kHz 下约 0.7 ms）有可能
 *   【跨过从机内部的寄存器更新时刻】，读到"半新半旧"的一组数据。
 *   BME280 数据手册明确写有"数据寄存器不是同时更新的"。
 *
 * 修法：读之前先看一眼状态寄存器的 measuring 位（0xF3 bit3）。
 *   它是 1 = 正在转换 → 此刻寄存器正在被改写，这一轮先别读。
 *   它是 0 = 转换已完成且稳定 → 这 8 字节在读的过程中不会被改写。
 *
 * 代价几乎为零（一次单字节 I2C 读），收益是从"事后拦截"变成"事前避免"。
 * 于是物理量闸门退化成第二道防线，仍然保留 ——
 * 纵深防御：能预判就预判，预判不到还有兜底。
 *
 * 【备选做法（更彻底，留待后续）】
 *   改用 Forced 模式：每次显式触发一次转换 → 等 measuring 落回 0 → 再读。
 *   那样读取窗口必然落在更新时刻之外。但 Normal 模式已经跑通并有实测数据，
 *   本版选择"保留已验证的配置 + 补一个前置检查"，改动面最小。
 * ============================================================ */
static uint8_t wait_not_measuring(void)
{
    uint8_t st;
    uint8_t tries;

    for (tries = 0U; tries < 30U; tries++)
    {
        if (i2c_read_reg(BME280_ADDR, REG_STATUS, &st, 1U) != 0U) { return 1U; }
        if ((st & STATUS_MEASURING) == 0U) { return 0U; }
        delay_us(2000U);      /* 2 ms 后重看，最多等 60 ms */
    }

    return 2U;
}

/* ============================================================
 * 读校准系数：两段共 33 字节
 * ============================================================ */
static uint8_t read_calibration(void)
{
    uint8_t b[26];
    uint8_t e[7];

    /* --- 第 1 段：0x88 起 26 字节 --- */
    if (i2c_read_reg(BME280_ADDR, REG_CALIB00, b, 26U) != 0U) { return 1U; }

    s_cal.dig_T1 = (uint16_t)(((uint16_t)b[1] << 8) | b[0]);   /* 无符号 */
    s_cal.dig_T2 = (int16_t)(((uint16_t)b[3] << 8) | b[2]);    /* 有符号 */
    s_cal.dig_T3 = (int16_t)(((uint16_t)b[5] << 8) | b[4]);

    s_cal.dig_P1 = (uint16_t)(((uint16_t)b[7] << 8) | b[6]);
    s_cal.dig_P2 = (int16_t)(((uint16_t)b[9] << 8) | b[8]);
    s_cal.dig_P3 = (int16_t)(((uint16_t)b[11] << 8) | b[10]);
    s_cal.dig_P4 = (int16_t)(((uint16_t)b[13] << 8) | b[12]);
    s_cal.dig_P5 = (int16_t)(((uint16_t)b[15] << 8) | b[14]);
    s_cal.dig_P6 = (int16_t)(((uint16_t)b[17] << 8) | b[16]);
    s_cal.dig_P7 = (int16_t)(((uint16_t)b[19] << 8) | b[18]);
    s_cal.dig_P8 = (int16_t)(((uint16_t)b[21] << 8) | b[20]);
    s_cal.dig_P9 = (int16_t)(((uint16_t)b[23] << 8) | b[22]);

    /* b[24]（0xA0）是保留字节，跳过；dig_H1 是单字节，在 0xA1 */
    s_cal.dig_H1 = b[25];

    /* --- 第 2 段：0xE1 起 7 字节 --- */
    if (i2c_read_reg(BME280_ADDR, REG_CALIB26, e, 7U) != 0U) { return 2U; }

    s_cal.dig_H2 = (int16_t)(((uint16_t)e[1] << 8) | e[0]);
    s_cal.dig_H3 = e[2];

    /* ★ dig_H4 / dig_H5 是 12 位，被拆成"高 8 位 + 低 4 位"跨字节存放，
     *   这是 BME280 驱动里最容易写错的地方（照抄 Bosch 官方写法）：
     *     H4 = 0xE4 全部 8 位作高 8 位，0xE5 的【低】4 位作低 4 位
     *     H5 = 0xE6 全部 8 位作高 8 位，0xE5 的【高】4 位作低 4 位
     *   注意 0xE5 这一个字节被 H4 和 H5 瓜分了！ */
    s_cal.dig_H4 = (int16_t)((int16_t)((int16_t)e[3] << 4) | (e[4] & 0x0FU));
    s_cal.dig_H5 = (int16_t)((int16_t)((int16_t)e[5] << 4) | (e[4] >> 4));

    s_cal.dig_H6 = (int8_t)e[6];

    return 0U;
}

/* ============================================================
 * 读一次数据区 8 字节，拆出三个原始 ADC 值
 * ============================================================ */
static uint8_t read_raw_once(int32_t *adc_P, int32_t *adc_T, int32_t *adc_H)
{
    uint8_t d[8];

    /* ★ 一次连读 8 字节（0xF7~0xFE）：气压 3 + 温度 3 + 湿度 2。
     *   数据手册要求"同一批测量"必须整段读出，分三次读可能跨两次转换，
     *   造成温湿压数据不同步。 */
    if (i2c_read_reg(BME280_ADDR, REG_DATA_START, d, 8U) != 0U) { return 1U; }

    /* BME280 的数据是 20 位，分 3 字节存放：MSB(8) + LSB(8) + XLSB 高 4 位
     * 组装：raw = (msb << 12) | (lsb << 4) | (xlsb >> 4) */
    *adc_P = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4));
    *adc_T = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4));

    /* 湿度是 16 位，只有 2 字节 */
    *adc_H = (int32_t)(((uint32_t)d[6] << 8) | (uint32_t)d[7]);

    return 0U;
}

/* ============================================================
 * 对外接口
 * ============================================================ */
uint8_t bme280_init(void)
{
    uint8_t v;

    /* 1. 校验芯片 ID：0x60 = BME280（有湿度）；0x58 = BMP280（无湿度）*/
    if (i2c_read_reg(BME280_ADDR, REG_CHIP_ID, &s_chip_id, 1U) != 0U) { return 10U; }
    if (s_chip_id != 0x60U)                                           { return 11U; }

    /* 2. 读 33 个校准系数 */
    {
        uint8_t rc = read_calibration();
        if (rc != 0U) { return (uint8_t)(20U + rc); }
    }

    /* 3. 配置湿度过采样。
     *    ★ 数据手册规定：ctrl_hum 必须在 ctrl_meas 之前写，否则不生效（经典坑）*/
    v = 0x01U;   /* osrs_h = 001 → 湿度 ×1 */
    if (i2c_write_reg(BME280_ADDR, REG_CTRL_HUM, &v, 1U) != 0U) { return 30U; }

    /* 4. config：t_sb=101（待机 1000 ms）| filter=010（IIR ×4）| spi3w=0
     *    → 0b101_010_00 = 0xA8 */
    v = 0xA8U;
    if (i2c_write_reg(BME280_ADDR, REG_CONFIG, &v, 1U) != 0U) { return 31U; }

    /* 5. ctrl_meas：osrs_t=010（温度 ×2）| osrs_p=101（气压 ×16）| mode=11（Normal）
     *    → 0b010_101_11 = 0x57
     *    气压过采样给高是因为气压对噪声最敏感，需要更多平均 */
    v = 0x57U;
    if (i2c_write_reg(BME280_ADDR, REG_CTRL_MEAS, &v, 1U) != 0U) { return 32U; }

    return 0U;
}

uint8_t bme280_get_chip_id(void) { return s_chip_id; }

void bme280_get_calib(bme280_calib_t *out)
{
    if (out != 0) { *out = s_cal; }
}

void bme280_get_raw(bme280_raw_t *out)
{
    if (out != 0) { *out = s_raw; }
}

uint32_t bme280_get_reject_count(void) { return s_reject_cnt; }

uint8_t bme280_read(bme280_data_t *out)
{
    int32_t adc_P, adc_T, adc_H;
    int32_t T;
    uint8_t attempt;

    if (out == 0) { return 1U; }

    /* 最多读两次：首次读数被判为不可信时立刻重读一次。
     * 重读与首次读相隔不到 1 ms，落在同一次转换结果内，
     * 既不会引入新误差，又能借从机重发拿到干净数据。 */
    for (attempt = 0U; attempt < 2U; attempt++)
    {
        /* 前置：先确认从机不在转换中，避免读到"半新半旧"的寄存器 */
        if (wait_not_measuring() != 0U) { return 1U; }

        if (read_raw_once(&adc_P, &adc_T, &adc_H) != 0U) { return 1U; }

        /* 顺序不能反：温度补偿会算出 t_fine，气压/湿度都依赖它 */
        T = compensate_temperature(adc_T);

        if (temp_is_plausible(T) != 0U)
        {
            /* 通过校验：记录原始量（供 PC 复算与诊断）并更新历史 */
            s_raw.adc_T = adc_T;
            s_raw.adc_P = adc_P;
            s_raw.adc_H = adc_H;
            s_raw.t_fine = t_fine;

            s_last_temp = T;
            s_have_last = 1U;

            out->temperature = T;                          /* 0.01 ℃ */

            /* 气压：64 位版返回 Q24.8（值/256 = Pa），右移 8 位得到 Pa */
            s_raw.comp_press_q24_8 = compensate_pressure(adc_P);
            out->pressure = s_raw.comp_press_q24_8 >> 8;    /* Pa */

            /* 湿度：Bosch 返回 Q22.10（值/1024 = %RH）。
             * 我们要"0.01 %RH"单位 → ×100 再右移 10 位（即 ×100/1024）*/
            s_raw.comp_hum_q22_10 = compensate_humidity(adc_H);
            out->humidity = (s_raw.comp_hum_q22_10 * 100U) >> 10;   /* 0.01 %RH */

            return 0U;
        }

        /* 未通过校验 → 循环再读一次 */
    }

    /* 连续两次都不可信：绝不把已知错误的数据交给上层 */
    s_reject_cnt++;
    return 2U;
}
