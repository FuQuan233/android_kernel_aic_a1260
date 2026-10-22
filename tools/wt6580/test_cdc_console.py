#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the host console's actual TTY I/O using a pseudo-terminal."""

import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import termios


def read_exact(fd, count):
    result = bytearray()
    while len(result) < count:
        ready, _, _ = select.select([fd], [], [], 5)
        assert ready, "Timed out waiting for terminal I/O"
        chunk = os.read(fd, count - len(result))
        assert chunk, "Terminal closed unexpectedly"
        result.extend(chunk)
    return bytes(result)


def check_interactive_terminal():
    master, slave = pty.openpty()
    keyboard, terminal = pty.openpty()
    previous = termios.tcgetattr(terminal)
    script = Path(__file__).with_name("cdc_console.py")
    process = subprocess.Popen(
        [sys.executable, str(script), os.ttyname(slave)],
        stdin=terminal, stdout=terminal, stderr=subprocess.PIPE)
    try:
        assert select.select([process.stderr], [], [], 5)[0]
        assert b"Connected to" in process.stderr.readline()
        # A single key must reach USB without waiting for Enter. It must not
        # be echoed twice when the device sends its own echo back.
        os.write(keyboard, b"h")
        assert read_exact(master, 1) == b"h"
        assert not select.select([keyboard], [], [], 0.1)[0]
        os.write(master, b"h")
        assert read_exact(keyboard, 1) == b"h"
        assert not select.select([keyboard], [], [], 0.1)[0]
        os.write(keyboard, b"\x7f")
        assert read_exact(master, 1) == b"\x7f"
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=5) == 0
        assert termios.tcgetattr(terminal) == previous
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        for fd in (master, slave, keyboard, terminal):
            os.close(fd)


def main():
    master, slave = pty.openpty()
    script = Path(__file__).with_name("cdc_console.py")
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "serial.log"
        process = subprocess.Popen(
            [sys.executable, str(script), os.ttyname(slave), "--log", str(log)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stderr], [], [], 5)
            assert ready
            assert b"Connected to" in process.stderr.readline()
            received = b"WT6580: DIAGNOSTIC_INIT_STARTED\r\n"
            os.write(master, received)
            assert read_exact(process.stdout.fileno(), len(received)) == received
            sent = b"help\ndmesg\n"
            process.stdin.write(sent)
            process.stdin.flush()
            assert read_exact(master, len(sent)) == sent
            process.stdin.close()
            os.close(master)
            master = -1
            assert process.wait(timeout=5) == 1
            assert log.read_bytes() == received
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if master >= 0:
                os.close(master)
            os.close(slave)
    check_interactive_terminal()
    print("Host console: I/O, capture, disconnect, immediate keys, remote echo and terminal restoration passed")


if __name__ == "__main__":
    main()
