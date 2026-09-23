/* ============================================================
 * self_test.c —— W1/W2/W4 验收自检实现
 *
 * 【设计原则】
 *   ① 每一项都打印机器可读的行（以 # 开头）—— PC 端脚本要解析，
 *      人也要能直接看懂，所以格式固定成 "#TAG,key=value,..."。
 *   ② 自检要往 Flash 写数据，所以【只写保留的测试区】，
 *      绝不碰记录区（前 4 MiB 是真实数据）。
 *   ③ 总线操作取锁、干完立刻放锁，且【不在持锁期间做串口打印】——
 *      自检代码跑在最低优先级的通信任务里，长时间占锁会把采集任务饿死。
 *      所以统一用"锁内只搬数据到局部缓冲，锁外再打印"的两段式。
 * ============================================================ */
#include "self_test.h"
#include "app_tasks.h"
#include "app_config.h"
#include "board.h"
#include "log.h"
#include "bme280.h"
#include "recorder.h"
#include "crc16.h"
#include "spi_soft.h"
#include "w25q64.h"
#include "i2c_soft.h"

/* ============================================================
 * 自检专用暂存区
 *
 * W25Q64 共 8 MiB：
 *   0x000000 – 0x3FFFFF  记录区（4 MiB，真实数据，自检绝不触碰）
 *   0x7FE000 – 0x7FFFFF  自检暂存区（最后 8 KiB = 2 个扇区 = 256 个槽位）
 * 放在芯片最尾部，是为了让"自检"和"记录"在物理地址上彻底分开，
 * 这样自检怎么写都不可能破坏日志。 */
#define SELFTEST_SCRATCH_ADDR   (W25Q64_TOTAL_SIZE - (2UL * W25Q64_SECTOR_SIZE))
#define SELFTEST_SCRATCH_SLOTS  ((2UL * W25Q64_SECTOR_SIZE) / REC_SIZE)   /* 256 */

#define SELFTEST_LOCK_TMO_MS    200U

/* ---------- 打印小工具 ---------- */
#define P(s)    log_str(s)
#define PU(v)   log_u32(v)
#define PI(v)   log_i32(v)
#define PNL()   log_nl()

/* ============================================================
 * 暂存区读写辅助
 * ============================================================ */

/* 完整写一条记录（两步写，与 recorder_append 的行为一致）
 * 返回 0 = 成功，1 = 第一步失败，2 = 第二步失败 */
static uint8_t scratch_write_full(uint32_t addr, const rec_data_t *d)
{
    uint8_t buf[REC_SIZE];

    recorder_pack(d, buf);

    if (w25q64_page_program(addr, buf, REC_TAIL_CRC_OFF) != 0U) { return 1U; }
    if (w25q64_page_program(addr + REC_TAIL_CRC_OFF,
                            &buf[REC_TAIL_CRC_OFF], 2U) != 0U) { return 2U; }
    return 0U;
}

/* 只写数据区 + 头 CRC，【不写尾 CRC】—— 模拟"数据写完但记录未收尾"的断电 */
static uint8_t scratch_write_no_tail(uint32_t addr, const rec_data_t *d)
{
    uint8_t buf[REC_SIZE];

    recorder_pack(d, buf);
    return (uint8_t)((w25q64_page_program(addr, buf, REC_TAIL_CRC_OFF) != 0U) ? 1U : 0U);
}

/* 只写数据区的前 n 字节 —— 模拟"编程中途断电"的半写记录 */
static uint8_t scratch_write_partial(uint32_t addr, const rec_data_t *d, uint16_t n)
{
    uint8_t buf[REC_SIZE];

    recorder_pack(d, buf);
    return (uint8_t)((w25q64_page_program(addr, buf, n) != 0U) ? 1U : 0U);
}

/* 造一条完全确定的测试记录（PC 端可独立复现同样的值）*/
static void make_test_record(rec_data_t *d, uint32_t idx)
{
    d->seq         = idx;
    d->timestamp_s = 1000U + idx;
    d->temperature = (int32_t)(20000UL + (idx % 2000UL));    /* 20.000~22.000 ℃ */
    d->humidity    = 40000UL + (idx % 4000UL);               /* 40.000~44.000 %RH */
    d->pressure_pa = 101325UL + (idx % 500UL);               /* 101325~101824 Pa */
    d->adc1        = 0U;
    d->adc2        = 0U;
}

