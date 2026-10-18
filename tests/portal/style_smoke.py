#!/usr/bin/env python3
"""Render actual portal components in both appearances, with read-only fixture data.
No service is registered, no permission is granted and no print/recording job starts.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

CASES = [
    'AccessDialog', 'AppChooserDialog', 'UserInfoDialog', 'ScreenChooserDialog',
    'RemoteDesktopDialog', 'ScreenshotDialog', 'InputCaptureDialog',
    'GlobalShortcutsDialog', 'DynamicLauncherDialog', 'WallpaperDialog', 'UsbDialog',
    'print', 'background', 'file-fallback',
]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--output', type=Path, help='Keep preview PNGs in this directory')
args = parser.parse_args()
binary = args.binary.resolve()
with tempfile.TemporaryDirectory(prefix='forma-style-test-') as tmp:
    target = args.output.resolve() if args.output else Path(tmp) / 'renders'
    failures = []
    for appearance in ['light', 'dark']:
        output = target / appearance
        output.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
            QT_FORCE_STDERR_LOGGING='1', QT_SCALE_FACTOR='1', FORMA_APPEARANCE=appearance,
            XDG_CURRENT_DESKTOP='FormaTest', XDG_CONFIG_HOME=str(Path(tmp) / 'config'),
            XDG_CACHE_HOME=str(Path(tmp) / 'cache'))
        for case in CASES:
            result = subprocess.run([str(binary), case, str(output / (case + '.png')), '--test'],
                env=env, capture_output=True, text=True, timeout=25)
            if result.returncode or "Binding loop" in result.stderr:
                failures.append((appearance, case, result.returncode))
                print(f'FAIL {appearance}/{case} ({result.returncode})\n{result.stderr}', flush=True)
            else:
                print(f'PASS {appearance}/{case}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} component previews failed: {failures}')
    print(f'All {len(CASES) * 2} rendering and cancellation checks passed.')
