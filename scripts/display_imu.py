#!/usr/bin/env python3
import evdev
from evdev import InputDevice, ecodes
import numpy as np
import json
import csv
import argparse
import math
import threading


import draw_cube
import uinput_mouse

from ahrs.filters import Madgwick
from ahrs.common.orientation import q2euler

MODULO = 2**16
COUNTS_PER_0p02 = 256
T_UNIT = 0.02

class LPF:
    def __init__(self, alpha=0.2):
        self.alpha = alpha
        self.prev = np.zeros(3)  # [gx, gy, gz]

    def filter(self, gyro):
        """
        Apply low-pass filter to gyro vector.
        gyro: np.array([gx, gy, gz]) in rad/s
        """
        self.prev = self.alpha * gyro + (1 - self.alpha) * self.prev
        return self.prev

R_align = np.array([
    [ 0,  -1,  0],   # new X = old Y
    [ -1,  0,  0],   # new Y = old X
    [ 0,  0, -1]    # new Z = - old Z
])

def find_imu_device():
    for path in evdev.list_devices():
        dev = InputDevice(path)
        if "IMU" in dev.name:
            print(f"Using device: {dev.name} ({path})")
            return dev
    raise RuntimeError("No IMU evdev device found")

def load_calibration(filename):
    with open(filename, "r") as f:
        calib = json.load(f)
    calib["accel"]["matrix"] = np.array(calib["accel"]["matrix"])
    calib["accel"]["bias"] = np.array(calib["accel"]["bias"])
    calib["gyro"]["bias"] = np.array(calib["gyro"]["bias"])
    calib["gyro"]["scale"] = np.array(calib["gyro"]["scale"])
    return calib

def apply_calibration(accel, gyro, calib):
    a = np.array(accel, dtype=float)
    g = np.array(gyro, dtype=float)
    a_corr = calib["accel"]["matrix"].dot(a - calib["accel"]["bias"])
    g_corr = (g - calib["gyro"]["bias"]) * calib["gyro"]["scale"]
    return a_corr, g_corr

def accel_to_quat(a):
    ax, ay, az = a / np.linalg.norm(a)
    # roll & pitch from accel
    roll  = math.atan2(ay, az)
    pitch = math.atan2(-ax, math.sqrt(ay*ay + az*az))
    # yaw cannot be resolved without mag → set 0
    cy, sy = math.cos(0.0/2), math.sin(0.0/2)
    cp, sp = math.cos(pitch/2), math.sin(pitch/2)
    cr, sr = math.cos(roll/2),  math.sin(roll/2)
    # quaternion [w,x,y,z]
    return np.array([
        cr*cp*cy + sr*sp*sy,
        sr*cp*cy - cr*sp*sy,
        cr*sp*cy + sr*cp*sy,
        cr*cp*sy - sr*sp*cy
    ])


def read_events(args):
    dev = find_imu_device()

    accel = [
    dev.absinfo(ecodes.ABS_X).value,
    dev.absinfo(ecodes.ABS_Y).value,
    dev.absinfo(ecodes.ABS_Z).value,
    ]
    gyro = [
        dev.absinfo(ecodes.ABS_RX).value,
        dev.absinfo(ecodes.ABS_RY).value,
        dev.absinfo(ecodes.ABS_RZ).value,
    ]
    print("Initial state:", accel, gyro)

    calib = load_calibration(args.calib) if args.calib else None
    writer = None
    if args.csv:
        csvfile = open(args.csv, "w", newline="")
        writer = csv.writer(csvfile)
        writer.writerow(["counter", "dt", "ax", "ay", "az", "gx", "gy", "gz"])

    counter = 0
    last_counter = None

    madgwick = Madgwick(sampleperiod=T_UNIT, beta=0.1)
    gyro_filter = LPF()
    q = None

    print("Press Ctrl+C to stop...")
    for event in dev.read_loop():
        if event.type == ecodes.EV_ABS:
            if event.code == ecodes.ABS_X: accel[0] = event.value
            elif event.code == ecodes.ABS_Y: accel[1] = event.value
            elif event.code == ecodes.ABS_Z: accel[2] = event.value
            elif event.code == ecodes.ABS_RX: gyro[0] = event.value
            elif event.code == ecodes.ABS_RY: gyro[1] = event.value
            elif event.code == ecodes.ABS_RZ: gyro[2] = event.value
        elif event.type == ecodes.EV_MSC and event.code == ecodes.MSC_SERIAL:
            counter = event.value
        elif event.type == ecodes.EV_SYN:
            dt = None
            if last_counter is not None:
                delta = (counter - last_counter) % MODULO
                dt = (delta / COUNTS_PER_0p02) * T_UNIT
            last_counter = counter

            if writer:
                writer.writerow([counter, dt, *accel, *gyro])
                continue

            if calib:
                a_corr, g_corr = apply_calibration(accel, gyro, calib)
                a_corr = R_align @ a_corr
                g_corr  = R_align @ g_corr
                g_corr *= (math.pi/180)
                if args.mouse:
                    gyro_filt = gyro_filter.filter(g_corr)
                    print(f"dt={dt if dt else 0:.5f}s | gyro_filt ={gyro_filt}")
                    uinput_mouse.imu_to_mouse_from_rads(-gyro_filt[2], -gyro_filt[1], s_x=30.0, s_y=30.0)

                if args.ahrs and dt:
                    #madgwick.Dt = dt

                    if q is None:
                        q = accel_to_quat(a_corr)

                    q = madgwick.updateIMU(q, gyr=g_corr, acc=a_corr)
                    draw_cube.update(q)
                    rads = q2euler(q)
                    euler = np.degrees(rads)
                    print(f"Roll={euler[0]:+.2f}  Pitch={euler[1]:+.2f}  Yaw={euler[2]:+.2f}")
                    continue
                print(f"dt={dt if dt else 0:.5f}s | Accel={a_corr} | Gyro={g_corr}")
                continue

            print(f"dt={dt if dt else 0:.5f}s | Accel={accel} | Gyro={gyro}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--calib", help="calibration JSON file")
    parser.add_argument("--csv", help="output CSV file for raw samples")

    parser.add_argument("--ahrs", help="Display Euler angles", action='store_true')
    parser.add_argument("--cube", help="Display spinning cube", action='store_true')
    parser.add_argument("--mouse", help="AirMouse via uinput", action='store_true')

    args = parser.parse_args()

    if args.cube:
        t = threading.Thread(target=read_events, kwargs={'args':args}, daemon=True)
        t.start()

        draw_cube.start()
    else:
        read_events(args)


if __name__ == "__main__":
    main()
