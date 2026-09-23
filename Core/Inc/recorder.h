/* ============================================================
 * recorder.h —— 32 字节定长记录格式 + W25Q64 顺序存储
 *
 * ============================================================
 *  记录格式（32 B = 28 B 数据区 + 2 B 头 CRC + 2 B 尾 CRC）
 * ============================================================
 *   偏移    长度  字段         类型 / 换算
 *   -----  ----  -----------  ------------------------------------
 *   0–3     4    记录序号      uint32 小端
 *   4–7     4    时间戳(秒)    uint32 小端
 *   8–11    4    温度          int32  小端，℃ × 1000
 *   12–15   4    湿度          uint32 小端，%RH × 1000
 *   16–19   4    气压          uint32 小端，单位 Pa
 *   20–23   4    ADC 通道 1    uint32（预留，11 月模块 3 填；本阶段固定 0）
 *   24–27   4    ADC 通道 2    uint32（预留）
 *   28–29   2    记录头 CRC    CRC-16/MODBUS      初值 0xFFFF，覆盖 0–27
 *   30–31   2    记录尾 CRC    CRC-16/CCITT-FALSE 初值 0xFFFF，覆盖 0–29
 *
 *   ★ 两个 CRC 必须用【不同多项式】，不能都用 MODBUS。
 *     原因见 crc16.h 的完整推导（这是 2026-09-17 实测 + 推导挖出来的）：
 *       CRC 对初值是仿射的，只要第二条 CRC 覆盖"数据 + 第一条 CRC"
 *       且用同一多项式，结果就恒为一个只由初值决定的常数，与数据无关。
 *     实测两次：MODBUS 从头 CRC 续算 → 恒 0x0000；
 *               MODBUS 换初值 0x0000 重算 0–29 → 恒 0x4FFE。
 *     ⇒ 换多项式才打破恒等式。现在头 MODBUS / 尾 CCITT，互为独立证据。
 *
 *   ⚠ 这是一次【破坏性格式变更】：旧记录（尾 CRC 为 0x0000）在新固件下
 *     会被判成"未收尾"，上电扫描会跳过它。
 *     刷本版固件后请先清空记录区（发两次 e），再重新开始长跑。
 *
 * 【为什么湿度/温度用 ×1000 而不是 ×100】
 *   BME280 的物理精度是 ±0.5 ℃ / ±3 %RH，×100（0.01 ℃）本来已经远超需要。
 *   但定长记录改长度是【破坏性变更】—— 已写入的历史数据会全部失效，
 *   而且 32 B 这种紧凑空间里挤不出"版本/长度"字段做兼容。
 *   所以一次定到毕设结束：留 ×1000 的余量，为将来可能换更高精度传感器留路。
 *   气压用 Pa（= 0.01 hPa）而不是 0.1 hPa 是同一个理由：
 *   BME280 气压相对精度 0.12 hPa，若量化台阶是 0.1 hPa 就一点余量都没有。
 * ============================================================
 *
 * 【头尾双 CRC 的分工 —— 本格式最有价值的地方】
 *   头 CRC（28–29）回答「数据区有没有被改坏」
 *   尾 CRC（30–31）把"数据区 + 头 CRC"再算一次，回答「这条记录有没有写完」
 *
 *   两者配合，能把断电时刻精确定位到三种情况：
 *     · 编程中断电       → 数据区末尾仍是 0xFF，头 CRC 失败
 *                          → 判为「半写记录」，丢弃
 *     · 数据写完但尾 CRC 未写完 → 头 CRC 通过、尾 CRC 是 0xFF
 *                          → 判为「数据完整但记录未收尾」，
 *                            能精确识别，而【不是误判成数据有错】
 *     · 全部写完         → 两个 CRC 都通过 → 有效记录
 *
 *   ★ 单 CRC 做不到这个区分。这是毕设"理论分析与计算"的得分点。
 *
 * 【为了让"尾 CRC 缺失"真的可复现，必须分两步写】
 *   如果一次页编程写满 32 字节，掉电时要么全写成、要么全没写，
 *   中间那种"数据完整但没尾 CRC"的状态几乎不会出现，双 CRC 的设计就落空了。
 *   所以 recorder_append() 分两次页编程：
 *       第一次写偏移 0–29（数据 + 头 CRC）
 *       第二次写偏移 30–31（尾 CRC）
 *   两次之间掉电 → 正好复现"记录未收尾"，设计才是有意义的。
 * ============================================================ */
#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include "w25q64.h"

#define REC_SIZE            32U     /* 单条记录字节数 */
#define REC_DATA_LEN        28U     /* 数据区长度（参与头 CRC）*/
#define REC_HEAD_CRC_OFF    28U     /* 头 CRC 偏移 */
#define REC_TAIL_CRC_OFF    30U     /* 尾 CRC 偏移 */

/* 记录区大小：取 W25Q64 的前 4 MiB（二进制 4 MiB = 4,194,304 B）
 *
 * ★ 与执行计划文档的口径差异（写论文时统一一下）：
 *   计划文档按十进制 4 MB = 4,000,000 B 算，得到 125 000 条 ≈ 1.45 天。
 *   本实现按二进制 4 MiB  = 4,194,304 B 算，得到 131 072 条 ≈ 1.52 天。
 *   差 4.8%，原因是"MB"这个词的两种含义。此处按真实存储器边界取值，
 *   因为 4 MiB 正好是 1024 个 4 KB 扇区，边界干净、擦除逻辑好写。 */
