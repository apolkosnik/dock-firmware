#!/usr/bin/env python3
"""Generate GunCon 3 test vectors with an independent encoder.

This is the gun-side (encrypting) direction of the GunCon 3 report cipher,
written independently of the firmware decoder in src/usb_host/guncon3_protocol.cpp.
Its key table was extracted from a separate source (RPCS3's 256-entry table),
not from the firmware's copy.

Output lines (hex, space separated):
  V <key:8> <report:15> <plain:13>   valid report; decoder must accept and return plain
  C <key:8> <report:15>              valid report with one byte of 0..13 changed; must be rejected
  R <key:8> <report:15> <0|1>        random bytes; 1 if the checksum is valid

Usage: gen_vectors.py <seed> <count>
"""

import random
import sys

KEY_TABLE = bytes.fromhex(
    "91fd4c8b20c17c095814f6005255bf4175c01330b5d0698589bbd688bc73188d"
    "58ab3d985cf248e9ac9f7a0c7c25d8ffdc7d08dbbc188c1dd63c35e12c148e64"
    "8339b0e44ef7517ba813ace943c008250e15c4209313f5c348cc471cc520de60"
    "55eea040b4e77495b046ecf0a5b823c80406fc28cbf8172c251ccb18e36c8085"
    "dd7e09d9bc198f1dd43d37e12f158d640604fd29cffa142e251fc918e36d8184"
    "803bb1e54df75178a913ade980c10b2593fc4d8923c27c0b5915f6015055bf81"
    "75c31031b5d3698489bad689bd70198e58a83d9b5df049e8ad9d7a0d7e24dafc"
    "0d14c5239111f5c04bcd441cc521df6154eda281b7e57494b047eef1a5bb21c8"
)
assert len(KEY_TABLE) == 256

FIXED_KEY = [0x01, 0x12, 0x6F, 0x32, 0x24, 0x60, 0x17, 0x21]


def checksum_byte(d, key):
    """Byte 13 that makes bytes 0..12 of an encrypted report pass the check."""
    v = (key[7] + d[0] - d[1] - d[2]) & 0xFF
    v = (v ^ d[3]) & 0xFF
    v = (v + d[4] + d[5]) & 0xFF
    v ^= d[6]
    v ^= d[7]
    v = (v + d[8] + d[9] - d[10] - d[11]) & 0xFF
    return v ^ d[12]


def encrypt(plain, key, d14):
    d = list(plain) + [0, d14]
    ko = (((((key[1] ^ key[2]) - key[3] - key[4]) ^ key[5]) + key[6] - key[7]) ^ d14) & 0xFF
    ki = 0
    for i in range(13):
        b = d[i]
        for _ in range(3):
            bk = KEY_TABLE[ko]
            ki += 1
            kr = key[ki]
            if ki == 7:
                ki = 0
            if bk & 3 == 0:
                b = (b + bk + kr) & 0xFF
            elif bk & 3 == 1:
                b = (b - bk - kr) & 0xFF
            else:
                b = b ^ bk ^ kr
            ko = (ko + 1) & 0xFF
        d[i] = b
    d[13] = checksum_byte(d, key)
    return d


def hx(b):
    return bytes(b).hex()


def random_plain(rng):
    p = [rng.randrange(256) for _ in range(13)]
    # Bias some reports towards the axis edge cases.
    r = rng.randrange(8)
    if r == 0:
        p[5], p[6] = 0x00, 0x80  # Y = INT16_MIN
    elif r == 1:
        p[3], p[4] = 0x00, 0x80  # X = INT16_MIN
    elif r == 2:
        p[5], p[6] = 0xFF, 0x7F  # Y = INT16_MAX
    return p


def main():
    seed = int(sys.argv[1])
    count = int(sys.argv[2])
    rng = random.Random(seed)
    out = []
    for n in range(count):
        key = FIXED_KEY if n % 2 == 0 else [rng.randrange(256) for _ in range(8)]
        plain = random_plain(rng)
        rep = encrypt(plain, key, rng.randrange(256))
        out.append("V %s %s %s" % (hx(key), hx(rep), hx(plain)))

        bad = list(rep)
        pos = rng.randrange(14)
        bad[pos] = (bad[pos] + rng.randrange(1, 256)) & 0xFF
        out.append("C %s %s" % (hx(key), hx(bad)))

        rnd = [rng.randrange(256) for _ in range(15)]
        ok = 1 if checksum_byte(rnd, key) == rnd[13] else 0
        out.append("R %s %s %d" % (hx(key), hx(rnd), ok))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
