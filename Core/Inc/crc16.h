/* ============================================================
 * crc16.h —— CRC-16/MODBUS
 *
 *   多项式 0x8005，反射后为 0xA001
 *   初值   0xFFFF
 *   输入反射：是    输出反射：是
 *   存储/传输：低字节先发（小端）
 *
 * 【和 BLE 项目里 CRC16-ARC 的区别】
 *   CRC16-ARC  poly=0xA001 init=0xAAAA 大端存储
 *   CRC16-MODBUS poly=0xA001 init=0xFFFF 【小端】存储
 *   —— 只有初值和字节序不同，这也是 Modbus 最容易想当然写错的地方：
 *      算法算对了，但把两个字节按大端发出去，协议就是不通。
 * ============================================================ */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

#define CRC16_MODBUS_INIT   0xFFFFU

/* ============================================================
 * 尾 CRC 必须用【另一个多项式】，不能沿用 MODBUS
 *
 * ★★ 这是一个实测 + 推导挖出来的设计缺陷（2026-09-17），务必读完 ★★
 *
 * 1) 现象
 *    原实现：尾 CRC = crc16_modbus_update(头CRC, buf+28, 2)
 *    （= CRC-16/MODBUS 覆盖 0–29，从头 CRC 的状态续算）
 *    上板倒出 1246 条记录，1245 条的尾 CRC 存值都是 0x0000。
 *
 * 2) 第一层原因：CRC 的"追加性质"
 *    把一段数据的 CRC 追加到数据后再算一次，结果恒为 0x0000。
 *    （init=0xFFFF、refin/refout=1、xorout=0 的 CRC-16 都有这个性质。）
 *
 * 3) 第二层原因（更致命，换初值也救不回来）
 *    CRC 对初值是【仿射】的：crc_J(m) = crc_0(m) ⊕ g_len(J)，g 线性。
 *    于是对于任意初值 J：
 *        尾 = crc_J(数据 ‖ 头CRC)
 *           = crc_0(数据 ‖ 头CRC) ⊕ g_30(J)
 *           = g_30(0xFFFF) ⊕ g_30(J)          ← 头 CRC 满足自身校验，把数据吃掉了
 *           = g_30(0xFFFF ⊕ J)                ← 与数据【完全无关】
 *    ⇒ 只要第二条 CRC 覆盖范围是"数据 + 第一条 CRC"、且用的是【同一个多项式】，
 *      那它恒等于一个只由两个初值和长度决定的常数。
 *
 *    实测两次，两个初值都验证了这个公式：
 *      J = 0xFFFF（原实现） → 尾 CRC 恒为 0x0000
 *      J = 0x0000（中途改过）→ 尾 CRC 恒为 0x4FFE
 *
 * 4) 结论与修法
 *    "双 CRC"这个想法是对的，但【第二条 CRC 不能覆盖第一条 CRC 的输出】，
 *    否则它必然退化成常数 —— 只剩"写完没写完"这一个作用，
 *    退化成了一个 2 字节固定标记，不是校验值。
 *
 *    修法有两个，本项目选 (a)：
 *      (a) 换【多项式】：尾 CRC 用 CRC-16/CCITT-FALSE(0x1021)，
 *          覆盖范围仍是 0–29，语义不变，但换掉多项式就打破了上面的恒等式。
 *      (b) 只覆盖数据区 0–27、换初值 —— 也行，但就丢了"尾 CRC 覆盖头 CRC"这层语义。
 *
 *    ⇒ 现在：头 CRC = MODBUS(0–27)，尾 CRC = CCITT-FALSE(0–29)，
 *      两条 CRC 覆盖范围不同、多项式不同，互为真正独立的证据。
 * ============================================================ */

/* CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，不反射，xorout 0
 *
 * ★ 存储字节序：本工程统一【小端】存（与记录里其它字段一致）。
 *   标准 CCITT-FALSE 的"标准检查值"是按大端读出的 0x29B1，
 *   本函数返回的寄存器值是 0x29B1，存进 Flash 时按小端放 B1 29。
 *   —— 字节序只影响"怎么存"，不影响算法正确性；
 *      关键是固件和 PC 端用同一套约定。 */
#define CRC16_CCITT_POLY    0x1021U
#define CRC16_CCITT_INIT    0xFFFFU

/* 标准检查值：ASCII "123456789" 在 CRC-16/CCITT-FALSE 下的寄存器值 */
#define CRC16_CCITT_CHECK   0x29B1U

/* 从初值 0xFFFF 开始，对 data[0..len-1] 算 CRC-16/MODBUS */
uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

/* 指定初值重算（MODBUS）*/
uint16_t crc16_modbus_init(uint16_t init, const uint8_t *data, uint16_t len);

/* CRC-16/CCITT-FALSE —— 记录【尾 CRC】用。
 * 多项式 0x1021，高位先行（不反射），init 0xFFFF，xorout 0。
 * 为什么尾 CRC 不能用 MODBUS：见本文件上方"尾 CRC 必须用另一个多项式"一节。 */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

/* 从"已经算到一半的状态"继续往下算。
 *
 * ★ 这不是可选的优化，而是 CRC 的固有能力：CRC 是流式（有状态）算法，
 *   crc(buf, 30) 完全等价于 crc16_modbus_update(crc16_modbus(buf, 28), buf+28, 2)。
 *
 *   本项目的 32 字节记录正是用这个性质：
 *     头 CRC = crc16_modbus(buf, 28)           覆盖数据区
 *     尾 CRC = crc16_modbus_update(头CRC, buf+28, 2)   续算两个字节
 *   既省掉一次重复计算，又正好把"尾 CRC 覆盖了头 CRC"这件事在代码里写明。 */
uint16_t crc16_modbus_update(uint16_t crc, const uint8_t *data, uint16_t len);

#endif /* CRC16_H */
