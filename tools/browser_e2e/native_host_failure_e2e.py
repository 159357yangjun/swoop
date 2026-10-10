#!/usr/bin/env python3
"""Packaged Windows GUI + real Native Messaging host + local 404 failure E2E.

Does NOT start a real browser or test toolbar extension UI.
Uses the native protocol and the *same* binaries produced by the Windows
installer/dist packaging step. No external network. No retries of add_download.
"""
from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time


class Gone(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.respond()

    def do_GET(self):
        self.respond()

    def respond(self):
        self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", "7")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(b"missing")

    def log_message(self, *_args):
        return


def host_call(host: Path, cwd: Path, obj: dict, timeout: float = 15) -> dict:
    packet = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    framed = struct.pack("<I", len(packet)) + packet
    child = subprocess.Popen(
        [str(host)], cwd=str(cwd),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        raw, err = child.communicate(framed, timeout=timeout)
    except subprocess.TimeoutExpired:
        child.kill()
        child.communicate()
        raise AssertionError("Native Host did not answer within timeout")
    if child.returncode != 0:
        raise AssertionError(
            f"Native Host rc={child.returncode}, stderr={err.decode('utf-8', errors='replace')[:500]!r}"
        )
    if len(raw) < 4:
        raise AssertionError(f"Missing framed host response (got {len(raw)} bytes)")
    length = struct.unpack("<I", raw[:4])[0]
    if length > 1024 * 1024 or len(raw) != 4 + length:
        raise AssertionError(f"Malformed Native Messaging length: prefix={length}, bytes={len(raw)}")
    answer = json.loads(raw[4:].decode("utf-8"))
    if not isinstance(answer, dict):
        raise AssertionError("Host response not an object")
    return answer


def main() -> int:
    if os.name != "nt":
        raise SystemExit("Windows-only packaged-runtime check")
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", required=True)
    args = parser.parse_args()
    dist = Path(args.dist).resolve()
    gui = dist / "idm-next.exe"
    host = dist / "idm-next-host.exe"
    if not gui.is_file() or not host.is_file():
        raise AssertionError("Packaged GUI/host executable absent")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Gone)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}/not-found.zip"
    with tempfile.TemporaryDirectory(prefix="idm-native-e2e-") as scratch:
        stdout_path = Path(scratch) / "gui.out"
        stderr_path = Path(scratch) / "gui.err"
        gui_stdout = stdout_path.open("wb")
        gui_stderr = stderr_path.open("wb")
        proc = subprocess.Popen(
            [str(gui)], cwd=str(dist), stdin=subprocess.DEVNULL,
            stdout=gui_stdout, stderr=gui_stderr
        )
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise AssertionError(f"GUI exited early, rc={proc.returncode}")
                try:
                    ping = host_call(host, dist, {"action": "ping"}, timeout=5)
                except (AssertionError, OSError):
                    ping = {}
                if ping.get("success") is True:
                    break
                time.sleep(0.7)
            else:
                raise AssertionError("Packaged GUI did not expose IPC within 45s")

            # One-shot add: retrying here may duplicate a task if acknowledgement was lost.
            add = host_call(host, dist, {
                "action": "add_download", "url": url, "filename": "not-found.zip",
                "saveDir": scratch, "queue": "", "format": ""
            })
            task_id = add.get("taskId")
            session_id = add.get("sessionId")
            if add.get("success") is not True or type(task_id) is not int or task_id <= 0:
                raise AssertionError(
                    f"GUI failed to return a real accepted task ID: {add!r}; "
                    f"GUI process exit code={proc.poll()!r}"
                )
            if not isinstance(session_id, str) or len(session_id) < 32:
                raise AssertionError(f"GUI failed to return a session identity: {add!r}")
            print(f"[PASS] real Native Host accepted exactly one task id={task_id} session_token_present")

            stale = host_call(host, dist, {
                "action": "get_task_status", "taskId": task_id,
                "sessionId": "00000000-0000-4000-8000-000000000000"
            })
            if stale.get("success") is not False:
                raise AssertionError(f"Stale GUI session incorrectly trusted: {stale!r}")
            print("[PASS] stale session cannot query/relabel accepted task")

            deadline = time.monotonic() + 75
            last = {}
            while time.monotonic() < deadline:
                last = host_call(host, dist, {
                    "action": "get_task_status", "taskId": task_id, "sessionId": session_id
                })
                if last.get("success") is True and last.get("state") == 4:
                    if last.get("taskId") != task_id or last.get("sessionId") != session_id:
                        raise AssertionError(f"Task identity mismatch: {last!r}")
                    if not isinstance(last.get("error"), str) or not last["error"].strip():
                        raise AssertionError(f"Failure state lacks actionable reason: {last!r}")
                    # A generic nonempty failure string is not enough: the server sent HTTP 404.
                    # Verify the network layer's real status code survives engine → GUI → host.
                    if "404" not in last["error"]:
                        raise AssertionError(f"HTTP 404 reason was lost or replaced: {last!r}")
                    if (Path(scratch) / "not-found.zip").exists():
                        raise AssertionError("HTTP 404 produced a falsely completed final file")
                    print("[PASS] real engine reports HTTP 404 reason through Native Host; no final file")
                    return 0
                if last.get("success") is True and last.get("state") == 3:
                    raise AssertionError(f"HTTP 404 incorrectly became a completed download: {last!r}")
                time.sleep(0.6)
            raise AssertionError(f"Real 404 task did not reach an observable failure state: {last!r}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            gui_stdout.close()
            gui_stderr.close()
            print("[GUI STDERR TAIL]", stderr_path.read_text(errors="replace")[-1500:])
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("[FAIL] real Native Host download-failure E2E:", repr(exc), file=sys.stderr)
        sys.exit(1)
