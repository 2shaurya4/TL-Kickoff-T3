"""Move one SO-101 servo a small amount, in raw encoder counts.

Direct register access: no calibration file, no policy, no AI. Reads
Present_Position, adds your delta, writes Goal_Position.

    python motor_jog.py                      # list positions, move nothing
    python motor_jog.py shoulder_pan 100     # nudge +100 counts (~8.8 deg)
    python motor_jog.py gripper -150         # nudge the other way
    python motor_jog.py --release            # torque off, arm goes limp
"""

import argparse
import time

from lerobot.motors import Motor, MotorNormMode
from lerobot.motors.feetech import FeetechMotorsBus

from port_of import resolve
PORT = resolve()
COUNTS_PER_REV = 4096

MOTORS = {
    "shoulder_pan": Motor(1, "sts3215", MotorNormMode.RANGE_M100_100),
    "shoulder_lift": Motor(2, "sts3215", MotorNormMode.RANGE_M100_100),
    "elbow_flex": Motor(3, "sts3215", MotorNormMode.RANGE_M100_100),
    "wrist_flex": Motor(4, "sts3215", MotorNormMode.RANGE_M100_100),
    "wrist_roll": Motor(5, "sts3215", MotorNormMode.RANGE_M100_100),
    "gripper": Motor(6, "sts3215", MotorNormMode.RANGE_0_100),
}

parser = argparse.ArgumentParser()
parser.add_argument("motor", nargs="?", choices=list(MOTORS), help="which joint to move")
parser.add_argument("delta", nargs="?", type=int, help="counts to move, 4096 = one full turn")
parser.add_argument("--release", action="store_true", help="disable torque on every motor")
args = parser.parse_args()

bus = FeetechMotorsBus(port=PORT, motors=MOTORS)
bus.connect(handshake=False)

present = bus.sync_read("Present_Position", normalize=False, num_retry=3)
volt = bus.sync_read("Present_Voltage", normalize=False, num_retry=3)
load = bus.sync_read("Present_Load", normalize=False, num_retry=3)
curr = bus.sync_read("Present_Current", normalize=False, num_retry=3)
temp = bus.sync_read("Present_Temperature", normalize=False, num_retry=3)

print(f"{'joint':15s} {'id':>2s} {'raw':>5s} {'deg':>7s} {'V':>5s} {'load':>5s} {'mA':>6s} {'C':>3s}")
for name, raw in present.items():
    print(
        f"{name:15s} {MOTORS[name].id:2d} {raw:5d} {raw * 360 / COUNTS_PER_REV:7.1f}"
        f" {volt[name] / 10:4.1f}V {load[name]:5d} {curr[name] * 6.5:6.0f} {temp[name]:3d}"
    )

if args.release:
    bus.disable_torque()
    print("\ntorque disabled on all motors")
elif args.motor and args.delta is not None:
    start = present[args.motor]
    target = max(0, min(COUNTS_PER_REV - 1, start + args.delta))

    # Gentle: ramp the acceleration and cap speed before anything moves.
    bus.write("Acceleration", args.motor, 10, normalize=False)
    bus.write("Goal_Velocity", args.motor, 300, normalize=False)
    bus.enable_torque(args.motor)
    bus.write("Goal_Position", args.motor, target, normalize=False)
    print(f"\n{args.motor}: {start} -> {target}")

    time.sleep(1.0)
    reached = bus.read("Present_Position", args.motor, normalize=False)
    print(f"landed at {reached} (error {reached - target:+d} counts)")
    print("torque is still ON and holding. Run with --release to let go.")
else:
    print("\nnothing moved. Pass a motor name and a delta to jog it.")

bus.disconnect(disable_torque=False)
