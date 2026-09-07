import uinput
import time
import numpy as np

# -----------------------------
# Create virtual mouse device
device = uinput.Device([
    uinput.REL_X,
    uinput.REL_Y,
    uinput.BTN_LEFT,
    uinput.BTN_RIGHT
])


def imu_to_mouse_from_rads(d_y, d_p, s_x=50.0, s_y=50.0):
    # Convert to relative pixels
    rel_x = int(d_y * s_x)
    rel_y = int(d_p * s_y)

    print(f"REL_X : {rel_x} REL_Y: {rel_y}")
    #return

    # Send to uinput
    device.emit(uinput.REL_X, rel_x, syn=False)
    device.emit(uinput.REL_Y, rel_y, syn=True)


prev_pitch, prev_yaw = None, None

# 20deg/s - 10 pix, threshold 5deg/s, 2pix

def imu_to_mouse_from_euler(euler, dt, s_x=50.0, s_y=50.0):
    global prev_pitch, prev_yaw

    roll, pitch, yaw = euler

    if prev_pitch is None:
        prev_pitch, prev_yaw = pitch, yaw
        return  # first frame

    # Relative angular velocity (radians per frame)
    delta_yaw   = yaw   - prev_yaw
    delta_pitch = pitch - prev_pitch

    prev_pitch, prev_yaw = pitch, yaw

    # Wrap around -pi..pi
    delta_yaw   = (delta_yaw + np.pi) % (2*np.pi) - np.pi
    delta_pitch = (delta_pitch + np.pi) % (2*np.pi) - np.pi

    delta_yaw /= dt
    delta_pitch /= dt
    imu_to_mouse_from_rads(delta_yaw, delta_pitch, s_x, s_y)
