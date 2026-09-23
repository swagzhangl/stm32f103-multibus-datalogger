/* ============================================================
 * self_test.h —— W1/W2/W4 的验收自检
 *
 * 每一项都对应执行计划里的一条验收标准，且都往串口打出可留存的证据。
 * 用法：串口发单字符命令触发（见 app_tasks.c 的命令表），
 *       或在 app_config.h 里把 APP_SELFTEST_ON_BOOT 设成 1 开机自动跑。
 * ============================================================ */
#ifndef SELF_TEST_H
#define SELF_TEST_H

#include <stdint.h>

/* W4 第 1 步：四种 SPI 模式的回环自测 + 主从模式不匹配演示。
 * 结论：回环能证明"位引擎正确"，不能证明"模式配对正确"——
 *       后者要靠 w25q64_id_all_modes() 和逻辑分析仪波形。 */
void selftest_spi_four_modes(void);

/* W4 第 3 步（最硬的证据）：分别用四种模式读 W25Q64 的 JEDEC ID。
 *   Mode0 → 应得 EF 40 17
 *   Mode3 → 也应得 EF 40 17（W25Q64 支持 0 和 3）
 *   Mode1/Mode2 → 必然读错（这就是"模式配错会怎样"的物理证据） */
void selftest_w25q64_id_all_modes(void);

/* W4 第 2/3 步：记录闭环
 *   写 N 条 → 回读 → 逐条校验头尾双 CRC → 打印比对结果
 *   含页边界（第 256 字节附近）与扇区边界（第 4096 字节附近）的专项测试
 * 返回 0 = 全部通过 */
uint8_t selftest_record_roundtrip(void);

/* 双 CRC 设计的验证：人为构造断电的三种状态，验证能被正确区分
 *   ① 半写（只有数据区，且数据区本身不完整）→ 应判 HALF
 *   ② 数据写完但尾 CRC 未写          → 应判 UNFINISHED（关键！）
 *   ③ 完整记录                       → 应判 OK
 * 返回 0 = 三种都判对了 */
uint8_t selftest_powerloss_cases(void);

/* 把已存记录按 HEX 吐给 PC（REC,<idx>,<64 hex>），供 parse_records.py 校验 */
void selftest_records_dump(void);

/* 全量回读已存记录并重算双 CRC，打印分类统计 */
void selftest_records_verify(void);

/* 打印 33 个出厂校准系数（#CALIB 行），供 verify_bme280.py 复算 */
void selftest_bme280_calib_dump(void);

/* 触发一次测量并打印 #RAW/#CMP/#OUT 三行，供 verify_bme280.py 逐位比对 */
void selftest_bme280_measure_dump(void);

/* 依次跑上面所有项 */
void selftest_run_all(void);

#endif /* SELF_TEST_H */
