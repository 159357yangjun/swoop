#!/usr/bin/env python3
"""Packaged Windows Native Host + GUI HLS *without FFmpeg in the downloader*.

FFmpeg is permitted only in test fixture generation and independent output
verification. Force the C++ fallback through an explicit test-only environment
flag; no faked status or mocked GUI. Verify plaintext MPEG-TS, AES-128 with
explicit and sequence-derived IV, bad key, malformed ciphertext, non-overwrite
and absence of any finished file on failure.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from hls_ffmpeg_e2e import (
    build_fixture, host_call, poll_terminal, run, submit_once, wait_gui
)


class Server(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path.split("?", 1)[0])
        super().do_GET()

    def log_message(self, *_args):
        pass


def make_encrypted(ffmpeg: str, root: Path, name: str,
                   *, iv: str | None, number: int, duration: int) -> Path:
    p = root / name
    p.mkdir()
    key = bytes(range(16))
    (p / "key.bin").write_bytes(key)
    info = p / "key-info.txt"
    info.write_text("key.bin\n" + str(p / "key.bin") + "\n" +
                    (iv + "\n" if iv else ""), encoding="ascii")
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
         "-f", "lavfi", "-i", "testsrc2=size=160x96:rate=10",
         "-f", "lavfi", "-i", "sine=frequency=550:sample_rate=48000",
         "-t", str(duration), "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p", "-g", "10", "-c:a", "aac",
         "-b:a", "64k", "-f", "hls", "-hls_time",
         "1" if iv else "10", "-hls_playlist_type", "vod",
         "-hls_key_info_file", str(info), "-start_number", str(number),
         "-hls_segment_filename", str(p / "seg%03d.ts"),
         str(p / "index.m3u8")], timeout=45)
    playlist = p / "index.m3u8"
    text = playlist.read_text(encoding="utf-8")
    if f"#EXT-X-MEDIA-SEQUENCE:{number}" not in text:
        raise AssertionError("Fixture lacks the intended nonzero media sequence")
    if iv is None:
        # FFmpeg writes an explicit IV even with only the sequence supplied.
        # For a SINGLE segment the chosen IV is exactly the media sequence.
        # Strip the redundant IV attribute to test the spec's implicit-IV case.
        if len(list(p.glob("seg*.ts"))) != 1:
            raise AssertionError("Implicit-IV fixture must contain exactly one segment")
        text, matches = re.subn(r",IV=0x[0-9a-fA-F]{32}", "", text)
        if matches != 1:
            raise AssertionError("Cannot construct true implicit-IV fixture")
        playlist.write_text(text, encoding="utf-8")
    return playlist


def copy_fixture(root: Path, source: str, name: str) -> Path:
    destination = root / name
    shutil.copytree(root / source, destination)
    return destination / "index.m3u8"


def video_digest(ffmpeg: str, source: Path) -> str:
    args = [ffmpeg, "-v", "error", "-xerror", "-nostdin"]
    # -allowed_extensions is a HLS demuxer option. Supplying it to the
    # MPEG-TS demuxer itself fails with "Option not found" on FFmpeg 9.
    if source.suffix.lower() == ".m3u8":
        args += ["-allowed_extensions", "ALL"]
    args += ["-i", str(source), "-map", "0:v:0",
             "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
    decoded = subprocess.run(args, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=25)
    if decoded.returncode != 0:
        raise AssertionError(
            f"Independent video decode failed for {source}: "
            + decoded.stderr.decode("utf-8", errors="replace")[-1200:])
    payload = decoded.stdout
    if not payload:
        raise AssertionError(f"No decoded video frames: {source}")
    return hashlib.sha256(payload).hexdigest()


def main() -> int:
    if os.name != "nt":
        raise AssertionError("Packaged Windows GUI/Native Host are mandatory")
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", required=True)
    args = parser.parse_args()
    dist = Path(args.dist).resolve()
    gui, host = dist / "idm-next.exe", dist / "idm-next-host.exe"
    if not gui.is_file() or not host.is_file():
        raise AssertionError("Real packaged GUI and Native Host are missing")
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise AssertionError("FFmpeg and FFprobe needed for independent fixtures and verification")

    with tempfile.TemporaryDirectory(prefix="idm-hls-manual-real-") as folder:
        temp = Path(folder)
        public, downloads = temp / "media", temp / "downloads"
        public.mkdir()
        downloads.mkdir()
        build_fixture(ffmpeg, public)
        explicit_iv = "00112233445566778899aabbccddeeff"
        exp = make_encrypted(ffmpeg, public, "explicit",
                             iv=explicit_iv, number=7, duration=3)
        implicit = make_encrypted(ffmpeg, public, "implicit",
                                 iv=None, number=17, duration=2)
        bad_key = copy_fixture(public, "explicit", "bad-key")
        (bad_key.parent / "key.bin").write_bytes(b"eightbyt")
        truncated = copy_fixture(public, "explicit", "truncated")
        encrypted_segments = sorted(truncated.parent.glob("seg*.ts"))
        assert encrypted_segments, "Encrypted fixture has no segments"
        encrypted_segments[0].write_bytes(encrypted_segments[0].read_bytes()[:-1])

        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(Server, directory=str(public)))
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        output_file = (temp / "gui.stdout").open("wb")
        error_file = (temp / "gui.stderr").open("wb")
        env = dict(os.environ)
        env["IDM_HLS_TEST_FORCE_MANUAL"] = "1"
        env["QT_FORCE_STDERR_LOGGING"] = "1"
        process = subprocess.Popen([str(gui)], cwd=str(dist), env=env,
                                   stdin=subprocess.DEVNULL,
                                   stdout=output_file, stderr=error_file)
        base = f"http://127.0.0.1:{server.server_port}"
        session = None
        ids = set()

        def add(path: str, filename: str) -> tuple[int, str]:
            nonlocal session
            task, sid = submit_once(host, dist, base + "/" + path,
                                    filename, downloads)
            if task in ids or (session is not None and sid != session):
                raise AssertionError("Duplicated task or unstable GUI session")
            ids.add(task)
            session = sid
            return task, sid

        def no_stages_or_unwanted(filename: str) -> None:
            if list(downloads.rglob(".idm-next-hls-*")):
                raise AssertionError("Failed/manual HLS left private staging directories")
            if list(downloads.rglob(filename)):
                raise AssertionError("Failed HLS created a final file: " + filename)

        try:
            wait_gui(host, dist, process)
            successes = [
                ("high/index.m3u8", "plain-manual.ts", public / "high/index.m3u8"),
                ("explicit/index.m3u8", "explicit-aes.ts", exp),
                ("implicit/index.m3u8", "implicit-sequence-17.ts", implicit),
            ]
            for url, name, source in successes:
                task, sid = add(url, name)
                state = poll_terminal(host, dist, task, sid, timeout=65)
                if state.get("state") != 3:
                    raise AssertionError(f"Expected complete manual TS for {name}: {state!r}")
                paths = list(downloads.rglob(name))
                if len(paths) != 1:
                    raise AssertionError(f"Manual TS missing or duplicated: {paths!r}")
                produced = paths[0]
                if produced.stat().st_size < 1000 or produced.stat().st_size % 188:
                    raise AssertionError(f"Corrupt final TS byte count: {name}")
                run([ffprobe, "-v", "error", "-show_entries",
                     "format=duration", "-of", "json", str(produced)], timeout=15)
                if video_digest(ffmpeg, produced) != video_digest(ffmpeg, source):
                    raise AssertionError(f"Wrong video content in pure-C++ result: {name}")
                if list(downloads.rglob(".idm-next-hls-*")):
                    raise AssertionError("Manual HLS left private temp files after success")
                print(f"[PASS] real pure-C++ HLS {name} task={task} "
                      f"bytes={produced.stat().st_size} decode_sha256=match")

            for url, name, required_error in (
                ("bad-key/index.m3u8", "bad-key.ts", "密钥长度"),
                ("truncated/index.m3u8", "truncated-ciphertext.ts", "AES-128"),
            ):
                task, sid = add(url, name)
                state = poll_terminal(host, dist, task, sid, timeout=35)
                if state.get("state") != 4 or required_error not in str(state.get("error", "")):
                    raise AssertionError(f"Malformed AES stream did not fail clearly: {state!r}")
                no_stages_or_unwanted(name)
                print(f"[PASS] corrupted pure-C++ AES rejected: {name} task={task}")

            # Existing user .ts: compare exact bytes before/after, not just status.
            existing = next(downloads.rglob("plain-manual.ts"))
            before = hashlib.sha256(existing.read_bytes()).hexdigest()
            task, sid = add("high/index.m3u8", existing.name)
            state = poll_terminal(host, dist, task, sid, timeout=25)
            if state.get("state") != 4 or "已存在" not in str(state.get("error", "")):
                raise AssertionError(f"Manual TS collision was not rejected: {state!r}")
            if hashlib.sha256(existing.read_bytes()).hexdigest() != before:
                raise AssertionError("Existing manual TS was overwritten")
            if list(downloads.rglob(".idm-next-hls-*")):
                raise AssertionError("Manual collision did not clean stage")
            print("[PASS] pure-C++ no-FFmpeg collision preserved original TS")
            return 0
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            output_file.close()
            error_file.close()
            if sys.exc_info()[0] is not None:
                print("[GUI STDERR TAIL]", (temp / "gui.stderr").read_text(
                    encoding="utf-8", errors="replace")[-3500:], file=sys.stderr)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("[FAIL] Real pure-C++ HLS/AES-128 E2E:", repr(exc), file=sys.stderr)
        sys.exit(1)
