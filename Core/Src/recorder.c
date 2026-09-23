/* ============================================================
 * recorder.c —— 32 字节定长记录 + W25Q64 顺序存储
 * ============================================================ */
#include "recorder.h"
#include "crc16.h"

/* ---------- 定长编解码小工具 ----------
 * ★ 不使用 memcpy(&buf, d, sizeof(*d))！
 *   结构体会因为"内存对齐"插入填充字节，不同编译器/不同成员顺序
 *   填充位置都可能不同 —— 那样存进 Flash 的数据格式就不可控了。
 *   手工逐字节拆装虽然啰嗦，但【字节布局由你说了算】，
 *   这是通信/存储格式的行业惯例，也是 PC 端能独立解析的前提。 */
static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}

/* ---------- 文件级状态 ---------- */
static uint32_t s_wr_addr = 0U;   /* 下一个待写入的字节地址 */
static uint32_t s_count   = 0U;   /* 已占用槽位数（含被跳过的残破槽）*/
static uint8_t  s_buf[REC_SIZE];

/* ============================================================
 * 打包
 * ============================================================ */
void recorder_pack(const rec_data_t *d, uint8_t buf[REC_SIZE])
{
    uint16_t head_crc;

    put_u32_le(&buf[0],  d->seq);
    put_u32_le(&buf[4],  d->timestamp_s);
    put_u32_le(&buf[8],  (uint32_t)d->temperature);
    put_u32_le(&buf[12], d->humidity);
    put_u32_le(&buf[16], d->pressure_pa);
    put_u32_le(&buf[20], d->adc1);
    put_u32_le(&buf[24], d->adc2);

    /* 头 CRC：覆盖偏移 0–27（数据区），初值 0xFFFF */
    head_crc = crc16_modbus(buf, REC_DATA_LEN);
    put_u16_le(&buf[REC_HEAD_CRC_OFF], head_crc);

    /* 尾 CRC：覆盖偏移 0–29（数据区 + 头 CRC），但用【另一个多项式】
     *        CRC-16/CCITT-FALSE(0x1021)，而不是沿用 MODBUS。
     *
     * ★★ 必须换多项式的数学原因（详见 crc16.h 的完整推导）★★
     *   第一条实测：尾 CRC 从头 CRC 续算（MODBUS 同多项式）→ 1246 条记录里
     *     1245 条的尾 CRC 都是 0x0000（CRC 的"追加性质"）。
     *   第二条实测：改成 init=0x0000 重算后，尾 CRC 仍然恒定，只是换了个常数
     *     0x4FFE —— 说明光换初值没用。
     *   推导：CRC 对初值是仿射的，只要第二条 CRC 覆盖"数据 + 第一条 CRC"
     *     且用同一多项式，就有
     *         尾 = g_len(init1 ⊕ init2)   ← 与数据无关
     *     J=0xFFFF → 0x0000、J=0x0000 → 0x4FFE，两次实测都符合这个公式。
     *
     *   ⇒ 换掉多项式才能打破恒等式。现在两条 CRC：
     *       头：MODBUS(0–27)         覆盖数据区
     *       尾：CCITT-FALSE(0–29)    覆盖数据区 + 头 CRC
     *     范围不同、多项式不同，互为真正独立的证据。 */
    put_u16_le(&buf[REC_TAIL_CRC_OFF],
               crc16_ccitt(buf, REC_TAIL_CRC_OFF));
}

void recorder_unpack(const uint8_t buf[REC_SIZE], rec_data_t *d)
{
    d->seq          = get_u32_le(&buf[0]);
    d->timestamp_s  = get_u32_le(&buf[4]);
    d->temperature  = (int32_t)get_u32_le(&buf[8]);
    d->humidity     = get_u32_le(&buf[12]);
    d->pressure_pa  = get_u32_le(&buf[16]);
    d->adc1         = get_u32_le(&buf[20]);
    d->adc2         = get_u32_le(&buf[24]);
}

