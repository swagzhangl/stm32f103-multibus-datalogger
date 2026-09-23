# -*- coding: utf-8 -*-
"""
长跑【过夜自动采样】：每隔 N 分钟发一次 s/p，累计栈水位样本。

为什么需要它：
  uxTaskGetStackHighWaterMark 给的是【历史最小剩余】，只反映已经跑过的路径。
  只采一两次就照着缩栈，将来一跑没跑过的分支就可能 HardFault。
  睡一觉起来只有 1 条样本 = 那个压缩比还是不能写进简历。
  挂这个脚本过夜，8 小时能攒 16 条以上，就够了。

用法：
  python tools\\longrun_watch.py 8              # 采 8 小时，每 30 分钟一次
  python tools\\longrun_watch.py 8 --interval 20 # 每 20 分钟一次
  python tools\\longrun_watch.py 0.05 --interval 0.5   # 自测：3 分钟、每 30 秒

只发 s / p —— 不碰 SPI 锁，不会影响采集连续性。
到点会自己退出，退出后 COM8 才空出来给 longrun_finish.py 用。
"""
import sys, os, time, argparse
import serial

PORT, BAUD = "COM8", 115200
LOG_DIR = r"D:\work\2026-09-17-15-24-45\firmware\logs"
STACK_LOG = os.path.join(LOG_DIR, "longrun_stack.txt")

ap = argparse.ArgumentParser()
ap.add_argument("hours", type=float, nargs="?", default=8.0,
                help="采集总时长（小时）")
ap.add_argument("--interval", type=float, default=30.0,
                help="采样间隔（分钟），默认 30")
ap.add_argument("--port", default=PORT)
args = ap.parse_args()

os.makedirs(LOG_DIR, exist_ok=True)
end_t = time.time() + args.hours * 3600.0
gap_s = max(args.interval * 60.0, 20.0)

print("=" * 66)
print("过夜自动采样")
print("=" * 66)
print("  端口     : %s" % args.port)
print("  总时长   : %.2f 小时" % args.hours)
print("  间隔     : %.1f 分钟" % args.interval)
print("  预计样本 : 约 %d 条" % max(1, int(args.hours * 60 / args.interval)))
print("  结束时刻 : %s" % time.strftime("%H:%M:%S", time.localtime(end_t)))
print("  日志     : %s" % STACK_LOG)
print("=" * 66)
print()
print("★ 这个窗口【不要关】。到点它会自己退出。")
print("★ 收尾前等它退出，再跑 python tools\\longrun_finish.py")
print()


def sample(n):
    try:
        ser = serial.Serial(args.port, BAUD, timeout=1,
                            dsrdtr=False, rtscts=False)
    except Exception as e:
        print("  [%d] 打不开 %s：%s" % (n, args.port, e))
        print("       （多半是串口助手开着，或上一个实例没退出）")
        return None

    buf = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        m = ser.in_waiting
        if m:
            buf.append(ser.read(m).decode("utf-8", "replace"))
        time.sleep(0.05)

    ser.write(b"s\r\n")
    t0 = time.time()
    while time.time() - t0 < 2.0:
        m = ser.in_waiting
        if m:
            buf.append(ser.read(m).decode("utf-8", "replace"))
        time.sleep(0.05)

    ser.write(b"p\r\n")
    t0 = time.time()
    while time.time() - t0 < 2.0:
        m = ser.in_waiting
        if m:
            buf.append(ser.read(m).decode("utf-8", "replace"))
        time.sleep(0.05)
    ser.close()

    text = "".join(buf)
    stk = [l for l in text.splitlines() if l.startswith("#STK")]
    sta = [l for l in text.splitlines() if l.startswith("#STA")]
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")

    with open(STACK_LOG, "a", encoding="utf-8") as f:
        f.write("# sample %s\n" % ts)
        if stk: f.write(stk[-1] + "\n")
        if sta: f.write(sta[-1] + "\n")

    print("  [%d] %s" % (n, ts))
    if stk: print("       " + stk[-1])
    if sta: print("       " + sta[-1])
    else:   print("       （没收到 #STA —— 板子可能重启或掉电了）")
    return sta[-1] if sta else None


n = 0
uptimes = []
while time.time() < end_t:
    n += 1
    sta = sample(n)
    if sta:
        # 提取 uptime，用来发现"中途重启过"
        try:
            up = int(sta.split("uptime=")[1].split("s")[0])
            uptimes.append(up)
            if len(uptimes) >= 2 and uptimes[-1] < uptimes[-2]:
                print("       ⚠️ uptime 变小了 —— 板子中途重启过！")
                print("          「连续运行」这个数已经不成立，需要重新开始。")
        except (IndexError, ValueError):
            pass

    left = end_t - time.time()
    if left <= 0:
        break
    time.sleep(min(gap_s, left))

print()
print("=" * 66)
print("采样结束，共 %d 条。现在可以跑：" % n)
print("    python tools\\longrun_finish.py")
print("=" * 66)
