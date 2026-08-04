#!/usr/bin/env python3
# Log Teensy serial output to a timestamped file, echoing to screen.
# Also forwards anything you type (+ Enter) to the Teensy — for the
# phase commands: b, s, i, d, c.
import serial, sys, time, subprocess, os, signal, threading

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"

# Evict any port squatter (teensy-monitor, IDE helpers) before opening.
pids = subprocess.run(["lsof", "-t", PORT],
                      capture_output=True, text=True).stdout.split()
for pid in pids:
    os.kill(int(pid), signal.SIGTERM)
if pids:
    time.sleep(0.5)

fname = time.strftime("trace_%Y%m%d_%H%M%S.log")
ser = serial.Serial(PORT, 115200)
print(f"logging {PORT} -> {fname}   (type b/s/i/d/c + Enter; Ctrl-C to stop)")

def keyboard_to_serial():
    for line in sys.stdin:
        ser.write(line.strip().encode())

threading.Thread(target=keyboard_to_serial, daemon=True).start()

with open(fname, "w") as f:
    try:
        while True:
            line = ser.readline().decode(errors="replace").rstrip()
            print(line)
            f.write(line + "\n")
            f.flush()
    except KeyboardInterrupt:
        print(f"\nsaved {fname}")
