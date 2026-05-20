#!/usr/bin/env python3
# Read the MCU chip temperature from the firmware status line over the USB-CDC console.
# Status line (src/main.cpp): "... %5.1fW %.0f℃%.0f℃ ..." -> 1st=NTC (nan on mock), 2nd=ucTemp (chip).
# Usage: chiptemp.py <port> <seconds> [command-to-send-first]
import sys, time, re, serial

port = sys.argv[1]
dur = float(sys.argv[2])
cmd = sys.argv[3] if len(sys.argv) > 3 else None

# 2nd ℃ value; tolerate "nan" and negatives.
pat = re.compile(r'([-\d.]+|nan)℃([-\d.]+|nan)℃')

s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.3)
if cmd:
    for line in cmd.split(';'):
        s.write((line.strip() + '\n').encode())
        s.flush()
        print(f"-> sent: {line.strip()}", flush=True)
        time.sleep(0.5)

t0 = time.time()
samples = []
buf = b''
while time.time() - t0 < dur:
    buf += s.read(256)
    while b'\n' in buf:
        raw, buf = buf.split(b'\n', 1)
        line = raw.decode('utf-8', 'replace').strip()
        m = pat.search(line)
        if m:
            try:
                t = float(m.group(2))
            except ValueError:
                continue
            samples.append((time.time() - t0, t))
            print(f"[{time.time()-t0:6.1f}s] chip={t:.0f}C   {line[:70]}", flush=True)
s.close()
if samples:
    temps = [t for _, t in samples]
    print(f"\n== {len(samples)} samples over {dur:.0f}s: "
          f"min={min(temps):.0f} max={max(temps):.0f} last={temps[-1]:.0f} (C)")
else:
    print("\n== no temperature samples parsed (is firmware booted? right port?)")
