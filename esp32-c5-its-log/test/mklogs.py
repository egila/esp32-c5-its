#!/usr/bin/env python3
# Write synthetic log files for run-tests.sh (no real capture data).
# usage: mklogs.py [<output directory>]

import os
import struct
import sys

MAGIC = b'\xaa\x55\x55\xaa'
TIME, ITS, GPS, INFO, VERSION = 0, 1, 2, 254, 255

BOOT_TEXT = (b'ESP-ROM:esp32c5\r\n'
             b'rst:0x1 (POWERON),boot:0x18 (SPI_FAST_FLASH_BOOT)\r\n'
             b'I (25) boot: ESP-IDF bootloader\r\n')


def record(pkt_type, body, time_us):
  # struct Header (log-data.h): type, reserved 0, body size, timestamp
  return struct.pack('<BBHq', pkt_type, 0, len(body), time_us) + body


def session(time_s, its_count, gps=False, time_records=True):
  t = time_s * 1000000
  recs = [record(VERSION, bytes([1, 1, 0, 0]), t),
          record(INFO, b'synthetic test log\n', t)]
  if time_records:  # TIME records are not written to SD logs
    recs.append(record(TIME, b'', t))
  if gps:
    recs.append(record(GPS, struct.pack('<6i', 480000000, 110000000,
                                        500000, 450000, 1000, 9000000), t))
  for i in range(its_count):
    recs.append(record(ITS, bytes((i + j) & 0xff for j in range(60)),
                       t + (i + 1) * 1000))
  return recs


def usb(recs):
  return b''.join(MAGIC + r for r in recs)


def with_unknown_type(recs):
  # a type log-cvt does not know, before the last record
  recs.insert(-1, record(3, bytes(20), 1001500))
  return recs


def main():
  out = sys.argv[1] if len(sys.argv) > 1 else '.'
  files = {
    # boot text before the first magic is normal in a USB log
    'clean.usb': BOOT_TEXT + usb(session(1, 10, gps=True)),
    'clean.sd': b''.join(session(1, 10, gps=True, time_records=False)),
    # reset after a magic was sent: boot text follows the magic
    'reset-after-magic.usb': (BOOT_TEXT + usb(session(1, 10)) + MAGIC +
                              BOOT_TEXT + usb(session(1, 10))),
    # capture starts with a fragment ending in aa 55 55, so the first
    # magic found overlaps the real one
    'overlap.usb': b'\x12\x34' + MAGIC[:3] + usb(session(1, 10)),
    'unknown-type.usb': BOOT_TEXT + usb(with_unknown_type(session(1, 2))),
  }
  for name, data in files.items():
    with open(os.path.join(out, name), 'wb') as f:
      f.write(data)


if __name__ == '__main__':
  main()
