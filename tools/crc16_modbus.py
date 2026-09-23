#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
crc16_modbus.py —— CRC-16/MODBUS 自检 + 任意数据计算

用法：
    python crc16_modbus.py --selftest
    python crc16_modbus.py --hex "3132333435363738 39"
    python crc16_modbus.py --ascii "123456789"

【为什么要单独做这一个脚本】
  固件开机横幅里会打一行：
      #CRC,check='123456789',got=0x4B37,expect=0x4B37,PASS
  把这一行的 got 和本脚本 --selftest 的输出对一下，
  就能在 3 秒内确认"固件和 PC 是同一套 CRC"。
  如果不一致，后面所有记录校验都会失败，
  而且看起来会非常像"Flash 坏了" —— 先排除算法问题，再查硬件。
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from multibus import crc16_modbus, crc16_selftest, CRC16_MODBUS_CHECK


def main():
    ap = argparse.ArgumentParser(description="CRC-16/MODBUS 计算与自检")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--selftest", action="store_true",
                   help="用标准检查值 '123456789' 验证实现")
    g.add_argument("--hex", help="十六进制字节串，空格可选，如 '3132333435'")
    g.add_argument("--ascii", help="按 ASCII 取字节")
    ap.add_argument("--low-first", action="store_true",
                    help="按 Modbus 习惯打印低字节先发的顺序")
    args = ap.parse_args()

    if args.selftest:
        ok, got = crc16_selftest()
        print("CRC-16/MODBUS 自检")
        print("  输入        : ASCII '123456789'")
        print("  期望检查值  : 0x%04X" % CRC16_MODBUS_CHECK)
        print("  本实现算出  : 0x%04X" % got)
        print("  结果        : %s" % ("PASS" if ok else "FAIL"))
        print()
        print("  请把固件开机横幅里的这一行拿出来对：")
        print("      #CRC,check='123456789',got=0x%04X,expect=0x%04X,PASS"
              % (got, CRC16_MODBUS_CHECK))
        return 0 if ok else 1

    if args.hex:
        cleaned = args.hex.replace(" ", "").replace(",", "").replace("-", "")
        if len(cleaned) % 2:
            print("ERR: 十六进制长度必须是偶数", file=sys.stderr)
            return 2
        try:
            data = bytes.fromhex(cleaned)
        except ValueError as e:
            print("ERR: 不是合法十六进制串: %s" % e, file=sys.stderr)
            return 2
    else:
        data = args.ascii.encode("utf-8")

    crc = crc16_modbus(data)
    print("输入 %d 字节" % len(data))
    print("  CRC-16/MODBUS = 0x%04X" % crc)
    if args.low_first:
        print("  低字节先发    = %02X %02X" % (crc & 0xFF, (crc >> 8) & 0xFF))
    else:
        print("  大端写法      = %02X %02X" % ((crc >> 8) & 0xFF, crc & 0xFF))
        print("  （注意 Modbus 在线路上是小端：先发 0x%02X 再发 0x%02X）"
              % (crc & 0xFF, (crc >> 8) & 0xFF))
    return 0


if __name__ == "__main__":
    sys.exit(main())
