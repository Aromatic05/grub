#!/usr/bin/env python3
"""Assert a real QEMU guest reset/shutdown through QMP, before host cleanup."""

import argparse
import json
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--expect", choices=("guest-reset", "guest-shutdown"), required=True)
    parser.add_argument("--qmp-log", required=True)
    parser.add_argument("--qemu-log", required=True)
    parser.add_argument("--bda-output")
    parser.add_argument("--timeout", type=float, default=35)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("expected QEMU executable and command-line arguments after --")

    messages = []
    match = None
    status = None
    bda = None
    with tempfile.TemporaryDirectory(prefix="a1c-qmp-", dir="/tmp") as sockdir:
        socket_path = os.path.join(sockdir, "qmp.sock")
        command += ["-qmp", f"unix:{socket_path},server=on,wait=off", "-monitor", "none"]
        with open(args.qemu_log, "wb") as logfile:
            proc = subprocess.Popen(command, stdout=logfile, stderr=subprocess.STDOUT)
            client = None
            try:
                client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection_deadline = time.monotonic() + min(args.timeout, 6)
                while True:
                    try:
                        client.connect(socket_path)
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if proc.poll() is not None or time.monotonic() >= connection_deadline:
                            raise RuntimeError("QMP socket did not become available")
                        time.sleep(0.05)
                client.setblocking(False)
                buffer = b""

                def send(message):
                    client.sendall(json.dumps(message, separators=(",", ":")).encode() + b"\n")

                def receive(wait):
                    nonlocal buffer
                    ready, _, _ = select.select([client], [], [], max(0, wait))
                    if not ready:
                        return []
                    try:
                        received = client.recv(65536)
                    except (BlockingIOError, ConnectionResetError):
                        return []
                    if not received:
                        return []
                    buffer += received
                    found = []
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        if line.strip():
                            try:
                                found.append(json.loads(line))
                            except json.JSONDecodeError:
                                pass
                    messages.extend(found)
                    return found

                receive(0.25)
                send({"execute": "qmp_capabilities", "id": "caps"})
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    for message in receive(min(0.2, deadline - time.monotonic())):
                        if (message.get("event") in ("SHUTDOWN", "RESET")
                                and message.get("data", {}).get("guest") is True
                                and message.get("data", {}).get("reason") == args.expect):
                            match = message
                            break
                    if match:
                        break
                    if proc.poll() is not None:
                        break

                if match:
                    send({"execute": "query-status", "id": "status"})
                    if args.bda_output:
                        send({
                            "execute": "human-monitor-command",
                            "arguments": {"command-line": "xp /1hx 0x472"},
                            "id": "bda",
                        })
                    deadline = time.monotonic() + 2.5
                    while time.monotonic() < deadline:
                        for message in receive(0.1):
                            if message.get("id") == "status":
                                status = message.get("return")
                            if message.get("id") == "bda":
                                bda = message.get("return")
                        if status is not None and (not args.bda_output or bda is not None):
                            break
                    if args.bda_output and isinstance(bda, str):
                        Path(args.bda_output).write_text(bda)

                # Only messages seen before issuing quit are evidence of guest behavior.
                Path(args.qmp_log).write_text(json.dumps(messages, indent=2) + "\n")
                if match and status and status.get("running") is False:
                    print("PASS: QMP guest=true reason=" + args.expect
                          + " status=" + str(status.get("status")), flush=True)
                    result = 0 if (not args.bda_output or isinstance(bda, str)) else 1
                else:
                    print("FAIL: no confirmed QMP guest=true reason=" + args.expect
                          + " before host quit; status=" + repr(status), file=sys.stderr)
                    result = 1
            except (OSError, RuntimeError, ValueError) as exc:
                print("FAIL: QMP probe: " + str(exc), file=sys.stderr)
                result = 1
                Path(args.qmp_log).write_text(json.dumps(messages, indent=2) + "\n")
            finally:
                if client is not None:
                    if proc.poll() is None:
                        try:
                            client.sendall(b'{"execute":"quit","id":"host-cleanup"}\n')
                        except OSError:
                            pass
                    client.close()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
    return result


if __name__ == "__main__":
    sys.exit(main())
