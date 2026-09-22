"""Pick the servo bus port: argv[1], else $SO101_PORT, else the only serial port."""

import os
import sys

from serial.tools import list_ports


def resolve(argv_index=1):
    if len(sys.argv) > argv_index and sys.argv[argv_index].upper().startswith("COM"):
        return sys.argv.pop(argv_index).upper()
    if os.environ.get("SO101_PORT"):
        return os.environ["SO101_PORT"].upper()
    ports = [p.device for p in list_ports.comports()]
    if len(ports) == 1:
        return ports[0]
    raise SystemExit(f"Specify a port. Available: {ports or 'none'}")
