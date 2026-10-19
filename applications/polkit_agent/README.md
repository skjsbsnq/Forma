# Forma authentication agent

A standalone `libpolkit-agent-1` listener with a Forma/SDL3 password dialog, inspired by the macOS 26 Tahoe administrator authentication card. No Qt dependency. It uses the real polkit/PAM session helper; the agent never checks passwords itself. The lock illustration is original vector drawing, and the font is the project's Inter, not Apple's SF font.

## Build and preview

Dependencies: C++20 compiler, CMake, the framework's normal SDL3/FreeType dependencies, GLib/GIO and `polkit-agent-1`. Install Wayland development tools (`wayland-scanner`, included in Arch's `wayland` package) for compositor blur. The two needed protocol XML files are bundled. NextKde is optional.

```sh
cmake -S . -B build-polkit -DFORMA_POLKIT_AGENT_ONLY=ON \
  -DMAYONNAISE_USE_SYSTEM_DEPS=ON -DMAYONNAISE_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-polkit --target forma-polkit-agent -j4
build-polkit/applications/polkit_agent/forma-polkit-agent --preview --light
build-polkit/applications/polkit_agent/forma-polkit-agent --preview --dark
build-polkit/applications/polkit_agent/forma-polkit-agent --preview-error
build-polkit/applications/polkit_agent/forma-polkit-agent --preview-identities
build-polkit/applications/polkit_agent/forma-polkit-agent --preview-backdrop
```

Preview does **not** register an agent or invoke authentication. It uses fabricated text and always shows a fabricated failure when submitted. Do not enter a real password into preview. `MAYO_SCREENSHOT=/tmp/preview.png` works only in preview; daemon mode refuses framebuffer capture. These PNGs show the application's framebuffer and do not capture compositor-rendered blur. For genuine compositor validation, start `--preview-backdrop` and run `python3 applications/polkit_agent/tools/capture_preview.py /tmp/material.png`. The script requires PyGObject/Pillow, captures only the test overlap through KWin ScreenShot2, and refuses a live or ambiguous window. Outside the card the checkerboard should be sharp; inside it should be blurred.

The UI follows system appearance by default. `MAYO_APPEARANCE=light|dark` is a transient override. The 300-point width is an initial visual match to the reference proportions, not a verified macOS system metric. Longer policy/PAM messages grow the card, then scroll at 680 points so controls remain reachable. Multiple identities use a picker in the username row. Repeated PAM challenges support both hidden and visible response modes.

## NextKde material

When NextKde's `kos_bridge` effect and window appearance are enabled at startup,
the card delegates its outer clipping, outline and shadow to the compositor.
It publishes a rectangular blur region and omits the second client-side rounded
shape/border. This prevents white corner crescents where NextKde's continuous
contour differs from Forma's circular contour. Detection only reads the loaded
effects and the existing appearance configuration; it does not alter desktop
settings. Other environments retain the client-drawn 28-point card.

The opt-in `WindowOptions` dialog/material API requests `ext_background_effect_manager_v1` blur on SDL's existing `wl_surface`, and declares matching rounded geometry through `kos_surface_shape_manager_v1` where available. The client uses a separate Wayland event queue and never impersonates Quickshell. Without a blur-capable compositor, the card uses an opaque background. The effect is desktop backdrop blur, not the framework's in-window material filter. NextKde's extra Quickshell-only refraction/highlight path is intentionally not required. Fonts, lock artwork and unmeasured compositor shadows mean this is a close visual recreation, not an assertion of pixel identity.

## Install and opt-in activation

Installation adds a binary, desktop metadata, and an **unenabled** user service. It does not disable KDE's agent and does not install an XDG autostart entry.

```sh
sudo cmake --install build-polkit --component forma-polkit-agent
systemctl --user daemon-reload
```

A local-source Arch recipe is in `packaging/polkit-agent/PKGBUILD`; run `makepkg` there. Its output may be installed with `sudo pacman -U ...pkg.tar.zst`. This Arch package provides and conflicts with `polkit-kde-agent`, so pacman removes the old KDE agent in the same transaction while preserving Plasma's package dependencies. It leaves the Forma service disabled. See `packaging/polkit-agent/README.md` (installed as `ARCH.md` beside this document) for activation and rollback.

Only one authentication agent can register for the login session. Before switching, discover how KDE's agent is currently launched:

```sh
systemctl --user list-units --all '*polkit*'
pgrep -af 'polkit.*authentication|polkit.*agent'
rg -n 'polkit|authentication-agent' ~/.config/autostart /etc/xdg/autostart
```

Record the current unit/autostart filename and executable before stopping it. If KDE runs as a user unit, stop that exact unit, then start Forma:

```sh
systemctl --user stop ACTUAL_KDE_AGENT_UNIT
systemctl --user start forma-polkit-agent.service
systemctl --user status forma-polkit-agent.service
journalctl --user -u forma-polkit-agent.service -n 30
```

For an XDG-autostart-launched agent, create a same-basename override under `~/.config/autostart/` with `Hidden=true`, and terminate only the identified KDE agent process. Preserve any existing override first. Do not disable polkit's system daemon. Once the new agent is confirmed for the current session, enable its user service for future graphical sessions:

```sh
systemctl --user enable forma-polkit-agent.service
```

The supplied unit assumes `/usr/bin/forma-polkit-agent`; adjust `ExecStart` with a user override for another install prefix. Enabling it also creates the `plasma-polkit-agent.service` alias used by KDE's workspace target; remove the original KDE package before enabling that alias. Its environment must include the graphical session's `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` and D-Bus address; KDE normally imports them. The agent remains hidden until requested, serializes concurrent requests, handles requester cancellation, and retries registration after polkit restarts. Registration success/failure is logged only when the status changes, so `journalctl` distinguishes a registered listener from retries or conflicts. Registration failure (including another existing agent) never opens a fake password window.

## Rollback

```sh
systemctl --user disable --now forma-polkit-agent.service
```

Then restart the exact KDE unit recorded above, or restore/remove only the XDG override created during activation and launch the recorded KDE authentication-agent executable. Logging out and back in restores XDG autostart once its override is removed. Package removal alone does not restore a manually disabled KDE agent; restore that launch path first. No switch or service installation is performed by the build or preview commands.

## Validation and security boundaries

Full repository checks: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`. The backend test uses real GObject listener/tasks and session signals with PAM execution suppressed; it covers queued cancellation, repeated prompts, visible responses, failure/retry, successful completion, shutdown and stale async discovery. It does not claim a live PAM or daemon-restart integration test. The dialog test checks response clearing before callbacks, disabled submission, challenge changes and long-message layout.

Response text is cleared before UI callbacks, backend response buffers are overwritten after delivery, and secure fields overwrite retained text on clear/destruction. SDL input events, PAM/GLib internals, allocator copies during editing and operating-system memory are outside this best-effort erasure guarantee. No password is logged, written to settings, or explicitly copied to the clipboard. Secure fields block copy/cut; paste is supported. Desktop window activation/focus is compositor policy; an unsolicited Wayland request may require the user to click the card.
