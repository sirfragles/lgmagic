#!/usr/bin/env python3
import os
import struct

HIDRAW_DEVICE = "/dev/hidraw7"

# Expected payload sizes (excluding Report ID)
EXPECTED_SIZES = {
    0xFD: 20,   # 30 total
    0xF9: 29,
    0x01: 29,   # gyro/motion — try 29 first, adjust later if needed
}

BUTTON_CODES = {
    0x0000: "None",

    0x8000: "POWER",
    0x8099: "POWER2",
    0x8010: "0",
    0x8011: "1",
    0x8012: "2",
    0x8013: "3",
    0x8014: "4",
    0x8015: "5",
    0x8016: "6",
    0x8017: "7",
    0x8018: "8",
    0x8019: "9",

    0x8044: "WHEEL_PRESS",


    0x8053: "LIST",
    0x8045: "...",
    0x8002: "VOL+",
    0x8003: "VOL-",
    0x8009: "MUTE",
    0x808B: "MIC",
    0x807C: "HOME",
    0x8043: "SETTINGS",
    0x8028: "BACK",
    0x80AB: "GUIDE",
    0x805D: "STREAMING",
    0x800B: "INPUT",
    0x8098: "STB MENU",
    
    
    #0x8000: "CH+",
    0x8001: "CH-",
    
    0x8072: "RED",
    0x8071: "GREEN",
    0x8063: "YELLOW",
    0x8061: "BLUE",
    
    0x8081: "MOVIES",
    0x80B0: "PLAY",
    0x80BA: "PAUSE",

    0x8040: "UP",
    0x8041: "DOWN",
    0x8006: "RIGHT",
    0x8007: "LEFT",
    # fill as discovered
}

wheel_pos = 0

def parse_fd(data):
    global wheel_pos
    # Button + wheel parsing
    btn_bytes = data[-3:-1]  # last two bytes before wheel
    wheel = int.from_bytes([data[-1]], signed=True)
    wheel_pos += wheel
    
    btn_code = int.from_bytes(btn_bytes)
    
    name = BUTTON_CODES.get(btn_code, f"UNKNOWN_{btn_code:04X}")
    print(f"Button: {name} ({btn_code})  Wheel={wheel}")
    print(f"[0xFD] Buttons raw: {data.hex()}")

    # First 16 bytes parsing
    counter = int.from_bytes(data[1:3], 'little', signed=False)
    const_fd00 = int.from_bytes(data[3:5], 'little', signed=False)  # should always be 0xFD00

    points_of_interest = [int.from_bytes(data[i:i+2], 'big', signed=True)
                          for i in range(5, 17, 2)]
    
    # Print neatly
    print(f"{'Counter':>8}  {'Const':>6}  {'POI1':>6}  {'POI2':>6}  {'POI3':>6}  {'ACCEL_X':>6}  {'ACCEL_Y':>6}  {'ACCEL_Z':>6}")
    print(f"{counter:8}  {hex(const_fd00):>6}  " +
          "  ".join(f"{p:6}" for p in points_of_interest))
    print(f"Button: {name} ({btn_code:04X})  Wheel={wheel} Wheel Pos = {wheel_pos}")

def parse_f9(data):
    print(f"[0xF9] Extra control raw: {data.hex()}")

def parse_01(data):
    if len(data) % 2 == 0:
        values = struct.unpack("<" + "h" * (len(data)//2), data)
        print(f"[0x01] Gyro values: {values}")
    else:
        print(f"[0x01] Odd-size gyro packet: {data.hex()}")

parsers = {
    0xFD: parse_fd,
    0xF9: parse_f9,
    0x01: parse_01,
}

with open(HIDRAW_DEVICE, "rb", buffering=0) as f:
    while True:
        report_id_raw = f.read(1)
        if not report_id_raw:
            break
        report_id = report_id_raw[0]
        size = EXPECTED_SIZES.get(report_id, 15)  # fallback guess
        payload = f.read(size)
        if report_id in parsers:
            parsers[report_id](payload)
        else:
            print(f"[0x{report_id:02X}] Unknown len={len(payload)}: {payload.hex()}")

