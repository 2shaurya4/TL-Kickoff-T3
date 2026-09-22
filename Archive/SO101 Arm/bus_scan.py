"""Diagnostic: broadcast-ping COM6 at every supported baud rate."""

from lerobot.motors import Motor, MotorNormMode
from lerobot.motors.feetech import FeetechMotorsBus

from port_of import resolve
PORT = resolve()
BAUDS = [1_000_000, 500_000, 250_000, 128_000, 115_200, 76_800, 57_600, 38_400, 19_200, 9_600]

# One placeholder motor; handshake is skipped so it is never verified.
bus = FeetechMotorsBus(port=PORT, motors={"probe": Motor(1, "sts3215", MotorNormMode.RANGE_M100_100)})
bus.connect(handshake=False)
print(f"port {PORT} open\n")

found_any = False
for baud in BAUDS:
    bus.port_handler.setBaudRate(baud)
    found = bus.broadcast_ping(num_retry=2)
    label = found if found else "-"
    print(f"{baud:>9} baud : {label}")
    if found:
        found_any = True

bus.disconnect(disable_torque=False)
print("\nservos found" if found_any else "\nno servos responded at any baud rate")
