# -*- coding: utf-8 -*-
"""
长跑【中途采样】：只发 s / p，不占 SPI 锁，不影响采集连续性。

用法： python longrun_sample.py
结果追加到 logs\\longrun_stack.txt（带时间戳，最后算栈水位要用）
"""
import sys, os, time
import serial

# 串口与日志目录都不依赖本机固定路径（端口可用 MULTIBUS_PORT 环境变量覆盖）
PORT = os.environ.get("MULTIBUS_PORT", "COM8")
BAUD = 115200
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(os.path.dirname(_TOOLS_DIR), "logs")
STACK_LOG = os.path.join(LOG_DIR, "longrun_stack.txt")

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
        time.sleep(0.04)
def snap():
    return "".join(buf)

pump(1.5)
ser.write(b"s\r\n"); pump(2)
ser.write(b"p\r\n"); pump(2)
ser.close()

stk = [l for l in snap().splitlines() if l.startswith("#STK")]
sta = [l for l in snap().splitlines() if l.startswith("#STA")]

ts = time.strftime("%Y-%m-%dT%H:%M:%S")
P("采样时间 %s" % ts)
if stk:
    P("  " + stk[-1])
if sta:
    P("  " + sta[-1])

with open(STACK_LOG, "a", encoding="utf-8") as f:
    f.write("# sample %s\n" % ts)
    if stk:
        f.write(stk[-1] + "\n")
    if sta:
        f.write(sta[-1] + "\n")

n = sum(1 for l in open(STACK_LOG, encoding="utf-8", errors="replace")
        if l.startswith("# sample"))
P("")
P("已累计采样 %d 次（存于 logs\\longrun_stack.txt）" % n)
P("建议：至少累计 10 次以上再算栈水位，且必须覆盖 24 h 全程")
sys.stdout.write("\n".join(out))
