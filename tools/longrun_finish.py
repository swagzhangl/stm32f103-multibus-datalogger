# -*- coding: utf-8 -*-
"""
长跑【收尾】：取栈水位 → 倒出全部记录 → 存日志。

用法： python longrun_finish.py
倒出可能很慢（几千条 × 每行约 80 字符），脚本最多等 20 分钟。
"""
import sys, os, time
import serial

PORT, BAUD = "COM8", 115200
LOG_DIR = r"D:\work\2026-09-17-15-24-45\firmware\logs"
STACK_LOG = os.path.join(LOG_DIR, "longrun_stack.txt")
OUT = os.path.join(LOG_DIR, "longrun_records.txt")

out = []
def P(s=""):
    out.append(s)

os.makedirs(LOG_DIR, exist_ok=True)

try:
    ser = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
except Exception as e:
    P("!! 打不开 %s :: %s" % (PORT, e)); P("   先关掉串口助手。")
    sys.stdout.write("\n".join(out)); sys.exit(2)

buf = []
def pump(sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        n = ser.in_waiting
        if n:
            buf.append(ser.read(n).decode("utf-8", "replace"))
        time.sleep(0.03)
def snap():
    return "".join(buf)

pump(1.5)

# ★ 先倒出、后采样：倒出（r）是 comm 任务最深的调用链之一。
#   uxTaskGetStackHighWaterMark 给的是"历史最小剩余"——
#   深路径没跑过就采样，得到的数字会偏乐观（照它缩栈会溢出）。
#   所以顺序必须是：先 r，把深路径走到，再采栈水位。

P("[1] 倒出记录（可能几分钟，最多等 20 分钟）……")
mark = len(snap())
ser.write(b"r\r\n")
t0 = time.time()
done = False
while time.time() - t0 < 1200:
    pump(3)
    if "#DUMP,end" in snap()[mark:]:
        done = True
        break
tail = snap()[mark:]
n_rec = sum(1 for l in tail.splitlines() if l.startswith("REC,"))
begin = [l for l in tail.splitlines() if "#DUMP,begin" in l]
P("    " + (begin[0] if begin else "（没收到 #DUMP,begin）"))
P("    收到 REC 行 %d 条" % n_rec)
P("    " + ([l for l in tail.splitlines() if "#DUMP,end" in l] or ["（没收到 #DUMP,end）"])[0])
if not done:
    P("    ⚠️ 20 分钟没等到 #DUMP,end，可能中途断了")

dump_end_mark = len(snap())

P("")
P("[2] 取栈水位（在倒出之后采，深路径已覆盖）")
ser.write(b"s\r\n"); pump(2)
ser.write(b"p\r\n"); pump(2)
tail2 = snap()[dump_end_mark:]
stk = [l for l in tail2.splitlines() if l.startswith("#STK")]
sta = [l for l in tail2.splitlines() if l.startswith("#STA")]
ts = time.strftime("%Y-%m-%dT%H:%M:%S")
with open(STACK_LOG, "a", encoding="utf-8") as f:
    f.write("# sample %s (finish, after dump)\n" % ts)
    if stk: f.write(stk[-1] + "\n")
    if sta: f.write(sta[-1] + "\n")
if stk: P("    " + stk[-1])
if sta: P("    " + sta[-1])

ser.close()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("# long-run finish @ %s\n" % ts)
    f.write(snap())
P("")
P("[3] 日志已存: %s (%d 字节)" % (OUT, os.path.getsize(OUT)))
P("")
P("下一步：告诉我一声，我跑 parse_records.py + stack_budget.py 出结论")
sys.stdout.write("\n".join(out))