/* ============================================================
 * 判定单个槽位状态
 *
 * 判定顺序就是断电三种情况的甄别流程，顺序不能换：
 *   先判空 → 再判头 CRC → 最后判尾 CRC
 * ============================================================ */
uint8_t recorder_slot_state(const uint8_t buf[REC_SIZE])
{
    uint8_t  i;
    uint16_t head_calc, head_stored;
    uint16_t tail_calc, tail_stored;

    /* ① 整槽全 0xFF = 擦除后的状态 = 从未写过 */
    for (i = 0U; i < REC_SIZE; i++)
    {
        if (buf[i] != 0xFFU) { break; }
    }
    if (i == REC_SIZE) { return REC_SLOT_EMPTY; }

    /* ② 头 CRC：回答"数据区有没有被改坏" */
    head_calc   = crc16_modbus(buf, REC_DATA_LEN);
    head_stored = get_u16_le(&buf[REC_HEAD_CRC_OFF]);
    if (head_calc != head_stored) { return REC_SLOT_HALF; }

    /* ③ 尾 CRC：回答"这条记录有没有写完"。
     *    能走到这一步说明数据区是好的，尾 CRC 只可能因为
     *    "第二次页编程没执行"而失败 —— 这正是我们要区分出来的那种断电。
     *    ★ 必须用 crc16_ccitt（与 recorder_pack() 同一个多项式），
     *      否则所有记录都会被误判成"未收尾"。 */
    tail_calc   = crc16_ccitt(buf, REC_TAIL_CRC_OFF);
    tail_stored = get_u16_le(&buf[REC_TAIL_CRC_OFF]);
    if (tail_calc != tail_stored) { return REC_SLOT_UNFINISHED; }

    return REC_SLOT_OK;
}

/* ============================================================
 * 上电扫描：定位写指针
 *
 * 两级扫描，为了快：
 *   ① 页级粗扫：顺序写入保证"每页第 1 条记录"先于本页其它记录写入，
 *      所以只看每页第 1 条即可判断整页是否写过。逐页读 32 字节，
 *      比逐条读快 8 倍。
 *   ② 条级精扫：从上一页开始逐条找，最多 8 条。
 * ============================================================ */
uint32_t recorder_scan(void)
{
    uint32_t total_pages = RECORD_AREA_BYTES / W25Q64_PAGE_SIZE;
    uint32_t page;
    uint32_t idx;
    uint8_t  probe[REC_SIZE];
    uint8_t  state;

    /* ① 页级粗扫 */
    for (page = 0U; page < total_pages; page++)
    {
        w25q64_read(page * W25Q64_PAGE_SIZE, probe, REC_SIZE);
        if (recorder_slot_state(probe) == REC_SLOT_EMPTY)
        {
            break;      /* 这一页第一条就是空的 → 后面全空 */
        }
    }

    /* ② 逐条精扫
     *
     * ★★ 必须从 page-1 页开始，不能从 page 页开始 ★★
     *   粗扫的判据是"页的第 1 条是否为空"。若某页只写了前几条就断电，
     *   这页的第 1 条仍然是有效记录，粗扫会认为它"写过"从而跳到下一页；
     *   但这一页后面还有空槽 —— 真正该继续写的位置在这里，不在下一页。
     *   不回退的话就会在 Flash 里留下一个【永久空洞】：
     *   扫描把空洞后面的记录也算作有效，回读自检就会报这些空洞 CRC 错。
     *   回退一页再逐条扫，最多多读 8 条，但保证结果精确。 */
    idx = (page > 0UL) ? ((page - 1UL) * REC_PER_PAGE) : 0UL;

    for (;;)
    {
        if (idx >= REC_SLOT_MAX) { break; }

        w25q64_read(idx * REC_SIZE, probe, REC_SIZE);
        if (recorder_slot_state(probe) != REC_SLOT_OK) { break; }

        idx++;
    }

    /* ③ 定写指针 */
    if (idx >= REC_SLOT_MAX)
    {
        /* 记录区写满了 */
        s_count   = REC_SLOT_MAX;
        s_wr_addr = RECORD_AREA_BYTES;
        return s_count;
    }

    state = recorder_slot_state(probe);

    if (state == REC_SLOT_EMPTY)
    {
        /* 这个槽位还是 0xFF，可以直接编程 */
        s_count   = idx;
        s_wr_addr = idx * REC_SIZE;
    }
    else
    {
        /* 半写 / 未收尾：这个槽位里有残数据，不擦扇区就写不回去。
         * NOR Flash 位只能 1→0，所以选择【跳过它】——
         * 只损失 32 字节，不损失这一扇区里其它记录。 */
        s_count   = idx + 1UL;              /* 残破槽也算"已占用" */
        s_wr_addr = (idx + 1UL) * REC_SIZE;
    }

    return s_count;
}

