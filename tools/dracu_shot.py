"""串口截图:DRACUPAGE title | DRACUPAGE <页号> -> PNG(与设备上屏画面逐像素一致)。

打开 USB-Serial-JTAG 会让设备复位一次,所以先等启动日志走完再发命令;设备空闲
7 分钟会 deep sleep(USB 也跟着哑),这种情况先按一下设备上的键再跑本脚本。
"""
import sys
import time

import serial
from PIL import Image

port = sys.argv[1]
target = sys.argv[2] if len(sys.argv) > 2 else "title"
out = sys.argv[3] if len(sys.argv) > 3 else "shot.png"
WAIT_BOOT = 20.0
CRLF = "\r\n"

ser = serial.Serial(port, 115200, timeout=0.4)
ser.setDTR(False)
ser.setRTS(False)
boot = bytearray()
deadline = time.time() + WAIT_BOOT
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        boot += chunk
        if b"main_task: Returned" in boot:
            break
time.sleep(0.6)
if b"main_task: Returned" not in boot:
    print("警告: 没等到启动日志(%d 字节),仍然尝试发命令" % len(boot), file=sys.stderr)


def fetch():
    ser.reset_input_buffer()
    ser.write(("DRACUPAGE %s%s" % (target, CRLF)).encode())
    data = bytearray()
    deadline = time.time() + 25
    started = False
    while time.time() < deadline:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            started = True
            if b"DRACUPAGE-END" in data:
                break
        elif started and len(data) > 220000:
            break
    return bytes(data)


def parse(data):
    """切出表头与像素。

    日志里还会出现一行命令回显("串口命令: DRACUPAGE title"),所以不能只找第一个
    "DRACUPAGE " —— 要找出后面跟着三个数字的那一处。
    """
    position = 0
    while True:
        marker = data.find(b"DRACUPAGE ", position)
        if marker < 0:
            return None
        head = data[marker:].split(b"\n", 1)
        parts = head[0].split()
        if len(parts) == 4 and len(head) == 2:
            try:
                width, height, total = int(parts[1]), int(parts[2]), int(parts[3])
            except ValueError:
                position = marker + 9
                continue
            pixels = head[1][:total]
            if len(pixels) >= total:
                return width, height, pixels
        position = marker + 9


for attempt in range(4):
    data = fetch()
    parsed = parse(data)
    if parsed is not None:
        width, height, pixels = parsed
        Image.frombytes("RGB", (width, height), pixels, "raw", "BGR;16").save(out)
        ser.close()
        print("%s %dx%d -> %s" % (target, width, height, out))
        raise SystemExit(0)
    if b"DRACUPAGE-ERR" in data:
        time.sleep(0.5)
        continue
ser.close()
sys.exit("截图失败: target=%s (收到 %d 字节)" % (target, len(data)))
