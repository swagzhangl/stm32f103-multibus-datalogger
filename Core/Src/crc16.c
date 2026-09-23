/* ============================================================
 * crc16.c —— CRC-16/MODBUS（逐位算法，无需查表）
 *
 * 【为什么不用查表法】
 *   查表法要占 512 字节 Flash（256 项 × 2 字节）。
 *   本项目每写一条记录算两次 CRC、每次 30 字节，1 秒才一次 ——
 *   查表省下的那几十微秒毫无意义，而代码清晰度是有意义的。
 *   （如果将来要跑几百 Hz 的协议解析，再换查表法也不迟。)
 *
 * 【逐位算法在干什么】
 *   CRC 把整段数据当成一个巨大的二进制多项式，对生成多项式做模 2 除法，
 *   余数就是校验值。逐位实现每次处理 1 bit：
 *     把当前位异或进最低位 → 若最低位为 1 则右移并异或多项式，否则只右移
 *   "反射"（refin/refout）意味着数据位和结果位都按低位先行处理，
 *   所以多项式也要用反射形式 0xA001（= 0x8005 的位反转）。
 * ============================================================ */
#include "crc16.h"

uint16_t crc16_modbus_update(uint16_t crc, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    uint8_t  bit;

    for (i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)data[i];      /* 当前字节异或进低 8 位 */

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x0001U) != 0U)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);   /* 反射多项式 */
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

uint16_t crc16_modbus_init(uint16_t init, const uint8_t *data, uint16_t len)
{
    return crc16_modbus_update(init, data, len);
}

uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    return crc16_modbus_update(CRC16_MODBUS_INIT, data, len);
}

/* ------------------------------------------------------------
 * CRC-16/CCITT-FALSE（高位先行，与 MODBUS 的反射写法相反）
 *
 *   每字节先异或进【高 8 位】，然后每次判最高位决定要不要异或 0x1021。
 *   这是"不反射"算法的标准写法 —— 和 MODBUS 那种
 *   "异或进低 8 位 + 判最低位 + 右移 + 0xA001" 正好镜像。
 *   两种写法混在一起是最常见的 CRC 实现 bug，所以单独成函数、不复用。
 * ---------------------------------------------------------- */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = CRC16_CCITT_INIT;
    uint16_t i;
    uint8_t  bit;

    for (i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);   /* 当前字节异或进高 8 位 */

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ CRC16_CCITT_POLY);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}