uint32_t recorder_count(void)   { return s_count; }
uint32_t recorder_capacity(void){ return REC_SLOT_MAX; }

/* ============================================================
 * 追加一条记录
 * ============================================================ */
/* 作废当前槽位并前移写指针。
 *
 * ★ 为什么必须这么做：NOR Flash 的位只能从 1 写成 0。
 *   往一个【已经编程过】的单元再写一次，落下去的其实是
 *       旧值 & 新值
 *   头 CRC 必然对不上 → 该槽位永久变成"半写记录"。
 *   所以任何一次"写了一半"的失败，都必须把这个槽位【永久跳过】，
 *   绝不能让下一次采集再写它。 */
static void slot_abandon(void)
{
    s_wr_addr += REC_SIZE;
    s_count++;
}

uint8_t recorder_append(const rec_data_t *d)
{
    uint8_t rc;
    uint8_t probe[REC_SIZE];
    uint8_t just_erased = 0U;

    if ((s_wr_addr + REC_SIZE) > RECORD_AREA_BYTES)
    {
        return 4U;                        /* 容量用尽 */
    }

    /* ★ 跨入新扇区时必须先擦除 —— 而且【必须在前置检查之前】！
     *
     *   Flash 只能把 1 写成 0（"与"逻辑），不先把整扇区擦回 0xFF，
     *   新数据里的 1 就写不进去，结果变成"新旧数据按位与"。
     *   128 条记录 = 8 页 = 4096 字节 = 1 个扇区，所以每 128 条擦一次。
     *
     * ★★ 顺序教训（2026-09-18 长跑实测，12728 条记录的代价）★★
     *   旧顺序是【先做前置检查、再擦扇区】。跨入一个有旧数据的新扇区时：
     *     前置检查读到旧数据 → 判"槽位被占用" → 作废跳过 → 返回 6
     *   于是这个扇区的 128 个槽位被【一条一条作废跳过】，
     *   而【扇区擦除永远不会被执行】—— 检查把擦除挡在了后面。
     *   实测后果：扇区 30~129（恰好是旧版 16 B 格式残留的区域）
     *   整整 12728 个槽位全部被跳过。
     *   ⇒ "先检查后擦除"在跨扇区这个场景下是【逻辑死锁】。 */
    if ((s_wr_addr & (W25Q64_SECTOR_SIZE - 1UL)) == 0UL)
    {
        rc = w25q64_sector_erase(s_wr_addr);
        if (rc != 0U) { return 2U; }      /* 擦除失败：不作废槽位，下一秒重试 */
        just_erased = 1U;
    }

    /* ★ 前置检查：目标槽位必须是"擦除态"(全 0xFF)。
     *
     *   防的是【同一槽位被写两次】：NOR Flash 只能 1→0，
     *   重写一次落下去的是 旧值 & 新值，头 CRC 必然失败。
     *
     *   ★ 只在"这一秒没有刚擦过"时才做：
     *     刚擦过的扇区必然是 0xFF，再读一遍纯属浪费；
     *     而且如果不跳过，上面"先擦后查"的顺序就白改了
     *     —— 等于又把检查放到了擦除前面。 */
    if (just_erased == 0U)
    {
        recorder_read(s_wr_addr / REC_SIZE, probe);
        if (recorder_slot_state(probe) != REC_SLOT_EMPTY)
        {
            slot_abandon();
            return 6U;                    /* 槽位已被占用：已跳过，未写入 */
        }
    }

    recorder_pack(d, s_buf);

#if REC_TWO_STEP_WRITE
    /* ============================================================
     * 分两步写 —— 这是让"尾 CRC"真正有意义的关键
     *
     * 第一步：写偏移 0–29（28 B 数据区 + 2 B 头 CRC）
     * 第二步：写偏移 30–31（2 B 尾 CRC）
     *
     * 两次页编程之间掉电 → 尾 CRC 仍是 0xFF 0xFF，
     * 而头 CRC 已经写好且能通过 → 上电扫描精确判定为
     * "数据完整但记录未收尾"，而不是误判成数据被改坏。
     *
     * 如果合成一次 32 字节页编程，掉电时要么整条写好、
     * 要么整条没写，这个中间状态几乎采集不到，双 CRC 设计就白费了。
     * ============================================================ */
    rc = w25q64_page_program(s_wr_addr, s_buf, REC_TAIL_CRC_OFF);
    if (rc != 0U) { slot_abandon(); return 3U; }

    rc = w25q64_page_program(s_wr_addr + REC_TAIL_CRC_OFF,
                             &s_buf[REC_TAIL_CRC_OFF], 2U);
    if (rc != 0U) { slot_abandon(); return 5U; }
#else
    /* 单步写（对比用）：一次写满 32 字节 */
    rc = w25q64_page_program(s_wr_addr, s_buf, REC_SIZE);
    if (rc != 0U) { slot_abandon(); return 3U; }
#endif

    /* ============================================================
     * ★ 写后回读校验 —— 把"静默失败"变成可检测的失败
     *
     * 【为什么要回读】
     *   页编程可能"假成功"：第二步的编程命令如果恰好在器件
     *   仍忙（上一步还没写完）时发出，会被【静默忽略】——
     *   不报错、不烧毁、就是不干活；而随后的状态轮询又恰好
     *   读到"空闲"，于是整条链路都以为写好了。
     *
     *   实测（2026-09-18 12:44，1 小时长跑）：
     *   4061 条里出现 9 条【连续】的"未收尾"（idx 1629~1637），
     *   头 CRC 全部正确、尾 CRC 全是 0xFFFF —— 正是这个模式。
     *   固件当时【没有报任何错误】（records 与秒数始终 1:1）。
     *
     * 【回读做什么】
     *   把刚写的 32 字节读回来，重算头尾 CRC：
     *     · OK        → 正常返回
     *     · UNFINISHED（头对、尾没写上）→ 重发第二步一次再验，
     *       这是唯一能补救的情况；仍不行就作废该槽位
     *     · 其余（数据区坏了）→ 无法补救，作废该槽位
     *
     * 【代价】
     *   每条多一次 32 字节读（约 0.4 ms）+ 可能的一次 2 字节补写。
     *   采集周期 1 s，这个开销可以忽略；换来的是
     *   "写进去"从【假设】变成【证据】。
     * ============================================================ */
    recorder_read(s_wr_addr / REC_SIZE, probe);
    if (recorder_slot_state(probe) != REC_SLOT_OK)
    {
        if (recorder_slot_state(probe) == REC_SLOT_UNFINISHED)
        {
            /* 只有"尾没写上"还能补救：重发第二步 */
            rc = w25q64_page_program(s_wr_addr + REC_TAIL_CRC_OFF,
                                     &s_buf[REC_TAIL_CRC_OFF], 2U);
            if (rc != 0U) { slot_abandon(); return 5U; }

            recorder_read(s_wr_addr / REC_SIZE, probe);
        }

        if (recorder_slot_state(probe) != REC_SLOT_OK)
        {
            slot_abandon();               /* 补不回来：作废这一格 */
            return 7U;
        }
    }

    s_wr_addr += REC_SIZE;
    s_count++;

    return 0U;
}

