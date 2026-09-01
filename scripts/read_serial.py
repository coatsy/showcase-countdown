#!/usr/bin/env python3
"""Reset a unit and print its boot banner.

The banner carries the assigned fanfare voice, so this is how you collect the
data that scripts/fleet_voices.py checks.

    python scripts/read_serial.py COM3
    python scripts/read_serial.py /dev/ttyUSB0 --seconds 30
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("read_serial: pyserial not installed (pip install pyserial)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--no-reset", action="store_true", help="don't pulse RTS")
    parser.add_argument("--send", help="text to send once the device has settled")
    parser.add_argument(
        "--send-after", type=float, default=25.0, help="seconds to wait before --send"
    )
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.5) as port:
        if not args.no_reset:
            # StickC-family boards use FTDI auto-reset; without this the banner
            # has already scrolled past by the time we attach.
            port.setDTR(False)
            port.setRTS(True)
            time.sleep(0.15)
            port.setRTS(False)

        start = time.time()
        deadline = start + args.seconds
        sent = args.send is None

        while time.time() < deadline:
            chunk = port.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            if not sent and time.time() - start >= args.send_after:
                sys.stdout.write("\n>>> sending %r\n" % args.send)
                sys.stdout.flush()
                port.write(args.send.encode())
                sent = True
    return 0


if __name__ == "__main__":
    sys.exit(main())
