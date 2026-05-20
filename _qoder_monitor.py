import os, sys, time
import serial

ser = serial.Serial('COM2', 115200, timeout=0.5)
print('Serial monitor opened on COM2 @ 115200', flush=True)
print('='*60, flush=True)

try:
    while True:
        line = ser.readline()
        if line:
            try:
                text = line.decode('utf-8', errors='replace').rstrip('\r\n')
                if text:
                    print(text, flush=True)
            except:
                pass
except KeyboardInterrupt:
    pass
finally:
    ser.close()
