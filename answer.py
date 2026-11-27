#!/usr/bin/env python3
"""星门织仪 · 最小工具集（v2，稳妥版：每条命令都重试一次，收不到就打印原始字节）

  python3 ~/serial-probe.py aim 64.8      # 定向到 64.8° 并等停稳
  python3 ~/serial-probe.py ledger        # 读底账（扫描峰值 / 留档峰值 / 修复刻度 / 星门）
  python3 ~/serial-probe.py sample        # 取一次样（样本号 / 实测角 / 三路光强）
  python3 ~/serial-probe.py bottom <样本号> <通道1-3> [实测角] [光强]
  python3 ~/serial-probe.py open          # 确认合相（三路留档都够再发）
端口默认 /dev/pts/2，可用 PORT=/dev/pts/N 覆盖。
"""
import os
import select
import struct
import sys
import termios
import time
import tty

PORT = os.environ.get("PORT", "/dev/pts/2")
SYNC = b"\xA5\x5A"


def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def frame(t, seq, p=b""):
    body = bytes([0x03, t, seq, len(p)]) + p
    c = crc16(body)
    return SYNC + body + bytes([c & 0xFF, c >> 8])


def open_port():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY)
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    while select.select([fd], [], [], 0.05)[0]:
        os.read(fd, 4096)
    return fd, old


def recv_frame(fd, want_type, timeout=2.5):
    """收帧；返回 (type, seq, payload) 或 None。失败时打印原始字节方便排查。"""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        buf += os.read(fd, 4096)
        while True:
            i = buf.find(SYNC)
            if i < 0:
                if buf:
                    print("   [非帧数据] %s" % buf.hex(" ").upper())
                buf = b""
                break
            buf = buf[i:]
            if len(buf) < 6:
                break
            ln = buf[5]
            total = 8 + ln
            if len(buf) < total:
                break
            fr, buf = buf[:total], buf[total:]
            body = fr[2:6 + ln]
            c_calc = crc16(body)
            c_given = fr[6 + ln] | (fr[7 + ln] << 8)
            if c_calc != c_given:
                if (c_calc ^ c_given) != 0x0100:
                    print("   [CRC 不符，丢弃] %s" % fr.hex(" ").upper())
                    continue
                print("   （模拟器 CRC 与规程差 0x0100，按已知瑕疵接受）")
            if fr[3] == want_type:
                return fr[3], fr[4], fr[6:6 + ln]
    return None


def transact(fd, typ, payload=b"", retries=2, wait=2.5):
    want = 0x80 if typ in (0x30, 0x40) else (typ | 0x80)
    seq = int(time.time() * 10) % 256
    for attempt in range(retries + 1):
        os.write(fd, frame(typ, seq, payload))
        got = recv_frame(fd, want, wait)
        if got:
            return got
        if attempt < retries:
            print("   （没收到，重发一次…）")
            time.sleep(0.4)
    return None


def f32(v):
    return struct.pack("<f", v)


def do_aim(fd, angle):
    t = time.time()
    while time.time() - t < 20:
        got = transact(fd, 0x20, f32(angle))
        if not got:
            print("定向无应答：检查窗口是否还开着、端口是否 /dev/pts/2")
            return
        _, _, p = got
        cur, tgt, st = struct.unpack_from("<ffB", p, 0)
        print("实测 %.1f°  目标 %.1f°  %s" % (cur, tgt, "转动中" if st else "已停稳"))
        if not st:
            return
        time.sleep(0.5)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    fd, old = open_port()
    try:
        cmd = sys.argv[1]
        if cmd == "aim":
            do_aim(fd, float(sys.argv[2]))
        elif cmd == "ledger":
            got = transact(fd, 0x03)
            if not got:
                print("底账无应答")
                return
            p = got[2]
            scan = struct.unpack_from("<HHH", p, 0)
            keep = struct.unpack_from("<HHH", p, 6)
            print("扫描峰值: 1->%d  2->%d  3->%d" % scan)
            print("留档峰值: 1->%d  2->%d  3->%d" % keep)
            print("修复刻度: %d   星门: %s" % (p[12], "已开启" if p[13] else "未开启"))
        elif cmd == "sample":
            got = transact(fd, 0x02)
            if not got:
                print("取样无应答")
                return
            p = got[2]
            idx = struct.unpack_from("<H", p, 0)[0]
            cur, tgt = struct.unpack_from("<ff", p, 2)
            g = struct.unpack_from("<HHH", p, 10)
            print("样本号 %d  实测 %.1f°  目标 %.1f°  %s" % (idx, cur, tgt, "转动中" if p[16] else "已停稳"))
            print("三路光强: 1->%d  2->%d  3->%d   修复刻度 %d" % (g[0], g[1], g[2], p[17]))
        elif cmd == "bottom":
            idx = int(sys.argv[2])
            ch = int(sys.argv[3])
            ang = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
            val = int(sys.argv[5]) if len(sys.argv) > 5 else 0
            payload = bytes([1]) + struct.pack("<H", idx) + bytes([ch]) + f32(ang) + struct.pack("<H", val)
            got = transact(fd, 0x30, payload)
            if not got:
                print("底片无应答")
                return
            p = got[2]
            print("底片回执: 状态码 %02X  修复刻度 %d" % (p[1], p[2]))
        elif cmd == "open":
            got = transact(fd, 0x40, b"", wait=3)
            if not got:
                print("启门无应答")
                return
            p = got[2]
            print("启门回执: 状态码 %02X  修复刻度 %d" % (p[1], p[2]))
        else:
            print(__doc__)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        os.close(fd)


if __name__ == "__main__":
    main()
