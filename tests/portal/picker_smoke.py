#!/usr/bin/env python3
"""Exercise the real SDL picker through directed X11 input events, without focus changes.
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

def key(window, symbol, state=0):
    if state & 4:
        modifier = KeyEvent(type=2, display=display, window=window, root=root,
                            keycode=x.XKeysymToKeycode(display, 0xffe3), same_screen=1)
        x.XSendEvent(display, window, 0, 1, C.byref(modifier))
    for kind, mask in [(2, 1), (3, 2)]:
        event = KeyEvent(type=kind, display=display, window=window, root=root,
                         keycode=x.XKeysymToKeycode(display, symbol), state=state, same_screen=1)
        x.XSendEvent(display, window, 0, mask, C.byref(event))
    if state & 4:
        modifier.type = 3
        modifier.state = state
        x.XSendEvent(display, window, 0, 2, C.byref(modifier))
    x.XFlush(display)

class ButtonEvent(C.Structure):
    _fields_ = KeyEvent._fields_[:13] + [('button', C.c_uint), ('same_screen', C.c_int),
                                       ('padding', C.c_byte * 128)]

class MotionEvent(C.Structure):
    _fields_ = KeyEvent._fields_[:13] + [('is_hint', C.c_char), ('same_screen', C.c_int),
                                       ('padding', C.c_byte * 128)]

def click(window, px, py, button=1):
    # SDL button events use the most recently reported pointer position.
    motion = MotionEvent(type=6, display=display, window=window, root=root,
                         x=px, y=py, same_screen=1)
    x.XSendEvent(display, window, 0, 64, C.byref(motion))
    x.XFlush(display)
    time.sleep(.05)
    for kind, mask in [(4, 4), (5, 8)]:
        event = ButtonEvent(type=kind, display=display, window=window, root=root,
                            x=px, y=py, button=button, same_screen=1,
                            state=0 if kind == 4 else 1 << (7 + button))
        x.XSendEvent(display, window, 0, mask, C.byref(event))
    x.XFlush(display)
    time.sleep(.15)

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='forma-picker-test-') as tmp:
    folder = Path(tmp) / 'files'
    folder.mkdir()
    (folder / 'existing.txt').write_text('keep this content')
    (folder / 'other.md').write_text('another file')
    # Use core mouse events so directed XSendEvent clicks reach SDL without moving
    # the user's real pointer or changing input focus through XTest.
    env = dict(os.environ, SDL_VIDEO_DRIVER='x11', SDL_VIDEO_X11_XINPUT2='0',
               MAYO_SCALE='1', XDG_CONFIG_HOME=str(Path(tmp) / 'config'))
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

    # Reuse one isolated settings directory across picker processes. Verify the actual
    # selected URI after UI events, so a moving sort arrow alone cannot pass the test.
    ordered = Path(tmp) / 'ordered'
    ordered.mkdir()
    for filename, size, modified in [('a-small.txt', 1, 100), ('m-medium.zip', 20, 200), ('z-large.pdf', 100, 300)]:
        path = ordered / filename
        path.write_bytes(b'x' * size)
        os.utime(path, (modified, modified))
    (ordered / '.hidden.txt').write_text('hidden')
    prefs = Path(env['XDG_CONFIG_HOME']) / 'mayonnaise/forma-file-picker/settings.conf'

    def settings():
        return dict(line.split(' = ', 1) for line in prefs.read_text().splitlines()
                    if ' = ' in line and not line.startswith('#'))

    def run_case(label, actions, expected=None, request=None):
        title = f'Forma picker regression {os.getpid()} {label}'
        data = dict(version=1, title=title, folder=str(ordered), **(request or {}))
        process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=env)
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
            assert window, f'{label}: Picker window did not appear'
            time.sleep(.3)
            actions(window)
            if expected:
                key(window, 0xff50)  # Home selects the first item in visual order
                time.sleep(.15)
            key(window, 0xff0d if expected else 0xff1b)
            output, error = process.communicate(timeout=8)
            assert process.returncode == 0, error
            result = json.loads(output)
            assert result['response'] == (0 if expected else 1), (label, result)
            if expected:
                from urllib.parse import unquote, urlparse
                path = Path(unquote(urlparse(result['uris'][0]).path))
                assert path == ordered / expected, (label, result)
            print(f'PASS {label}')
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    # Coordinates are window points for the fixed 960x640, scale-1 fixture.
    run_case('remember-list', lambda w: click(w, 730, 56))
    assert settings()['view'] == 'list'
    for label, column, expected, sort_by, ascending in [
        ('name-descending', 260, 'z-large.pdf', 'name', False),
        ('name-ascending', 260, 'a-small.txt', 'name', True),
        ('modified-descending', 600, 'z-large.pdf', 'modified', False),
        ('modified-ascending', 600, 'a-small.txt', 'modified', True),
        ('size-descending', 750, 'z-large.pdf', 'size', False),
        ('size-ascending', 750, 'a-small.txt', 'size', True),
        ('kind-ascending', 850, 'z-large.pdf', 'kind', True),
        ('kind-descending', 850, 'a-small.txt', 'kind', False),
    ]:
        run_case(label, lambda w, column=column: click(w, column, 94), expected)
        assert settings()['sortBy'] == sort_by
        assert settings()['sortAscending'] == ('true' if ascending else 'false')
        if label == 'size-descending':
            run_case('restore-sort', lambda w: None, 'z-large.pdf')

    def show_hidden(w):
        click(w, 260, 94)  # name ascending
        click(w, 430, 430, 3)  # blank area, below the short listing
        key(w, 0xff54)  # highlight the context menu's hidden-files action
        key(w, 0xff0d)
        time.sleep(.2)

    run_case('list-context-hidden', show_hidden, '.hidden.txt')
    assert settings()['showHidden'] == 'true'
    run_case('restore-hidden', lambda w: None, '.hidden.txt')
    run_case('ctrl-h-hides', lambda w: key(w, ord('h'), 4), 'a-small.txt')
    assert settings()['showHidden'] == 'false'

    def grid_hidden(w):
        click(w, 686, 56)
        click(w, 430, 430, 3)
        key(w, 0xff54)
        key(w, 0xff0d)
        time.sleep(.2)

    run_case('grid-context-hidden', grid_hidden, '.hidden.txt')
    assert settings()['view'] == 'icons'
    run_case('restore-icons-and-hidden', lambda w: None, '.hidden.txt')
x.XCloseDisplay(display)
