#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_capture.py —— 串口抓包 + 自动下发命令

【为什么要写这个而不是用现成的串口助手】
  验收流程里有几步需要"发命令 → 收一大段回读数据"：
      r  → 把 Flash 里的记录按 HEX 全倒出来（可能几万行、几十秒）
      t  → 跑 W4 自检
      m  → 触发一次测量并打印原始量
  用图形串口助手做这几件事很别扭（要手动保存、要盯着何时结束）。
  这个脚本把它变成一条命令，并且能在收到结束标记时自动停。

用法：
    # 只抓 30 秒
    python serial_capture.py --port COM5 --out log.txt --seconds 30

    # 发命令序列：先拿校准系数，再测 3 次
    python serial_capture.py --port COM5 --out log.txt --seq c,m,m,m --gap 2

    # 回读全部记录，收到 #DUMP,end 自动结束
    python serial_capture.py --port COM5 --out log.txt --seq r --stop-marker "#DUMP,end"

    # 跑完整 W4 自检
    python serial_capture.py --port COM5 --out log.txt --seq t --stop-marker "W4 自检结束"

依赖：pyserial（pip install pyserial）

【Windows 上找不到 COM 口】
    设备管理器 → 端口(COM 和 LPT) 里看 CH340 是哪个号。
    若没出现，装 CH340 驱动。
"""

import argparse
import sys
import time
import datetime

try:
    import serial
    import serial.tools.list_ports
    HAVE_PYSERIAL = True
except ImportError:
    HAVE_PYSERIAL = False


def list_ports():
    if not HAVE_PYSERIAL:
        return []
    return list(serial.tools.list_ports.comports())


def main():
    ap = argparse.ArgumentParser(description="串口抓包 + 自动下发命令")
    ap.add_argument("--port", help="串口号，如 COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", help="输出文件（UTF-8）")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="最长抓取时长（秒），默认 30")
    ap.add_argument("--seq", default=None,
                    help="开机后要依次发送的命令，逗号分隔，如 c,m,m,m,r")
    ap.add_argument("--gap", type=float, default=1.5,
                    help="相邻命令之间的间隔（秒），默认 1.5")
    ap.add_argument("--stop-marker", default=None,
                    help="收到这个字符串后自动结束，如 '#DUMP,end'")
    ap.add_argument("--list", action="store_true", help="列出可用串口后退出")
    args = ap.parse_args()

    if args.list or not args.port:
        ports = list_ports()
        if not HAVE_PYSERIAL:
            print("未安装 pyserial。请先：pip install pyserial")
            return 2
        print("可用串口：")
        for p in ports:
            print("  %-8s %s" % (p.device, p.description))
        if not ports:
            print("  （没有发现串口。检查 CH340 驱动是否安装、板子是否插好）")
        return 0

    if not HAVE_PYSERIAL:
        print("ERR: 未安装 pyserial。请先：pip install pyserial")
        return 2

    seq = []
    if args.seq:
        seq = [s.strip() for s in args.seq.split(",") if s.strip()]

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except Exception as e:
        print("ERR: 打开 %s 失败: %s" % (args.port, e))
        print("     常见原因：串口被别的程序（串口助手）占用；或串口号写错。")
        return 2

    print("已打开 %s @ %d 8N1" % (args.port, args.baud))
    if seq:
        print("将依次发送: %s（间隔 %.1f s）" % (" ".join(seq), args.gap))
    print("最长抓取 %.1f 秒%s"
          % (args.seconds,
             "，或直到出现 %r" % args.stop_marker if args.stop_marker else ""))
    print("按 Ctrl+C 可提前结束")
    print("-" * 60)

    out = None
    if args.out:
        out = open(args.out, "w", encoding="utf-8", newline="")
        out.write("# captured by serial_capture.py at %s\n"
                  % datetime.datetime.now().isoformat(timespec="seconds"))
        out.write("# port=%s baud=%d seq=%s\n" % (args.port, args.baud, seq))

    t0 = time.time()
    next_cmd_at = t0 + 1.0          # 先给板子 1 秒把开机横幅吐完
    cmd_i = 0
    stop = False
    tail = ""

    try:
        while True:
            now = time.time()
            if (now - t0) >= args.seconds:
                print("\n[达到时长上限，结束]")
                break

            # 到点了就发下一条命令
            if cmd_i < len(seq) and now >= next_cmd_at:
                c = seq[cmd_i]
                ser.write((c + "\r\n").encode("ascii"))
                ser.flush()
                print("\n>>> 发送命令: %r" % c)
                cmd_i += 1
                next_cmd_at = now + args.gap

            data = ser.read(4096)
            if data:
                text = data.decode("utf-8", errors="replace")
                if out:
                    out.write(text)
                sys.stdout.write(text)
                sys.stdout.flush()

                if args.stop_marker:
                    tail = (tail + text)[-256:]
                    if args.stop_marker in tail:
                        print("\n[检测到结束标记 %r，结束]" % args.stop_marker)
                        stop = True

            if stop:
                break

    except KeyboardInterrupt:
        print("\n[用户中断]")
    finally:
        ser.close()
        if out:
            out.close()
            print("已保存到 %s" % args.out)

    return 0


if __name__ == "__main__":
    sys.exit(main())