/* ============================================================
 * ① 四种 SPI 模式：回环自测 + 主从不匹配演示
 * ============================================================ */
void selftest_spi_four_modes(void)
{
    static const uint8_t TEST_BYTE = 0xA5U;   /* 1010_0101：位图不对称，移位一眼可见 */
    uint8_t m;
    uint8_t rx;
    uint8_t shift_seen = 0U;

    log_begin();
    P("\r\n---- [W4-1] 四种 SPI 模式回环自测 ----\r\n");
    P("#SPI4,需要一根杜邦线把 PA7(MOSI) 与 PA6(MISO) 短接\r\n");
    P("#SPI4,发送字节固定 0xA5（1010_0101，位图不对称，移位一眼可见）\r\n");
    log_end();

    if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U)
    {
        log_begin(); P("#SPI4,ERR,SPI 总线取锁超时\r\n"); log_end();
        return;
    }

    /* 回环测试不需要片选，把它拉高避免 W25Q64 误响应 */
    spi_cs_high();

    for (m = 0U; m < 4U; m++)
    {
        spi_soft_set_mode(m);
        rx = spi_soft_transfer(TEST_BYTE);

        log_begin();
        P("#SPI4,Mode");
        PU((uint32_t)m);
        P(",CPOL=");
        PU((uint32_t)((m >> 1) & 1U));
        P(",CPHA=");
        PU((uint32_t)(m & 1U));
        P(",SCLK_idle=");
        P(((m >> 1) & 1U) ? "HIGH" : "LOW");
        P(",tx=0x");
        log_hex8(TEST_BYTE);
        P(",rx=0x");
        log_hex8(rx);
        P(",echo=");
        P((rx == TEST_BYTE) ? "MATCH" : "DIFF");
        PNL();
        log_end();
    }

    /* ---- 主从模式不匹配演示 ----
     * 发送方按 Mode1（CPHA=1：前沿才换数据），
     * 接收方按 Mode0（CPHA=0：前沿就采样）。
     * 接收方的采样点正好压在数据跳变的那一刻 —— 建立时间不满足，
     * 于是读到相邻位，整体错位半拍。
     * 具体读回什么值取决于进入本次传输前 MOSI 的电平（末位残留），
     * 所以这里只断言"不等于原值"，并把实际值打出来让你自己看。 */
    spi_soft_set_mode(SPI_MODE0);
    rx = spi_soft_transfer_ex(TEST_BYTE, SPI_MODE1, SPI_MODE0);
    shift_seen = (uint8_t)((rx != TEST_BYTE) ? 1U : 0U);

    spi_soft_set_mode(SPI_MODE0);
    app_unlock_spi();

    log_begin();
    P("#SPI4,MISMATCH,tx_mode=1(CPHA=1),rx_mode=0(CPHA=0),tx=0x");
    log_hex8(TEST_BYTE);
    P(",rx=0x");
    log_hex8(rx);
    P(",shifted=");
    P(shift_seen ? "YES" : "NO");
    PNL();
    P("#SPI4,注意: 四种模式回环【全部 MATCH】才是预期结果。\r\n");
    P("#SPI4,     因为回环时收发用同一个模式，两者永远自洽 ——\r\n");
    P("#SPI4,     它只能证明「位引擎正确」（移位方向、MISO 通路、边沿计数），\r\n");
    P("#SPI4,     证明不了「模式配对对不对」。那条要靠下一条测试\r\n");
    P("#SPI4,     （用真实从机 W25Q64 读 ID）和逻辑分析仪波形来证明。\r\n");
    log_end();
}

/* ============================================================
 * ② 四种模式分别读 W25Q64 的 JEDEC ID —— 模式配错的物理证据
 * ============================================================ */
