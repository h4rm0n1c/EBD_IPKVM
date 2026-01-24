# main.py — Pico W bring-up/first test program for your veroboard wiring
# Inputs:  GP0..GP7  (from 74LVC245 B-side)
# Outputs: GP8..GP15 (to ULN2803 inputs -> LED sinks on ULN outputs)

import time
from machine import Pin

IN_PINS  = [0, 1, 2, 3, 4, 5, 6, 7]
OUT_PINS = [8, 9, 10, 11, 12, 13, 14, 15]

ins = [Pin(n, Pin.IN) for n in IN_PINS]
outs = [Pin(n, Pin.OUT, value=0) for n in OUT_PINS]

# Pico W "LED" is special (via CYW43). Try it, but don't depend on it.
try:
    led = Pin("LED", Pin.OUT)
except Exception:
    led = None

def read_byte():
    v = 0
    for i, p in enumerate(ins):
        v |= (p.value() & 1) << i
    return v

def write_byte(v):
    for i, p in enumerate(outs):
        p.value((v >> i) & 1)

def all_off():
    for p in outs:
        p.value(0)

def chase(delay_s=0.06):
    for i in range(8):
        write_byte(1 << i)
        time.sleep(delay_s)

last_print = time.ticks_ms()
hb = 0

while True:
    v = read_byte()

    # If your pushbutton truly drives all 8 inputs high together, this is a handy "mode switch".
    if v == 0xFF:
        chase()
    else:
        write_byte(v)

    now = time.ticks_ms()
    if time.ticks_diff(now, last_print) >= 100:
        last_print = now
        print("IN = 0x%02X (%s)" % (v, "".join("1" if (v >> b) & 1 else "0" for b in range(7, -1, -1))))
        if led is not None:
            hb ^= 1
            led.value(hb)

    time.sleep(0.005)
