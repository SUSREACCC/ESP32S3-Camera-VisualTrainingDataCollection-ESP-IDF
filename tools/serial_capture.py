"""复位 ESP32 并抓串口。

用法: serial_run.py <port> <baud> <seconds> <outfile> [--no-reset]

复位时序（ESP32 自动下载电路）:
  RTS -> EN, DTR -> GPIO0
  正常运行 = GPIO0 高(不拉低)，EN 拉低再放高
"""
import sys, time, serial

def main():
    if len(sys.argv) < 5:
        print(__doc__); return 1
    port, baud, secs, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    do_reset = "--no-reset" not in sys.argv

    ser = serial.Serial(port, baud, timeout=0.1)
    if do_reset:
        ser.dtr = False          # GPIO0 高 -> 正常启动
        ser.rts = True           # EN 低 -> 复位
        time.sleep(0.15)
        ser.rts = False          # 释放复位
        time.sleep(0.05)

    t_end = time.time() + secs
    buf = bytearray()
    while time.time() < t_end:
        try:
            n = ser.in_waiting
            chunk = ser.read(n if n else 1)
        except serial.SerialException as e:
            print(f"[serial] 断开: {e}"); break
        if chunk:
            buf.extend(chunk)
    ser.close()

    with open(out, "wb") as f:
        f.write(bytes(buf))
    print(f"[capture] {len(buf)} bytes -> {out}")
    return 0

sys.exit(main())
