#!/usr/bin/env python3
"""Run under dbus-run-session. Exercise the actual patched KDE service with a fake UI.
This tests the IPC boundary, URI validation, request cancellation and KDE fallback.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

service = str(Path(sys.argv[1]).resolve())
dest = 'org.freedesktop.impl.portal.desktop.kde'
path = '/org/freedesktop/portal/desktop'

def call(method, *args, object_path=path):
    return ['gdbus', 'call', '--session', '--dest', dest, '--object-path', object_path,
            '--method', method, *args]

with tempfile.TemporaryDirectory(prefix='forma-backend-test-') as tmp:
    root = Path(tmp)
    helper = root / 'picker'
    marker = root / 'mode'
    marker.write_text('success')
    helper.write_text('''#!/usr/bin/env python3
import json, pathlib, sys, time
r=json.loads(sys.stdin.readline())
p=pathlib.Path(__file__).parent
(p/'request.json').write_text(json.dumps(r))
mode=(p/'mode').read_text()
if mode=='wait': time.sleep(30)
elif mode=='cancel': print('{"version":1,"response":1}')
elif mode=='bad': print('{"version":1,"response":0,"uris":["https://example.com/"]}')
else: print('{"version":1,"response":0,"uris":["file:///tmp/forma%20test.txt"],"filterIndex":0}')
''')
    helper.chmod(0o755)
    env = dict(os.environ, FORMA_PORTAL_PICKER=str(helper), QT_QPA_PLATFORM='offscreen', XDG_CURRENT_DESKTOP='FormaTest', XDG_CONFIG_HOME=str(root/'config'))
    with (root/'service.log').open('w') as log:
        process = subprocess.Popen([service], env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                ready = subprocess.run(['gdbus','call','--session','--dest','org.freedesktop.DBus','--object-path','/org/freedesktop/DBus','--method','org.freedesktop.DBus.GetConnectionUnixProcessID',dest], capture_output=True, text=True)
                if ready.returncode == 0 and f'uint32 {process.pid}' in ready.stdout: break
                if process.poll() is not None: raise AssertionError((root/'service.log').read_text())
                time.sleep(.05)
            else: raise AssertionError('Service never registered')
            import json
            def invoke(n, method='OpenFile', options='{}'):
                return subprocess.Popen(call('org.freedesktop.impl.portal.FileChooser.'+method,
                    f'/org/freedesktop/portal/desktop/request/forma/test{n}', 'io.github.Forma.Test', '', 'Test', options),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for n, mode in enumerate(['success','cancel']):
                marker.write_text(mode)
                p=invoke(n, 'SaveFile' if n==1 else 'OpenFile')
                out, err=p.communicate(timeout=8)
                assert p.returncode == 0, err
                assert ('uint32 0' if mode=='success' else 'uint32 1') in out, out
                if mode=='success': assert 'file:///tmp/forma%20test.txt' in out, out
                print('PASS backend', mode)
            # Close reaches the helper while the D-Bus call remains pending.
            marker.write_text('wait')
            p=invoke(2)
            time.sleep(.3)
            closed=subprocess.run(call('org.freedesktop.impl.portal.Request.Close', object_path=path+'/request/forma/test2'), capture_output=True, text=True)
            assert closed.returncode == 0, closed.stderr
            out, err=p.communicate(timeout=5)
            assert 'uint32 1' in out, (out,err)
            print('PASS backend Request.Close')
            # Malformed UI results must go to the original KDE dialog, not escape to callers.
            marker.write_text('bad')
            p=invoke(3)
            time.sleep(.4)
            assert p.poll() is None, 'Invalid URI was accepted instead of falling back'
            closed=subprocess.run(call('org.freedesktop.impl.portal.Request.Close', object_path=path+'/request/forma/test3'), capture_output=True, text=True)
            assert closed.returncode == 0, closed.stderr
            out, err=p.communicate(timeout=5)
            assert 'uint32 1' in out, (out,err)
            print('PASS backend malformed URI -> KDE fallback -> cancel')
            # Load a real QML permission dialog from the service's embedded resources.
            # Cancel through Request.Close; this never grants access to anything.
            access_path = path + '/request/forma/access'
            access = subprocess.Popen(call('org.freedesktop.impl.portal.Access.AccessDialog',
                access_path, 'io.github.Forma.Test', '', 'Forma access test',
                'Read-only packaging check', 'Cancel this request without granting access.', '{}'),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                closed = subprocess.run(call('org.freedesktop.impl.portal.Request.Close', object_path=access_path),
                    capture_output=True, text=True)
                if closed.returncode == 0:
                    break
                assert access.poll() is None, access.communicate()
                time.sleep(.05)
            else:
                access.kill()
                access.communicate()
                raise AssertionError('Access QML request failed to register')
            out, err = access.communicate(timeout=5)
            assert access.returncode == 0 and 'uint32 1' in out, (out, err)
            print('PASS backend embedded AccessDialog -> Request.Close -> denied')

        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