void selftest_w25q64_id_all_modes(void)
{
    uint8_t m;
    uint8_t id[3];
    uint8_t ok_cnt = 0U;

    log_begin();
    P("\r\n---- [W4-2] 四种模式读 W25Q64 JEDEC ID ----\r\n");
    P("#SPIID,正确值应为 EF 40 17（Winbond / SPI NOR / 8 MB）\r\n");
    log_end();

    if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U)
    {
        log_begin(); P("#SPIID,ERR,SPI 总线取锁超时\r\n"); log_end();
        return;
    }

    for (m = 0U; m < 4U; m++)
    {
        spi_soft_set_mode(m);
        w25q64_read_id(id);

        log_begin();
        P("#SPIID,Mode");
        PU((uint32_t)m);
        P(",");
        log_hex8(id[0]); P(" ");
        log_hex8(id[1]); P(" ");
        log_hex8(id[2]);
        P(",");
        if (w25q64_is_id_ok(id) != 0U) { P("OK"); ok_cnt++; }
        else                           { P("WRONG"); }
        PNL();
        log_end();
    }

    spi_soft_set_mode(SPI_MODE0);
    app_unlock_spi();

    log_begin();
    P("#SPIID,读通模式数=");
    PU((uint32_t)ok_cnt);
    P("\r\n");
    if (ok_cnt == 2U)
    {
        P("#SPIID,结论: 符合预期 —— W25Q64 数据手册标注只支持 Mode0 与 Mode3。\r\n");
        P("#SPIID,为什么用 Mode0: 手册标注支持 Mode0/3，而 Mode0 下 SCLK 空闲为低，\r\n");
        P("#SPIID,与上电默认电平一致，不会在片选拉低瞬间产生多余时钟边沿误触发从机。\r\n");
        P("#SPIID,从机决定用哪个模式，主机必须跟随。\r\n");
    }
    else if (ok_cnt == 4U)
    {
        P("#SPIID,结论: 四种都读通 —— 说明连线没问题，但请核对 bit_delay 是否过大，\r\n");
        P("#SPIID,时序过于宽松时某些器件会「容忍」错误模式，掩盖真实问题。\r\n");
    }
    else if (ok_cnt == 0U)
    {
        P("#SPIID,结论: 一种都没读通 —— 先查接线（PA4=CS PA5=SCK PA6=DO PA7=DI）\r\n");
        P("#SPIID,与 3.3V 供电，再查 CS 是否在初始化后被正确拉高。\r\n");
    }
    else
    {
        P("#SPIID,结论: 部分读通 —— 记录下哪几种通了，对照手册的 Mode0/3 判断。\r\n");
    }
    log_end();
}

/* ============================================================
 * ③ 32 B 记录闭环：写 → 回读 → 逐条三重比对
 * ============================================================ */
uint8_t selftest_record_roundtrip(void)
{
    uint32_t i;
    uint32_t s;
    uint32_t bad     = 0U;
    uint32_t bad_idx = 0xFFFFFFFFUL;
    uint8_t  rc;
    uint8_t  buf[REC_SIZE];
    rec_data_t w, r;
    uint8_t  ok = 1U;

    log_begin();
    P("\r\n---- [W4-3] 32 B 记录闭环（写暂存区，不动记录区）----\r\n");
    P("#REC,槽位数=");
    PU(SELFTEST_SCRATCH_SLOTS);
    P("，跨 1 个扇区边界（第 128 槽）与 32 个页边界\r\n");
    log_end();

    /* ---- 擦掉 2 个暂存扇区 ---- */
    for (s = 0U; s < 2U; s++)
    {
        if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U) { return 1U; }
        rc = w25q64_sector_erase(SELFTEST_SCRATCH_ADDR + (s * W25Q64_SECTOR_SIZE));
        app_unlock_spi();

        if (rc != 0U)
        {
            log_begin(); P("#REC,ERR,扇区擦除失败,sector="); PU(s); PNL(); log_end();
            return 1U;
        }
    }

    /* ---- 写 ---- */
    for (i = 0U; i < SELFTEST_SCRATCH_SLOTS; i++)
    {
        make_test_record(&w, i);

        if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U) { ok = 0U; break; }
        rc = scratch_write_full(SELFTEST_SCRATCH_ADDR + (i * REC_SIZE), &w);
        app_unlock_spi();

        if (rc != 0U)
        {
            log_begin();
            P("#REC,ERR,写失败,idx="); PU(i); P(",rc="); PU(rc); PNL();
            log_end();
            ok = 0U;
            break;
        }
    }

    if (ok == 0U) { return 1U; }

    /* ---- 读回 + 逐条三重比对 ----
     * ① 双 CRC 状态必须 OK
     * ② 序号 / 时间戳 必须一致
     * ③ 温湿压 必须一致
     *
     * 只比 CRC 不够：CRC 只证明"字节没变"，
     * 万一打包函数把字段放错了位置，CRC 依然全对。
     * 必须再比一次字段值，才算证明"写进去 = 读出来 = 原值"。 */
    for (i = 0U; i < SELFTEST_SCRATCH_SLOTS; i++)
    {
        if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U) { ok = 0U; break; }
        w25q64_read(SELFTEST_SCRATCH_ADDR + (i * REC_SIZE), buf, REC_SIZE);
        app_unlock_spi();

        make_test_record(&w, i);
        recorder_unpack(buf, &r);

        if ((recorder_slot_state(buf) != REC_SLOT_OK) ||
            (r.seq != w.seq) ||
            (r.timestamp_s != w.timestamp_s) ||
            (r.temperature != w.temperature) ||
            (r.humidity != w.humidity) ||
            (r.pressure_pa != w.pressure_pa))
        {
            bad++;
            if (bad_idx == 0xFFFFFFFFUL) { bad_idx = i; }
        }
    }

    log_begin();
    P("#REC,written=");
    PU(SELFTEST_SCRATCH_SLOTS);
    P(",mismatch=");
    PU(bad);
    if (bad != 0U) { P(",first_bad_idx="); PU(bad_idx); }
    P(",result=");
    P((bad == 0U) ? "PASS" : "FAIL");
    PNL();
    log_end();

    return (uint8_t)((bad == 0U) ? 0U : 1U);
}

