#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_sample_log.py —— 生成一份"自洽的假日志"，用来在没板子时先跑通 PC 工具链

【为什么需要这个】
  板子还没焊好 / 串口还没通的时候，你没法验证 verify_bme280.py 和
  parse_records.py 到底对不对。等真跑起来才发现工具本身有 bug，
  会把"工具问题"误判成"固件问题"，白查半天。

  所以先造一份格式完全一致的假日志：内容是用本仓库的 Python 实现
  算出来的，各字段自洽。用它跑一遍工具，工具报 PASS，
  就说明【解析与比对逻辑】是通的；之后接上真板子，
  失败就一定是硬件/固件问题。

【它不能证明什么】
  它【不能】证明固件里的 C 实现和这里一致 ——
  那要靠板子开机横幅的 #CRC 检查值、以及真机跑出来的日志来对。
  这一点必须说清楚，否则容易自我安慰。

用法：
    python make_sample_log.py sample_log.txt
    python make_sample_log.py sample_log.txt --records 40 --with-powerloss
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from multibus import (pack_record, compensate, crc16_modbus,
                      REC_SIZE, REC_DATA_LEN, REC_HEAD_CRC_OFF, REC_TAIL_CRC_OFF,
                      CALIB_KEYS, CRC16_MODBUS_CHECK)

# 一颗真实 BME280 的典型出厂校准系数（用于演示，不代表你的芯片）
SAMPLE_CALIB = {
    "T1": 28253, "T2": 26382, "T3": 50,
    "P1": 37150, "P2": -10680, "P3": 3024, "P4": 6586, "P5": -120,
    "P6": -7, "P7": 9900, "P8": -10230, "P9": 4285,
    "H1": 75, "H2": 360, "H3": 0, "H4": 318, "H5": 50, "H6": 30,
}

# 典型原始 ADC 值
SAMPLE_ADC = [
    (519888, 330000, 26500),
    (520100, 330150, 26600),
    (520340, 329880, 26480),
]


def main():
    ap = argparse.ArgumentParser(description="生成自洽的假日志用于演练 PC 工具")
    ap.add_argument("outfile")
    ap.add_argument("--records", type=int, default=40, help="生成多少条记录")
    ap.add_argument("--with-powerloss", action="store_true",
                    help="额外构造断电三种情况（半写 / 未收尾）")
    args = ap.parse_args()

    lines = []
    lines.append("# ===== 由 make_sample_log.py 生成的演示日志（非真实硬件）=====")
    lines.append("#CRC,check='123456789',got=0x%04X,expect=0x%04X,%s"
                 % (crc16_modbus(b"123456789"), CRC16_MODBUS_CHECK,
                    "PASS" if crc16_modbus(b"123456789") == CRC16_MODBUS_CHECK else "FAIL"))

    # ---- 校准系数行（格式与固件 selftest_bme280_calib_dump 完全一致）----
    calib_line = "#CALIB," + ",".join("%s=%d" % (k, SAMPLE_CALIB[k])
                                      for k in CALIB_KEYS) + ",count=33"
    lines.append(calib_line)

    # ---- 测量样本行（#RAW / #CMP / #OUT）----
    for (adc_T, adc_P, adc_H) in SAMPLE_ADC:
        r = compensate(adc_T, adc_P, adc_H, SAMPLE_CALIB)
        lines.append("#RAW,adc_T=%d,adc_P=%d,adc_H=%d,t_fine=%d"
                     % (adc_T, adc_P, adc_H, r["t_fine"]))
        lines.append("#CMP,press_q24_8=%d,hum_q22_10=%d"
                     % (r["press_q24_8"], r["hum_q22_10"]))
        lines.append("#OUT,temp_centi=%d,humi_centi=%d,press_pa=%d"
                     % (r["temp_centi"], r["humi_centi"], r["press_pa"]))
        lines.append("")

    # ---- 记录流 ----
    lines.append("#DUMP,begin,total=%d,size=%d" % (args.records, REC_SIZE))

    for i in range(args.records):
        buf = pack_record(
            seq=i,
            timestamp_s=1000 + i,
            temp_milli=20000 + (i % 2000),
            humi_milli=40000 + (i % 4000),
            press_pa=101325 + (i % 500),
        )
        lines.append("REC,%d,%s" % (i, buf.hex().upper()))

    if args.with_powerloss:
        # 情况②：数据写完但尾 CRC 未写（手工把尾 CRC 抹成 0xFFFF）
        base = args.records
        buf = bytearray(pack_record(seq=base, timestamp_s=1000 + base,
                                    temp_milli=21234, humi_milli=41234,
                                    press_pa=101500))
        buf[REC_TAIL_CRC_OFF] = 0xFF
        buf[REC_TAIL_CRC_OFF + 1] = 0xFF
        lines.append("REC,%d,%s" % (base, bytes(buf).hex().upper()))

        # 情况③：编程中断电 —— 只写了前 12 字节，其余仍是 0xFF
        buf2 = bytearray(pack_record(seq=base + 1, timestamp_s=1000 + base + 1,
                                     temp_milli=21345, humi_milli=42345,
                                     press_pa=101600))
        partial = bytearray(b"\xFF" * REC_SIZE)
        partial[0:12] = buf2[0:12]
        lines.append("REC,%d,%s" % (base + 1, bytes(partial).hex().upper()))

    lines.append("#DUMP,end,total=%d" % (args.records + (2 if args.with_powerloss else 0)))

    with open(args.outfile, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    print("已生成 %s" % args.outfile)
    print("  测量样本 : %d 组（#CALIB + #RAW/#CMP/#OUT）" % len(SAMPLE_ADC))
    print("  记录     : %d 条" % args.records)
    if args.with_powerloss:
        print("  额外构造 : 1 条「未收尾」 + 1 条「半写」（用于验证双 CRC 分类）")
    print()
    print("接着跑这两个命令验证工具链：")
    print("  python verify_bme280.py %s" % args.outfile)
    print("  python parse_records.py %s%s"
          % (args.outfile, "  （预期有 2 条异常，属正常）" if args.with_powerloss else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
