#!/usr/bin/env python3
"""Exercise the real SDL picker through directed X11 key events, without focus changes.
Requires an X server (Xwayland works). Does not install or replace the user's portal.
"""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

x = C.CDLL('libX11.so.6')
x.XOpenDisplay.restype = C.c_void_p
x.XDefaultRootWindow.argtypes = [C.c_void_p]
x.XDefaultRootWindow.restype = C.c_ulong
x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
x.XFetchName.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_char_p)]
x.XFree.argtypes = [C.c_void_p]
x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
x.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.c_void_p]
x.XFlush.argtypes = [C.c_void_p]
x.XCloseDisplay.argtypes = [C.c_void_p]
display = x.XOpenDisplay(None)
assert display, 'No X display available'
root = x.XDefaultRootWindow(display)

class KeyEvent(C.Structure):
    _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                ('display', C.c_void_p), ('window', C.c_ulong), ('root', C.c_ulong),
                ('subwindow', C.c_ulong), ('time', C.c_ulong), ('x', C.c_int), ('y', C.c_int),
                ('x_root', C.c_int), ('y_root', C.c_int), ('state', C.c_uint),
                ('keycode', C.c_uint), ('same_screen', C.c_int), ('padding', C.c_byte * 128)]

class WindowAttributes(C.Structure):
    _fields_ = [(n, C.c_int) for n in ['x','y','width','height','border_width','depth']] + [
        ('visual',C.c_void_p),('root',C.c_ulong),('class_',C.c_int),('bit_gravity',C.c_int),
        ('win_gravity',C.c_int),('backing_store',C.c_int),('backing_planes',C.c_ulong),
        ('backing_pixel',C.c_ulong),('save_under',C.c_int),('colormap',C.c_ulong),
        ('map_installed',C.c_int),('map_state',C.c_int),('all_event_masks',C.c_long),
        ('your_event_mask',C.c_long),('do_not_propagate_mask',C.c_long),
        ('override_redirect',C.c_int),('screen',C.c_void_p)]
x.XGetWindowAttributes.argtypes = [C.c_void_p,C.c_ulong,C.POINTER(WindowAttributes)]

def find_window(parent, title):
    name = C.c_char_p()
    if x.XFetchName(display, parent, C.byref(name)) and name.value:
        match = name.value.decode(errors='replace') == title
        x.XFree(name)
        if match:
            return parent
    rr, pp, children, count = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
    if x.XQueryTree(display, parent, C.byref(rr), C.byref(pp), C.byref(children), C.byref(count)):
        ids = list(children[:count.value])
        if children:
            x.XFree(children)
        for child in ids:
            found = find_window(child, title)
            if found:
                return found
    return None

def key(window, symbol):
    for kind, mask in [(2, 1), (3, 2)]:
        event = KeyEvent(type=kind, display=display, window=window, root=root,
                         keycode=x.XKeysymToKeycode(display, symbol), same_screen=1)
        x.XSendEvent(display, window, 0, mask, C.byref(event))
    x.XFlush(display)

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='forma-picker-test-') as tmp:
    folder = Path(tmp) / 'files'
    folder.mkdir()
    (folder / 'existing.txt').write_text('keep this content')
    (folder / 'other.md').write_text('another file')
    env = dict(os.environ, SDL_VIDEO_DRIVER='x11', MAYO_SCALE='1', XDG_CONFIG_HOME=str(Path(tmp) / 'config'))
    for index, (request, keys, expected) in enumerate([
        ({'_click': True, 'filters': [{'name': '文本', 'terms': [{'type': 0, 'value': '*.txt'}]}]}, [0xff0d], 0),
        ({}, [0xff1b], 1),  # Escape cancels
        ({'directory': True}, [0xff0d], 0),  # current folder
        ({'save': True, 'name': '中文 空格.txt'}, [0xff0d], 0),
        ({'save': True, 'name': 'existing.txt'}, [0xff0d, 0xff0d], 0),
    ]):
        title = f'Forma picker smoke {os.getpid()} {index}'
        select_file = request.pop('_click', False)
        data = dict(version=1, title=title, folder=str(folder), **request)
        process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            process.stdin.write(json.dumps(data) + '\n')
            process.stdin.flush()
            deadline = time.monotonic() + 12
            window = None
            while time.monotonic() < deadline and process.poll() is None:
                window = find_window(root, title)
                if window:
                    attributes = WindowAttributes()
                    x.XGetWindowAttributes(display, window, C.byref(attributes))
                    if attributes.map_state == 2:
                        break
                    window = None
                time.sleep(.05)
            assert window, 'Picker window did not appear'
            time.sleep(.3)
            if select_file:
                key(window, 0xff50)  # Home selects first matching file
                time.sleep(.15)
            for symbol in keys:
                key(window, symbol)
                time.sleep(.15)
            output, error = process.communicate(timeout=8)
            assert process.returncode == 0, error
            result = json.loads(output)
            assert result['response'] == expected, result
            if expected == 0:
                from urllib.parse import unquote, urlparse
                path = Path(unquote(urlparse(result['uris'][0]).path))
                assert path == (folder / request['name'] if request.get('save') else folder / 'existing.txt' if select_file else folder), result
            assert (folder / 'existing.txt').read_text() == 'keep this content'
            assert not (folder / '中文 空格.txt').exists(), 'Chooser must not create saved files'
            print(f'PASS {title}')
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
x.XCloseDisplay(display)