/* ============================================================
 * ④ 双 CRC 设计的验证：断电三种状态能否被正确区分
 * ============================================================ */
uint8_t selftest_powerloss_cases(void)
{
    uint8_t    buf[REC_SIZE];
    uint8_t    st_ok = 0xFFU, st_unf = 0xFFU, st_half = 0xFFU;
    uint8_t    tail_hi = 0U, tail_lo = 0U;
    rec_data_t d;
    uint8_t    pass = 1U;
    uint8_t    rc   = 1U;      /* 默认失败，成功路径会置 0 */

    /* 三个槽位依次放在暂存区开头 */
    const uint32_t a_ok   = SELFTEST_SCRATCH_ADDR + (0UL * REC_SIZE);
    const uint32_t a_unf  = SELFTEST_SCRATCH_ADDR + (1UL * REC_SIZE);
    const uint32_t a_half = SELFTEST_SCRATCH_ADDR + (2UL * REC_SIZE);

    log_begin();
    P("\r\n---- [W4-4] 断电视角：双 CRC 能否区分三种状态 ----\r\n");
    log_end();

    /* ---- 锁内：擦 + 造三种状态 + 读回判定，全部只碰 Flash ---- */
    if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U) { return 1U; }

    do
    {
        if (w25q64_sector_erase(SELFTEST_SCRATCH_ADDR) != 0U) { rc = 1U; break; }

        /* ① 完整记录 */
        make_test_record(&d, 900UL);
        if (scratch_write_full(a_ok, &d) != 0U) { rc = 1U; break; }

        /* ② 数据写完但尾 CRC 未写（两次页编程之间掉电）*/
        make_test_record(&d, 901UL);
        if (scratch_write_no_tail(a_unf, &d) != 0U) { rc = 1U; break; }

        /* ③ 编程中途掉电：只写了数据区的前 12 字节 */
        make_test_record(&d, 902UL);
        if (scratch_write_partial(a_half, &d, 12U) != 0U) { rc = 1U; break; }

        w25q64_read(a_ok,   buf, REC_SIZE); st_ok   = recorder_slot_state(buf);
        w25q64_read(a_unf,  buf, REC_SIZE); st_unf  = recorder_slot_state(buf);
        tail_hi = buf[REC_TAIL_CRC_OFF + 1];
        tail_lo = buf[REC_TAIL_CRC_OFF];
        w25q64_read(a_half, buf, REC_SIZE); st_half = recorder_slot_state(buf);

        rc = 0U;
    } while (0);

    app_unlock_spi();

    if (rc != 0U)
    {
        log_begin(); P("#PWR,ERR,构造测试状态失败,rc="); PU((uint32_t)rc); PNL(); log_end();
        return 1U;
    }

    /* ---- 锁外打印 ---- */
    log_begin();
    P("#PWR,case1=完整记录,expect=OK(1),got=");       PU((uint32_t)st_ok);
    P(","); P((st_ok == REC_SLOT_OK) ? "PASS" : "FAIL"); PNL();

    P("#PWR,case2=数据完整但未收尾,expect=UNFINISHED(3),got="); PU((uint32_t)st_unf);
    P(","); P((st_unf == REC_SLOT_UNFINISHED) ? "PASS" : "FAIL"); PNL();

    P("#PWR,case3=编程中断电(半写),expect=HALF(2),got=");     PU((uint32_t)st_half);
    P(","); P((st_half == REC_SLOT_HALF) ? "PASS" : "FAIL"); PNL();

    P("#PWR,case2_tail_crc=0x"); log_hex8(tail_hi); log_hex8(tail_lo); PNL();

    if ((st_ok != REC_SLOT_OK) || (st_unf != REC_SLOT_UNFINISHED) || (st_half != REC_SLOT_HALF))
    {
        pass = 0U;
    }

    P("#PWR,结论: case2 的头 CRC 是有效值、尾 CRC 是 0xFFFF，\r\n");
    P("#PWR,     说明数据区完好、只是记录没写完。\r\n");
    P("#PWR,     单 CRC 只能报「整条坏了」，会把这条好数据一起丢掉 ——\r\n");
    P("#PWR,     这就是 32 B 用头尾两个 CRC 的全部理由。\r\n");
    P("#PWR,result=");
    P(pass ? "PASS" : "FAIL");
    PNL();
    log_end();

    return (uint8_t)(pass ? 0U : 1U);
}

