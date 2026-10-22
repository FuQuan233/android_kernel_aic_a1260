#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Open the WT6580 CDC ACM debug port on a Linux host, without pyserial."""

import argparse
import os
import select
import sys
import termios
import tty


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="?", default="/dev/ttyACM0")
    parser.add_argument("--log", help="Save received bytes to this file")
    args = parser.parse_args()
    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = termios.tcgetattr(fd)
    log = None
    stdin_fd = sys.stdin.fileno()
    stdin_previous = None
    try:
        tty.setraw(fd)
        settings = termios.tcgetattr(fd)
        settings[2] |= termios.CLOCAL | termios.CREAD
        settings[4] = settings[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        if args.log:
            log = open(args.log, "ab", buffering=0)
        print(f"Connected to {args.device}. Type help or dmesg; Ctrl-C exits.",
              file=sys.stderr)
        pending = bytearray()
        if os.isatty(stdin_fd):
            stdin_previous = termios.tcgetattr(stdin_fd)
            # Send each key immediately; the device echoes and edits the line.
            # Keep ISIG so Ctrl-C still exits this program.
            tty.setcbreak(stdin_fd, termios.TCSANOW)
        inputs = [fd, stdin_fd]
        while True:
            readable, writable, _ = select.select(inputs, [fd] if pending else [], [])
            if fd in writable:
                try:
                    count = os.write(fd, pending)
                    del pending[:count]
                except BlockingIOError:
                    pass
            if fd in readable:
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    continue
                if not data:
                    raise OSError("USB serial device disconnected")
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if log:
                    log.write(data)
            if stdin_fd in readable:
                line = os.read(stdin_fd, 4096)
                if line:
                    pending.extend(line)
                else:
                    inputs.remove(stdin_fd)
    finally:
        if stdin_previous is not None:
            termios.tcsetattr(stdin_fd, termios.TCSANOW, stdin_previous)
        if log:
            log.close()
        try:
            termios.tcsetattr(fd, termios.TCSANOW, previous)
        except termios.error:
            pass
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except OSError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