#define RECORD_AREA_BYTES   (4UL * 1024UL * 1024UL)

#define REC_SLOT_MAX        (RECORD_AREA_BYTES / REC_SIZE)          /* 131 072 条 */
#define REC_PER_PAGE        (W25Q64_PAGE_SIZE / REC_SIZE)           /* 每页 8 条 */
#define REC_PER_SECTOR      (W25Q64_SECTOR_SIZE / REC_SIZE)         /* 每扇区 128 条 */

/* 是否分两步写（详见 recorder.c 里 recorder_append 的说明）
 *   1 = 先写 0–29，再写 30–31（推荐；让"尾 CRC 缺失"这种断电状态可复现）
 *   0 = 一次写满 32 字节（写次数减半，但双 CRC 的第二种断电情况几乎采不到）
 * 想对比两种写法对双 CRC 判据的影响时，把它改成 0 重新编译即可。 */
#ifndef REC_TWO_STEP_WRITE
#define REC_TWO_STEP_WRITE  1
#endif

/* ---------- 槽位状态（扫描时逐条判定）---------- */
#define REC_SLOT_EMPTY      0U   /* 全 0xFF，从未写过 */
#define REC_SLOT_OK         1U   /* 头尾 CRC 都通过 → 有效记录 */
#define REC_SLOT_HALF       2U   /* 头 CRC 失败 → 编程中断电，半写记录 */
#define REC_SLOT_UNFINISHED 3U   /* 头 CRC 过、尾 CRC 失败 → 数据完整但未收尾 */

/* 一条记录的内容（还没打包成字节流）*/
typedef struct
{
    uint32_t seq;           /* 序号 */
    uint32_t timestamp_s;   /* 时间戳（秒）*/
    int32_t  temperature;   /* ℃ × 1000 */
    uint32_t humidity;      /* %RH × 1000 */
    uint32_t pressure_pa;   /* Pa */
    uint32_t adc1;          /* 预留 */
    uint32_t adc2;          /* 预留 */
} rec_data_t;

/* 校验/统计结果 */
typedef struct
{
    uint32_t total_slots;    /* 扫描过的槽位总数 */
    uint32_t ok;             /* 有效记录 */
    uint32_t half;           /* 半写记录 */
    uint32_t unfinished;     /* 未收尾记录 */
    uint32_t first_bad_idx;  /* 第一个异常槽位下标（没有则 = 0xFFFFFFFF）*/
} rec_stat_t;

/* 把一条记录打包成 32 字节（含头尾双 CRC），buf 至少 32 字节 */
void recorder_pack(const rec_data_t *d, uint8_t buf[REC_SIZE]);

/* 解包（回读校验/上位机对照用）。buf 必须是 32 字节原始流 */
void recorder_unpack(const uint8_t buf[REC_SIZE], rec_data_t *d);

/* 判定单个槽位的状态，返回 REC_SLOT_xxx */
uint8_t recorder_slot_state(const uint8_t buf[REC_SIZE]);

/* 上电扫描：找到已存条数并定位写指针。
 *
 * 【断电后写指针怎么定 —— 本函数的全部意义】
 *   逐槽位看状态，遇到第一个"不是有效记录"的槽位就停：
 *     · 该槽位是 EMPTY  → 写指针 = 这个槽位（它还是 0xFF，可以直接编程）
 *     · 该槽位是 HALF / UNFINISHED
 *                        → 写指针 = 【下一个】槽位
 *                          为什么是下一个：NOR Flash 位只能 1→0，
 *                          这个槽位里已经有残数据了，不擦扇区就写不回去。
 *                          直接跳过它，只损失 32 字节，不损失其它记录 ——
 *                          这是顺序追加型日志最划算的取舍。
 *   返回已存的有效记录条数 */
uint32_t recorder_scan(void);

/* 当前已存记录条数（RAM 镜像）*/
uint32_t recorder_count(void);

/* 追加一条记录（内部自动处理"进入新扇区先擦除"）
 * 返回：0 = 成功；2 = 扇区擦除失败；3 = 数据段编程失败；
 *       4 = 容量已满；5 = 尾 CRC 编程失败 */
uint8_t recorder_append(const rec_data_t *d);

/* 读回第 index 条记录的原始 32 字节 */
void recorder_read(uint32_t index, uint8_t buf[REC_SIZE]);

/* 回读全部已存记录并逐条重算【头、尾两个 CRC】，返回异常条数，stat 输出分类统计。
 *
 * ★ 为什么必须有这一步：
 *   UART 那行 HEX 是在 RAM 里打包后直接发出去的，
 *   它只能证明"打包正确 + 传输正确"，【证明不了"写进 Flash 的字节是对的"】。
 *   只有"读回来重新算 CRC"才能证明存储链路（页编程/扇区擦除/CS 时序）没出错。 */
uint32_t recorder_verify(rec_stat_t *stat);

/* 一次性清空：把"已经用到的扇区"全部擦回 0xFF，并把写指针归零。
 * 返回 0 = 全部擦成功；非 0 = 第几个扇区擦失败（从 1 开始计数）*/
uint32_t recorder_erase_all(void);

/* 记录区容量（条数）*/
uint32_t recorder_capacity(void);

#endif /* RECORDER_H */