/* ============================================================
 * 读回
 * ============================================================ */
void recorder_read(uint32_t index, uint8_t buf[REC_SIZE])
{
    uint32_t i;

    if (index >= REC_SLOT_MAX)
    {
        for (i = 0U; i < REC_SIZE; i++) { buf[i] = 0x00U; }
        return;
    }

    w25q64_read(index * REC_SIZE, buf, REC_SIZE);
}

/* ============================================================
 * 回读自检：逐条重算头、尾两个 CRC
 *
 * 这是"存储链路"的唯一证据。三条总线里 SPI 这一段如果有问题
 * （页编程没写好、扇区没擦干净导致新旧数据按位与、CS 时序错位），
 * CRC 都会当场报错。
 *
 * 耗时估算：每条读 32 字节 ≈ 60 µs，131072 条约 8 秒 —— 只在自检时跑。
 * ============================================================ */
uint32_t recorder_verify(rec_stat_t *stat)
{
    uint8_t  buf[REC_SIZE];
    uint32_t i;
    uint32_t bad = 0U;
    uint8_t  st;

    if (stat != 0)
    {
        stat->total_slots   = 0U;
        stat->ok            = 0U;
        stat->half          = 0U;
        stat->unfinished    = 0U;
        stat->first_bad_idx = 0xFFFFFFFFUL;
    }

    for (i = 0U; i < s_count; i++)
    {
        recorder_read(i, buf);
        st = recorder_slot_state(buf);

        if (stat != 0) { stat->total_slots++; }

        if (st == REC_SLOT_OK)
        {
            if (stat != 0) { stat->ok++; }
        }
        else
        {
            bad++;
            if (stat != 0)
            {
                if (st == REC_SLOT_HALF)       { stat->half++; }
                if (st == REC_SLOT_UNFINISHED) { stat->unfinished++; }
                if (stat->first_bad_idx == 0xFFFFFFFFUL) { stat->first_bad_idx = i; }
            }
        }
    }

    return bad;
}