/* ============================================================
 * ⑤ 把已存记录按 HEX 吐给 PC
 * ============================================================ */
#define DUMP_BATCH   8U

void selftest_records_dump(void)
{
    static uint8_t batch[DUMP_BATCH][REC_SIZE];   /* 256 B 静态缓冲，避开栈压力 */
    uint32_t total = recorder_count();
    uint32_t base;
    uint32_t k;
    uint32_t n;

    log_begin();
    P("\r\n#DUMP,begin,total=");
    PU(total);
    P(",size=");
    PU(REC_SIZE);
    PNL();
    log_end();

    for (base = 0U; base < total; base += DUMP_BATCH)
    {
        /* ---- 锁内：只做 Flash 读，把字节搬进 batch ---- */
        if (app_lock_spi(SELFTEST_LOCK_TMO_MS) == 0U)
        {
            log_begin(); P("#DUMP,abort,lock timeout at "); PU(base); PNL(); log_end();
            return;
        }

        n = 0U;
        for (k = 0U; (k < DUMP_BATCH) && ((base + k) < total); k++)
        {
            recorder_read(base + k, batch[k]);
            n++;
        }

        app_unlock_spi();

        /* ---- 锁外：打印 ---- */
        log_begin();
        for (k = 0U; k < n; k++)
        {
            P("REC,");
            PU(base + k);
            P(",");
            log_hex_buf(batch[k], REC_SIZE);
            PNL();
        }
        log_end();
    }

    log_begin();
    P("#DUMP,end,total=");
    PU(total);
    PNL();
    log_end();
}

/* ============================================================
 * ⑥ 全量回读校验已存记录
 * ============================================================ */
void selftest_records_verify(void)
{
    rec_stat_t st;
    uint32_t   bad;

    log_begin();
    P("\r\n---- [W4-5] 记录区全量回读校验 ----\r\n");
    log_end();

    if (app_lock_spi(3000U) == 0U)
    {
        log_begin(); P("#VERIFY,ERR,SPI 总线取锁超时\r\n"); log_end();
        return;
    }

    bad = recorder_verify(&st);
    app_unlock_spi();

    log_begin();
    P("#VERIFY,slots=");
    PU(st.total_slots);
    P(",ok=");
    PU(st.ok);
    P(",half=");
    PU(st.half);
    P(",unfinished=");
    PU(st.unfinished);
    P(",bad=");
    PU(bad);
    if (st.first_bad_idx != 0xFFFFFFFFUL)
    {
        P(",first_bad_idx=");
        PU(st.first_bad_idx);
    }
    P(",result=");
    P((bad == 0U) ? "PASS" : "FAIL");
    PNL();
    log_end();
}

