#!/usr/bin/env python3
"""
Tiny Pi-side client for the FM30 controller (USB VCP, /dev/ttyACM0).

  fm30.py status
  fm30.py freq 98.1
  fm30.py ps "MY STN" ; fm30.py rt "Now playing: ..."
  fm30.py raw "POWER 50%"

Needs: pip install pyserial
"""
import json, sys, time
import serial

PORT = "/dev/ttyACM0"


class FM30:
    def __init__(self, port=PORT):
        self.s = serial.Serial(port, 115200, timeout=1)
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def cmd(self, line):
        self.s.write((line.strip() + "\n").encode())
        while True:
            r = self.s.readline().decode(errors="replace").strip()
            if not r:
                raise TimeoutError(line)
            if r.startswith(("OK", "ERR", "{")):
                return r

    def status(self):
        return json.loads(self.cmd("STATUS"))

    def freq(self, mhz):   return self.cmd(f"FREQ {mhz}")
    def power(self, v):    return self.cmd(f"POWER {v}")
    def ps(self, text):    return self.cmd(f"PS {text[:8]}")
    def rt(self, text):    return self.cmd(f"RT {text[:64]}")


if __name__ == "__main__":
    f = FM30()
    if len(sys.argv) < 2 or sys.argv[1] == "status":
        print(json.dumps(f.status(), indent=2))
    elif sys.argv[1] == "raw":
        print(f.cmd(" ".join(sys.argv[2:])))
    else:
        print(f.cmd(" ".join(sys.argv[1:]).upper() if sys.argv[1] not in ("ps", "rt")
                    else sys.argv[1].upper() + " " + " ".join(sys.argv[2:])))
