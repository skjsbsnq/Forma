#!/usr/bin/env python3
"""Linux desktop regression: repeated real UI lifecycles with synthetic 2560x1600 pixels.
Pass the resident_fixture executable; no real screen, clipboard, or user config is read.
"""
import fcntl
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
driver = os.environ.get('SHOT_TEST_DRIVER', 'x11')
if driver == 'x11' and not os.environ.get('DISPLAY'):
    print('SKIP: X11/Xwayland display required')
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix='forma-shot-resident-') as tmp:
    root = Path(tmp)
    runtime = root / 'runtime'
    runtime.mkdir(mode=0o700)
    trace = root / 'trace'
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_CACHE_HOME=str(root / 'cache'), SDL_VIDEO_DRIVER=driver, SHOT_FIXTURE_TRACE=str(trace))
    if driver == 'wayland':
        display = Path(os.environ.get('WAYLAND_DISPLAY', 'wayland-0'))
        if not display.is_absolute():
            display = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / display
        env['WAYLAND_DISPLAY'] = str(display)
    for name in ('MAYO_SCREENSHOT', 'MAYO_TEST_ACTION', 'MAYO_DEMO'):
        env.pop(name, None)
    with (root / 'log').open('w+') as log:
        process = subprocess.Popen([binary, '--background'], env=env, stdout=log, stderr=log)
        endpoint = runtime / f'forma-capture-{os.getuid()}.sock'
        client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        client.settimeout(3)
        def count(marker):
            return trace.read_text().splitlines().count(marker) if trace.exists() else 0
        def rss():
            for line in Path(f'/proc/{process.pid}/status').read_text().splitlines():
                if line.startswith('VmRSS:'):
                    return int(line.split()[1])
            return 0
        def wait_for(condition):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                assert process.poll() is None, (root / 'log').read_text()
                if condition():
                    return
                time.sleep(.02)
            raise AssertionError(f'timed out; trace={trace.read_text()}, log={(root / "log").read_text()}')
        try:
            wait_for(endpoint.exists)
            time.sleep(.2)
            cold = rss()
            inode = endpoint.stat().st_ino
            samples = []
            for cycle in range(20):
                starts, captures = count('start'), count('capture')
                client.sendto(b'F0S', str(endpoint))
                wait_for(lambda: count('capture') > captures)
                wait_for(lambda: count('start') > starts and rss() < cold + 4096)
                assert endpoint.stat().st_ino == inode, 'handoff replaced the IPC socket'
                with Path(str(endpoint) + '.lock').open('r') as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        pass
                    else:
                        raise AssertionError('resident lost its singleton lock')
                samples.append(rss())
                print(f'PASS cycle {cycle + 1}: idle RSS {samples[-1]} KiB', flush=True)
            # Deliver a new window request near the idle handoff; it must survive
            # whether the receiver has already paused or is still draining work.
            captures = count('capture')
            client.sendto(b'F0S', str(endpoint))
            wait_for(lambda: count('capture') > captures)
            time.sleep(.95)
            client.sendto(b'F2S', str(endpoint))
            wait_for(lambda: count('capture') >= captures + 2)
            wait_for(lambda: rss() < cold + 4096)
            client.sendto(b'F0Q', str(endpoint))
            assert process.wait(timeout=10) == 0
            print(json.dumps({'cold_kib': cold, 'idle_kib': samples, 'handoff_request': 'passed'}))
        finally:
            client.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