/* ============================================================
 * ⑦ BME280：33 个校准系数（供 PC 独立复算）
 * ============================================================ */
void selftest_bme280_calib_dump(void)
{
    bme280_calib_t c;

    if (app_lock_i2c(SELFTEST_LOCK_TMO_MS) == 0U)
    {
        log_begin(); P("#CALIB,ERR,lock timeout\r\n"); log_end();
        return;
    }
    bme280_get_calib(&c);
    app_unlock_i2c();

    log_begin();
    P("#CALIB,T1="); PU(c.dig_T1);
    P(",T2=");      PI(c.dig_T2);
    P(",T3=");      PI(c.dig_T3);
    P(",P1=");      PU(c.dig_P1);
    P(",P2=");      PI(c.dig_P2);
    P(",P3=");      PI(c.dig_P3);
    P(",P4=");      PI(c.dig_P4);
    P(",P5=");      PI(c.dig_P5);
    P(",P6=");      PI(c.dig_P6);
    P(",P7=");      PI(c.dig_P7);
    P(",P8=");      PI(c.dig_P8);
    P(",P9=");      PI(c.dig_P9);
    P(",H1=");      PU(c.dig_H1);
    P(",H2=");      PI(c.dig_H2);
    P(",H3=");      PU(c.dig_H3);
    P(",H4=");      PI(c.dig_H4);
    P(",H5=");      PI(c.dig_H5);
    P(",H6=");      PI(c.dig_H6);
    P(",count=33");
    PNL();
    log_end();
}

/* ============================================================
 * ⑧ BME280：一次完整测量，把"输入 → 中间量 → 输出"全打出来
 *
 * 这是"PC 端复算逐位一致"那句话的证据源：
 *   PC 拿 #CALIB 的 33 个系数 + #RAW 的三个 adc 值，
 *   用同一套 Bosch 整数公式独立算一遍，
 *   结果必须与 #CMP / #OUT 完全相同（不是"接近"，是逐位相同）。
 * ============================================================ */
void selftest_bme280_measure_dump(void)
{
    bme280_data_t out;
    bme280_raw_t  raw;
    uint8_t       rc;

    if (app_lock_i2c(500U) == 0U)
    {
        log_begin(); P("#RAW,ERR,lock timeout\r\n"); log_end();
        return;
    }

    rc = bme280_read(&out);
    if (rc == 0U) { bme280_get_raw(&raw); }

    app_unlock_i2c();

    if (rc != 0U)
    {
        log_begin();
        P("#RAW,ERR,rc=");
        PU((uint32_t)rc);
        PNL();
        log_end();
        return;
    }

    log_begin();
    P("#RAW,adc_T=");   PI(raw.adc_T);
    P(",adc_P=");       PI(raw.adc_P);
    P(",adc_H=");       PI(raw.adc_H);
    P(",t_fine=");      PI(raw.t_fine);
    PNL();

    P("#CMP,press_q24_8="); PU(raw.comp_press_q24_8);
    P(",hum_q22_10=");      PU(raw.comp_hum_q22_10);
    PNL();

    P("#OUT,temp_centi=");  PI(out.temperature);
    P(",humi_centi=");      PU(out.humidity);
    P(",press_pa=");        PU(out.pressure);
    PNL();
    log_end();
}

/* ============================================================
 * ⑨ 一次跑完全部
 * ============================================================ */
void selftest_run_all(void)
{
    log_begin();
    P("\r\n======== W4 自检开始 ========\r\n");
    log_end();

    selftest_spi_four_modes();
    selftest_w25q64_id_all_modes();

    (void)selftest_record_roundtrip();
    (void)selftest_powerloss_cases();

    selftest_bme280_calib_dump();
    selftest_bme280_measure_dump();
    /* 注意：这里【故意不调用 selftest_records_dump()】。
     * 记录区可能有几万条，一次全倒出来要好几分钟并刷屏，
     * 需要时单独发命令 r —— 那个才是给 PC 脚本抓取用的。 */

    log_begin();
    P("======== W4 自检结束 ========\r\n");
    log_end();
}