/* ============================================================
 * 清空：只擦"已经用到的扇区"
 * ★★ 必须擦【整个记录区】，而不是"用到的部分" ★★
 *
 * 旧版按"已经用到多少"决定擦几个扇区，这在逻辑上有个致命漏洞：
 *   ① 记录区里【写指针之后】的扇区可能残留着【更早格式】的数据
 *      （本项目实测：旧版 16 B 格式的 3 万多条记录占了 130 个扇区，
 *       而 `e` 当时只擦了 3 个 —— 因为当时 s_count 只有 288）。
 *   ② 那段残留会让后续写入全部踩坑（见 recorder_append 的前置检查）。
 *   ③ 旧的"收敛循环"骗过了自己：擦完前几个扇区后 scan() 发现
 *      槽 0 已经是空的 → 返回 0 → 认为擦干净了 → 提前退出。
 *      实际上槽 0 之后还有 100 多个扇区的旧数据一动没动。
 *      这又是"返回值、日志、流程全正常，只有结果是错的"那一类 bug。
 *
 * 所以"清空记录区"只有一种诚实实现：把 4 MiB 全部擦一遍。
 * 代价：1024 个扇区 × 约 45 ms ≈ 46 s（最坏 400 ms/个 ≈ 6.8 min）。
 * 这是一次性的手动操作，慢一点可以接受，换来的是"records=0 等于真的干净"。
 * ============================================================ */
uint32_t recorder_erase_all(void)
{
    uint32_t s;
    uint32_t total = RECORD_AREA_BYTES / W25Q64_SECTOR_SIZE;

    for (s = 0U; s < total; s++)
    {
        if (w25q64_sector_erase(s * W25Q64_SECTOR_SIZE) != 0U)
        {
            return (s + 1UL);   /* 第几个扇区失败（从 1 开始）*/
        }
    }

    s_count   = 0U;
    s_wr_addr = 0U;

    return 0U;
}
