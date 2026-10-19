#!/usr/bin/env python3
"""Capture only the known preview/checkerboard overlap through KWin ScreenShot2.
Requires python-gobject and Pillow. Start forma-polkit-agent --preview-backdrop first.
Refuses to capture live authentication or pixels outside the preview background.
"""
import os
import sys
import threading
import time
from gi.repository import Gio, GLib
from PIL import Image

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

def call(path, interface, method, args):
    values = bus.call_sync('org.kde.KWin', path, interface, method, args, None,
                         Gio.DBusCallFlags.NONE, 3000, None).unpack()
    return values[0] if values else None

def windows(title):
    result = []
    for match in call('/WindowsRunner', 'org.kde.krunner1', 'Match', GLib.Variant('(s)', (title,))):
        info = call('/KWin', 'org.kde.KWin', 'getWindowInfo', GLib.Variant('(s)', (match[0][2:],)))
        if info.get('caption') == title and info.get('resourceClass') == 'io.github.skjsbsnq.Forma.PolkitAgent':
            info["match_id"] = match[0]
            result.append(info)
    return result

pairs = [(dialog, background) for dialog in windows('Authentication')
         for background in windows('Forma Material Test Pattern') if dialog['pid'] == background['pid']]
if len(pairs) != 1:
    sys.exit('Start exactly one --preview-backdrop instance; refusing ambiguous/live capture.')
dialog, background = pairs[0]
for window in (background, dialog):
    call('/WindowsRunner', 'org.kde.krunner1', 'Run', GLib.Variant('(ss)', (window['match_id'], '')))
# KWin maps/fades newly activated windows asynchronously.
time.sleep(1)
x, y = int(dialog['x']) - 40, int(dialog['y']) - 40
width, height = int(dialog['width']) + 80, int(dialog['height'] + .999) + 80
if not (x >= background['x'] and y >= background['y'] and
        x + width <= background['x'] + background['width'] and
        y + height <= background['y'] + background['height']):
    sys.exit('Center the preview over its checkerboard; refusing to capture unrelated desktop pixels.')
read_fd, write_fd = os.pipe()
chunks = []
def read_pipe():
    try:
        while True:
            chunk = os.read(read_fd, 65536)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        os.close(read_fd)
reader = threading.Thread(target=read_pipe, daemon=True)
reader.start()
fds = Gio.UnixFDList.new()
handle = fds.append(write_fd)
os.close(write_fd)
try:
    result, _ = bus.call_with_unix_fd_list_sync(
        'org.kde.KWin.ScreenShot2', '/org/kde/KWin/ScreenShot2', 'org.kde.KWin.ScreenShot2', 'CaptureArea',
        GLib.Variant('(iiuua{sv}h)', (x, y, width, height,
                     {'native-resolution': GLib.Variant('b', True)}, handle)),
        None, Gio.DBusCallFlags.NONE, 5000, fds, None)
finally:
    fds = None
reader.join(5)
if reader.is_alive():
    sys.exit('Screenshot pipe did not close.')
info = result.unpack()[0]
# QImage RGBX8888/RGBA8888 have RGBA byte order on every platform.
if info['format'] not in (16, 17):
    sys.exit(f"Unsupported QImage pixel format: {info['format']}")
image = Image.frombytes('RGBA', (info['width'], info['height']), b''.join(chunks),
                        'raw', 'RGBA', info['stride'])
output = sys.argv[1] if len(sys.argv) > 1 else '/tmp/forma-polkit-material.png'
image.save(output)
print(output)
