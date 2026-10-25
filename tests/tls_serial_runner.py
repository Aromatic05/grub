#!/usr/bin/env python3
"""Inject real GRUB cancellation keys: BIOS serial or EFI console via QMP."""
import argparse
import json
import socket
import os
import selectors
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--serial-log", required=True)
parser.add_argument("--timeout", type=float, default=110)
parser.add_argument("--qmp", default="")
parser.add_argument("qemu", nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.qemu[1:] if args.qemu[:1] == ["--"] else args.qemu
if not command:
    parser.error("QEMU command required")

start = time.monotonic()
markers = {
    b"TLS_CANCEL_C_BEGIN": bytes([3]),  # Ctrl-C
    b"TLS_CANCEL_D_BEGIN": bytes([4]),  # Ctrl-D
}
sent = set()
qmp = None
proc = None

def inject_qmp_control(control):
    global qmp
    if qmp is None:
        qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        qmp.settimeout(5)
        qmp.connect(args.qmp)
        qmp.recv(4096)  # QEMU greeting
        qmp.sendall(b'{"execute":"qmp_capabilities"}\n')
        qmp.recv(4096)
    name = "ctrl-c" if control == bytes([3]) else "ctrl-d"
    request = {"execute": "human-monitor-command",
               "arguments": {"command-line": "sendkey " + name}}
    qmp.sendall(json.dumps(request).encode() + b"\n")
    result = qmp.recv(4096)
    if b'"error"' in result:
        raise RuntimeError("QEMU rejected injected key: " + repr(result))

try:
    with open(args.serial_log, "wb") as output, \
         open(args.serial_log + ".stderr", "wb") as errors:
        proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=errors,
                                bufsize=0)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        recent = b""
        while time.monotonic() - start < args.timeout:
            events = selector.select(0.1)
            if not events and proc.poll() is not None:
                break
            for _, _ in events:
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    selector.unregister(proc.stdout)
                    break
                output.write(chunk)
                output.flush()
                recent = (recent + chunk)[-4096:]
                for marker, control in markers.items():
                    if marker in recent and marker not in sent:
                        if args.qmp:
                            inject_qmp_control(control)
                        else:
                            proc.stdin.write(control)
                            proc.stdin.flush()
                        sent.add(marker)
            if not selector.get_map() and proc.poll() is not None:
                break
        if proc.poll() is None:
            proc.kill()
            proc.wait()
            raise TimeoutError("QEMU exceeded TLS test timeout")
        if proc.returncode:
            raise RuntimeError(f"QEMU exited {proc.returncode}")
    missing = [m.decode() for m in markers if m not in sent]
    if missing:
        raise AssertionError(f"serial cancellation markers not received: {missing}")
except (OSError, RuntimeError, TimeoutError, AssertionError) as exc:
    print("TLS_SERIAL_RUNNER_ERROR", exc, file=sys.stderr)
    sys.exit(1)
finally:
    if qmp is not None:
        qmp.close()
    if proc is not None and proc.poll() is None:
        proc.kill()
        proc.wait()
print("TLS_CANCEL_KEYS_INJECTED", len(sent))
