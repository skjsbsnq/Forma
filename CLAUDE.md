# CLAUDE.md — Mayonnaise

## Project overview

Mayonnaise is a C++20 UI library that renders everything itself on top of OpenGL 3.3 Core and
looks and feels like modern macOS (the "Liquid Glass" design language: translucent blurred
materials, soft layered shadows, large continuous corner radii, vibrancy, short spring
animations). The MVP targets Linux (X11 + Wayland via SDL3) with all platform code isolated in
`src/platform/`. The public API is a retained-mode widget tree with a fluent builder API
(`mayo::VStack({...}).spacing(12)`) designed so a Hello World fits in ~15 lines.

## Build, run, test

```sh
# configure + build (library, applications, tests)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# debug build (enables KHR_debug GL output)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j

# ASan + UBSan build (the suppression file hides GL-driver leaks, see Gotchas)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMAYONNAISE_SANITIZE=ON && cmake --build build-asan -j
LSAN_OPTIONS=suppressions=$PWD/tools/lsan.supp MAYO_SCREENSHOT=/tmp/x.png ./build-asan/applications/photos/photos

# run the applications (binaries are named as installed in /usr/bin)
./build/applications/photos/photos <images or folders...>
./build/applications/video_player/video_player [media files...]  # library gallery without files; needs FFmpeg dev libs
./build/applications/task_manager/taskmanager                     # Linux only
./build/applications/finder/finder [folder]                       # Linux only
./build/applications/terminal/terminal [directory]                # Linux only
./build/applications/system_settings/systemsettings [panel]       # Linux + GIO; changes the real desktop!
./build/applications/calculator/calculator
./build/applications/music/music [files or folders...]            # needs FFmpeg dev libs; GIO adds MPRIS

# Arch packages (split: library + one package per app, plus mayo-apps-git)
cd packaging/arch && makepkg -si   # builds whatever is committed

# tests
ctest --test-dir build --output-on-failure

# headless-ish visual check: render N frames, save a PNG of the window, exit
MAYO_SCREENSHOT=/tmp/shot.png MAYO_SCREENSHOT_FRAMES=10 ./build/applications/photos/photos
```

Useful environment variables:
- `MAYO_SCREENSHOT=<file.png>` / `MAYO_SCREENSHOT_FRAMES=<n>` / `MAYO_SCREENSHOT_DELAY=<s>` — save the first window (`MAYO_SCREENSHOT_WINDOW=last`: the newest, e.g. a Settings window) after n frames (and at least s seconds, default 0.4) and quit. Screenshot runs never write settings.
- `MAYO_DEMO=viewer:N|zoom:N|favorite|open` (photos) / `MAYO_DEMO=seek:<s>|captions|settings|home|all|recent`
  (video_player) — drive the apps at startup for screenshots. The finder takes comma-separated steps, including file operations
  (`MAYO_DEMO="list,select:a.txt,duplicate:a.txt,wait:0.3,undo"`, full list in its main.cpp); point it at a
  scratch folder, and it never launches other programs in demo mode.
  Terminal: `MAYO_DEMO=<command>` types a command, `MAYO_DEMO_SCROLL=<points>` then swipes into the history.
  Calculator: `MAYO_DEMO=scientific|settings|<keys>` (keys are typed: `MAYO_DEMO='12+3*4='`).
  Music: comma-separated `recent|artists|albums|songs|favorites`, `album:<n>`, `play:<n>`, `search:<text>`, `queue`,
  `settings`, `pause`, `seek:<s>`, `favorite` (set `XDG_CACHE_HOME` too: scans write the tag cache).
  System settings: `MAYO_DEMO=<panel>[:action]` (actions: `wifi:other`, `displays:keep`, `appearance:fonts`,
  `keyboard:add`, `keyboard:shortcut`, `datetime:zones`); `SETTINGS_DESKTOP=kde|gnome` overrides the detected desktop; screenshot runs and `SETTINGS_READ_ONLY=1` never write
  GSettings, D-Bus or nmcli/pactl state (Mutter is only asked to *verify* display configs).
- `MAYO_VERBOSE=2` — also print frames per second and CPU ms per frame (find widgets that never stop requesting
  frames, or slow draws).
- `MAYO_TEST_SCENE=1|text` — renderer / typography test scenes instead of the widget tree.
- `MAYO_APPEARANCE=dark|light` — force appearance at startup.
- `MAYO_SCALE=<float>` — override the UI scale factor (HiDPI testing).
- `MAYO_NO_TRANSPARENCY=1` — opaque, square-cornered windows (X11 without compositor).
- `MAYO_VERBOSE=1` — print GL version/renderer and whether KHR_debug output is on.
- `SDL_VIDEO_DRIVER=x11|wayland` — force SDL backend.
- `MAYO_EGL_ALL_VENDORS=1` — load every glvnd EGL vendor (see the hybrid-GPU gotcha).
- `XDG_CONFIG_HOME=<dir>` — where settings live (`<dir>/mayonnaise/<app>/settings.conf`); point it at a scratch
  directory when testing so runs don't touch your real preferences.

CMake options: `MAYONNAISE_BUILD_APPS` (ON; the old `MAYONNAISE_BUILD_EXAMPLES` still works), `MAYONNAISE_BUILD_TESTS` (ON),
`MAYONNAISE_USE_SYSTEM_DEPS` (OFF), `MAYONNAISE_SANITIZE` (OFF).

## Architecture map

```
include/mayonnaise/   public headers only (no SDL/GL/FreeType includes)
  mayonnaise.hpp      umbrella header
  geometry/color      Vec2, Size, Rect, Insets, CornerRadii, Color
  theme.hpp           Appearance, semantic colors, metrics, TextStyle type scale
  animation.hpp       Animated<T>, easing curves, springs
  painter.hpp         public 2D drawing API used by every widget (wraps Renderer2D)
  widget.hpp          Widget node base class, Element (type-erased handle), WidgetHandle CRTP modifiers
  layout.hpp          VStack/HStack/ZStack/Spacer/Divider
  widgets/*.hpp       controls and containers
  app.hpp window.hpp  App (event loop, appearance) and Window
src/platform/         SDL3 wrapper: windows, GL context, events -> PlatformEvent, clipboard, cursors, DPI
src/render/           GL RAII wrappers, Renderer2D (batched SDF quads), blur, FreeType fonts + glyph atlas, Painter impl
src/core/             widget tree, layout, event dispatcher (hover/press/capture/focus), animation clock, App/Window
src/widgets/          widget implementations
src/theme/            light/dark themes, type scale
shaders/              GLSL, embedded at build time (cmake/Embed.cmake -> generated .cpp)
assets/fonts/         InterVariable.ttf (OFL) + license, embedded at build time
third_party/          vendored glad (generated GL 3.3 core + KHR_debug) and stb single headers
applications/         finder, terminal, task_manager, photos, video_player, system_settings, calculator, music; each has data/
                      (.desktop + icon) and is its own CMake install component
packaging/arch/       split PKGBUILD
tests/                doctest unit tests (geometry, color, theme, layout, event dispatch, scroll physics,
                      terminal screen, calculator engine, KConfig reader/writer)
```

Frame flow: `App::run` blocks in `SDL_WaitEventTimeout` until an event or the next animation /
timer deadline → platform events are translated to `PlatformEvent` → `Window` routes them through
the `EventDispatcher` (hit test, hover enter/leave, pointer capture, focus, keyboard) → widgets
mutate state and call `invalidate()`/`invalidateLayout()` → for each dirty window: layout
(measure/arrange, cached) → draw into an offscreen scene FBO via `Painter`/`Renderer2D` (materials
flush the batch and blur the scene so far) → overlays (menus, tooltips) → blit to the default
framebuffer → swap. If any animation is still running, another frame is scheduled.

## Design rules

- All colors, radii, spacing and fonts come from `mayo::Theme` (`theme().colors.*`, `theme().metrics.*`,
  `TextStyle`). No hardcoded colors or sizes inside widgets.
- Every widget handles hover, pressed, focused (accent focus ring), and disabled states, in both light and dark.
- No Apple proprietary assets: no SF Pro, SF Symbols, or Apple images. Text uses Inter (OFL);
  icons are vector-drawn with Painter primitives (`src/widgets/icons.cpp`).
- Prefer fewer widgets that look excellent over many rough ones.

## API rules

- Namespace `mayo`, single umbrella header `<mayonnaise/mayonnaise.hpp>`.
- Widgets are cheap value-like handles (`std::shared_ptr` to a node) — capture them in lambdas and mutate later.
- Sensible defaults; every widget looks correct with zero configuration.
- Modifiers return `*this` for chaining (`.padding() .frame() .foreground() .background() .cornerRadius() .disabled() .tooltip()`).
- Callbacks are `std::function`; no inheritance for normal use. Custom widgets subclass `mayo::Widget` and draw with `mayo::Painter`.
- Public headers must never include SDL, GL, glad, FreeType or stb headers. Use pimpl / forward declarations.
- Users never touch raw OpenGL; `mayo::Canvas` exposes the Painter for custom drawing.

## Coding conventions

- C++20, RAII for every GL/SDL/FreeType resource, no naked `new`/`delete`, `std::span`/`std::string_view` where useful.
- `PascalCase` types, `camelCase` functions/variables, `m_` private members, `kConstant` constants.
- Format with `.clang-format`; lint with `.clang-tidy`. Must compile warning-free with `-Wall -Wextra -Wpedantic` on GCC 12+/Clang 15+.
- Small focused files; comment the *why*, not the *what*.
- After each milestone: build, run the relevant example (take a `MAYO_SCREENSHOT`), run tests, commit.

## Current status

- [x] M1 Skeleton — CMake, deps, SDL3 window + GL 3.3 core context
- [x] M2 Renderer2D — instanced SDF quads, shadows, gradients, lines/arcs, rounded clips, dual-Kawase backdrop blur
- [x] M3 Text — FreeType + InterVariable (wght/opsz), glyph atlas w/ 1/4 px subpixel positioning, type scale, wrapping
- [x] M4 Core — widget tree, measure/arrange stacks, event dispatcher (hover/capture/focus/Tab/overlays), Animated<T>
- [x] M5 Basic widgets — Label, Button (5 styles), Toggle, Checkbox, Slider, Divider, vector icons
- [x] M6 Window chrome — borderless transparent window, traffic lights, hit-test drag/resize, unified Toolbar
- [x] M7 Materials — glass Sidebar in SplitView, Menu / Picker / Popover, Form cards, Canvas, App::after timers
- [x] M8 TextField (+Search, secure), SegmentedControl, ProgressBar (+indeterminate, spinner), ScrollView (glide, momentum, rubber band, overlay bars)
- [x] M9 Polish — HiDPI verified (MAYO_SCALE=2), accent colours, gallery + settings_app, README + screenshots,
      Clang/GCC warning-free, ASan/UBSan clean, KHR_debug clean on Wayland + X11, idle CPU ~0, install() verified

Verified on: Arch-based (CachyOS) with GCC 16 / Clang 22, Mesa radeonsi, Wayland + XWayland. Not yet verified
on a stock Ubuntu 24.04 / Fedora 40 machine (README lists the packages; the FetchContent path for SDL3,
FreeType and doctest was verified here with `-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON`).

Known issues / next steps:
- No kerning or shaping (HarfBuzz; system HarfBuzz is common, make it optional).
- Interactive behaviours (drag/resize via hit test, real touchpad momentum feel, IME) were only smoke-tested
  by hand-free runs; unit tests cover the physics and editing logic, not the feel.
- No client-side window shadow (would need an input-shape margin; SDL can't express it).
- Popups have a fade-in but no fade-out (they're removed immediately on dismiss).
- Menus open below their button; macOS pop-up buttons overlay the selected row on the button.
- Single-line TextField only; no multi-line text editor.
- Video player: subtitles embedded in MKV/MP4 aren't read yet (external .srt/.vtt only); no ASS styling.
- No project LICENSE file yet (the PKGBUILD says LicenseRef-custom).
- System settings example: no Printers, Online Accounts, Sharing or Color panels; displays can't be dragged to
  rearrange (side-by-side packing only); the shortcut recorder can't capture keys the compositor grabs
  (Super+arrows) because SDL doesn't expose the keyboard-shortcuts-inhibit protocol; passwords are changed
  with `passwd`, not in the app. Clicking through the panels was only verified via MAYO_DEMO screenshots.
  On KDE Plasma: global shortcuts open Plasma's own editor (kcm_keys); the KDE panels have not been run on a real
  Plasma session yet (only `SETTINGS_DESKTOP=kde` on GNOME), so key names/defaults may need adjusting there.
- Music example: no gapless playback, ReplayGain or user playlists (only Favorite Songs); covers cached per album
  are not refreshed when the embedded picture changes (delete `~/.cache/mayonnaise/music/covers`).

## Decisions

- **glad and stb are vendored** in `third_party/` (glad pre-generated for GL 3.3 core + `GL_KHR_debug`)
  instead of FetchContent: glad needs Python to generate, and both are tiny. SDL3, FreeType and doctest
  use `find_package` first and fall back to `FetchContent`; `MAYONNAISE_USE_SYSTEM_DEPS=ON` makes the
  system packages mandatory.
- **InterVariable.ttf** (one file, `wght` + `opsz` axes) instead of several static weights: smaller binary,
  and the optical-size axis gives Inter Display shapes for large titles automatically.
- **Window move/resize uses `SDL_SetWindowHitTest`** so the compositor performs the interactive move/resize
  (the only reliable way on Wayland, where clients cannot position themselves).
- **Transparent window surface** so client-drawn rounded window corners work; disabled with
  `MAYO_NO_TRANSPARENCY=1`. No client-side window shadow (would need an input-shape margin SDL can't express).
- **Absolute layout coordinates**: every widget's `frame()` is in window points; `ScrollView` re-arranges its
  content on scroll (measure results are cached, so this is cheap) instead of a transform stack.
- **Default stack alignment is leading/top** (not SwiftUI's center) because forms and lists are the common case;
  `.align(Alignment::Center)` centers both axes.
- **Premultiplied alpha everywhere** in the renderer (needed for a correct alpha channel on a transparent window).
- **Root content fills the window.** The window always arranges its content at the full content rect, so a root
  stack's `.align()` positions its children in the window and a root `.background()` covers the window.
- **Two clips per primitive**: the plain intersection of every pushed rect plus the innermost rounded rect.
  Cheap (no stencil, no batch breaks) and correct for the UI case of rect clips nested in rounded panels.
- **Overlays (menus, popovers) are measured at their ideal size** (`{inf, inf}`), then clamped to the window, so
  expanding content (spacers) doesn't stretch them to full width.
- **Fonts beyond Inter** (`<mayonnaise/font.hpp>`): `Font::family` (0 = Inter) and `Font::italic`;
  `systemFontFamily("monospace")` resolves installed files through a dlopen()ed libfontconfig (no build
  dependency). Missing bold/italic faces are synthesized (outline embolden / shear). A character no
  registered family has triggers a fontconfig `charset=` lookup once and the found font joins the fallbacks,
  so CJK, symbols and Nerd Font icons render anywhere (Finder file names too). Inter's tracking applies only
  to Inter glyphs.
- **Terminal example**: `Screen` (applications/terminal/screen.cpp) is a UI-free xterm emulator tested by
  tests/test_terminal_screen.cpp; `Pty` reads on a worker thread with back-pressure (4 MB) and never blocks
  the UI on writes; the view parses at most ~8 ms of output per loop pass (64 KB slices) and continues after the
  next frame, so floods don't freeze input (it used to swallow the whole 4 MB, ~100 ms, at once). Runs of plain
  ASCII skip the byte-by-byte parser, and reflow moves cells instead of copying them. `TerminalView` snaps
  cells to whole device pixels and draws box drawing, blocks, braille and powerline glyphs as geometry so they
  join between cells; its history scrolls by points (`ScrollPhysics`), drawing a partial row at the edge.
- **Applications and packaging**: `applications/CMakeLists.txt` `mayonnaise_app(<binary> ID <desktop id> SOURCES ...)`
  builds an app, passes the desktop id as `MAYO_DESKTOP_ID` to `mayo::App(name, desktopId)` (it becomes the
  Wayland app_id / X11 class, so GNOME matches the window to the .desktop file and icon), and installs binary,
  `data/<id>.desktop` and `data/<id>.svg` as install component `<binary>`. The PKGBUILD is one
  `cmake --install --component` per package. App icons are generated by `tools/make_app_icons.py`
  (superellipse squircles, original artwork); settings directories keep the old app names.
- **Video player library**: `library.cpp` scans `$XDG_VIDEOS_DIR` + added folders on a thread, `Prober` workers
  read duration and a thumbnail (freedesktop thumbnail cache first, else one decoded frame ~10% in, PNG-encoded
  with FFmpeg into `~/.cache/mayonnaise/video_player/`). Subtitles (`subtitles.cpp`) are SRT/WebVTT files named
  like the video; non-UTF-8 files are read in the Windows code page of the user's language (1254 for Turkish).
- **System Settings example** (applications/system_settings) talks to GNOME through GIO (the only example with a
  GLib dependency, skipped without it). GLib's default main context runs on a background thread (`gx::start`);
  GSettings objects, D-Bus subscriptions and async calls are created on the UI thread, so their callbacks fire
  on that thread and hop over with `App::post` — panels never see GLib's thread. `gx::Key` wraps one key and
  never aborts on a missing schema (g_settings_new does); `ui::toggleRow/pickerRow/sliderRow` bind a control to
  a key both ways. Wi-Fi/wired state comes from `nmcli -t` refreshed by `nmcli monitor`, but joining a
  protected network uses NetworkManager's D-Bus API so passwords never appear in argv. Sound uses `pactl -f
  json` + `pactl subscribe` (tiny JSON reader in json.cpp). Display changes are applied *temporarily* and made
  persistent only after "Keep Changes" (20 s auto-revert). Each complex panel is a model object owned by its
  page (`ui::page(sections, model)`), so callbacks capture a raw `this` without shared_ptr cycles.
- **Settings** (`mayo::Settings`, `App::settings()`): one `key = value` file per app in
  `~/.config/mayonnaise/<app>/settings.conf`, written atomically 0.5 s after the last change and on exit.
  The App stores `appearance` (system/light/dark), `accent` (system or #RRGGBB) and `window.<name>.*` for
  windows created with `autosaveName`. Apps are named explicitly (`App app("finder")`); `App()` uses the
  executable name. Every example has a Settings window (Ctrl+,) built from `AppearanceSettings(app)` plus
  its own `FormSection`s.
- **Follows the desktop**: appearance via `SDL_GetSystemTheme` + `SDL_EVENT_SYSTEM_THEME_CHANGED` (portal
  `color-scheme`); the accent colour via the portal's `accent-color`, read with a dlopen()ed libdbus (no build
  dependency) at startup and whenever a window gains focus, then snapped to the macOS palette
  (`nearestAccent`).
- **Finder example** keeps all views on one shared `Browser` state (folder, entries, selection, hooks into
  `main.cpp`); `FileView` holds the Finder selection rules, rubber band, type-select, inline rename and drag
  and drop so the icon grid, list rows and browser columns only provide geometry. Column view nests vertical
  ScrollViews in a horizontal one (scroll chaining handles the axes).
- **Photos example** uses custom `Widget` subclasses (grid, viewer, page container) rather than new library
  widgets: it doubles as the reference for writing custom widgets. A custom detail container opts into
  running under the toolbar with `extendsIntoTitleBar()`.
- **Scroll physics are shared** (`<mayonnaise/scroll_physics.hpp>`): ScrollView, Sidebar, the terminal's history
  and Quick Look all use `ScrollPhysics` (wheel glide, 1:1 touchpad tracking, macOS rubber band
  `c * (1 - x/dim)^2` with c = 0.55, momentum, critically damped bounce). Custom widgets that scroll themselves
  should use it too instead of clamping offsets, or they feel dead next to everything else. A lone vertical
  ScrollView bounces even when its content fits; one nested in another vertical scroller passes the gesture on.
- **Touchpad gestures are latched** by the dispatcher: every event of a gesture goes to the widget that took the
  first one (until the fingers lift, or a 0.25 s pause without end events), so content moving under the pointer
  or reaching an edge never hands half a swipe to another scroller.
- **System Settings on KDE Plasma**: `gx::Key` can be backed by a `KeyBackend` (text values + a declared type), so
  the same `ui::` rows bind to KConfig entries (`kcfg::key`, `flagKey`, `multiKey`, `appliedKey` in kconfig.hpp).
  `kcfg::write` edits the user's file in place (keeps comments and unknown entries, `[A][B]` nested groups,
  KConfig escapes), emits `org.kde.kconfig.notify.ConfigChanged` (nested group names joined by `\x1d`) and pokes
  services that don't watch their files (`Reload`: KWin reconfigure, kxkb reloadConfig, PowerDevil refreshStatus,
  screensaver configure, KGlobalSettings notifyChange). Files are watched with GFileMonitor. Writes that need
  Plasma's tools (colour schemes, wallpapers, cursors, icons, global themes) go through `appliedKey`. Displays
  reuse the Mutter panel with a KScreen backend (`kscreen-doctor -j` / `output.<name>.mode.<id> ...`; KScreen
  stores changes itself, the keep prompt reverts by re-applying). Pointer settings are KWin InputDevice D-Bus
  properties (KWin persists them). The KDE panels live in panels_kde.cpp; shared helpers in panels_shared.hpp.
  Only the KConfig layer is tested (tests/test_kconfig.cpp); the Plasma services were not available on the dev
  machine (GNOME), so the KDE panels were verified with `SETTINGS_DESKTOP=kde` screenshots and a fake
  `kscreen-doctor`.
- **Music example** (applications/music): `AudioPlayer` (player.cpp) is one FFmpeg decode thread feeding
  `mayo::AudioOutput` ~0.4 s ahead, position = last sample time − queued; accurate seeks drop samples before the
  target. The library (library.cpp) caches tags in `$XDG_CACHE_HOME/mayonnaise/music/library.tsv` and embedded
  covers in `covers/` (the attached picture bytes as they are, also used as MPRIS artUrl). Albums are grouped by
  album artist, or by folder when there is none (compilations stay together). Covers load lazily on one worker
  when first drawn. Progress/end-of-track checks run on a 0.25 s `App::after` timer only while playing. MPRIS
  (mpris.cpp, only with GIO) runs GDBus on its own GMainContext thread; calls hop to the UI with `App::post`.
- **Calculator example** (applications/calculator): `Engine` is UI-free and tested (tests/test_calculator_engine.cpp):
  it keeps the whole expression and evaluates with shunting-yard; after an operator the display shows only
  what that operator settles (2 + 3 × shows 3). Separators come from the C++ locale (`numpunct`). The view is one
  custom widget (keys are a grid that scales with the window); characters arrive through text input so every
  keyboard layout and the numpad work, and `Window::setSize` switches between the two keypad sizes.
- **Extra components beyond the brief**: `Form`/`FormSection`/`FormRow` (System Settings cards), `Picker`,
  `ToolbarGroup`, `ButtonStyle::Glass`, `App::after/post` timers, `MAYO_SCREENSHOT` capture for visual checks.

## Gotchas

- **Traffic lights sit 24 pt in** (`trafficLightInset`): at 18 they overlapped the sidebar panel's rounded corner.
- **Settings files keep tabs raw** (only `\n` and `\\` are escaped): hand-written test configs need real tabs.
- **Hybrid GPUs load every EGL vendor.** glvnd's libEGL maps the NVIDIA stack (~90 MB RSS, can wake the dGPU)
  even when Mesa drives the display. `Platform::init` restricts EGL to Mesa on Wayland when all connected
  screens hang off non-NVIDIA GPUs, and unsets the variable once the context exists so launched programs
  don't inherit it. PRIME offload (`__NV_PRIME_RENDER_OFFLOAD`) and `MAYO_EGL_ALL_VENDORS=1` opt out.
- **GPU memory on integrated GPUs is system RAM** (GTT, not in RSS: read `drm-resident-gtt` in
  /proc/<pid>/fdinfo). Instances stream through a ring buffer (`Renderer2D::flush`); orphaning the whole
  buffer on every batch made Mesa allocate and cache a buffer per batch (~20 MB more with many thumbnails).
  The glyph atlas is cleared through an FBO instead of uploading zeros.
- **ImageData drops its CPU pixels after upload** unless `setRetainPixels(true)`; `scaledToFit()` must run
  before the first draw. Dynamic images (`create()`/`update()`) keep theirs.
- **Idle heap trimming**: after 1 s of idleness following work, `App::run` calls `malloc_trim(0)` so memory
  freed by decoder threads (glibc per-thread arenas) goes back to the OS.
- **One FreeType face per (weight, optical size)**, resized on demand; a face per pixel size cost ~70 KB each.
- **Finder file operations are real.** Test them in a scratch folder; items trashed on another file system
  go to `$topdir/.Trash-$uid` (e.g. /tmp/.Trash-1000 for tmpfs), as the spec requires.
- **Screenshot runs never write settings** (read-only `Settings`), but they do read them: use `XDG_CONFIG_HOME`
  for reproducible shots. Live system-theme switching was implemented against SDL's event but not exercised
  by an automated run (it would flip the desktop's theme).
- **Ctrl+W only closes a window when nothing handled it** (`EventDispatcher::keyDown` returns whether the key
  was used): in a terminal it deletes a word. `Window::onCloseRequested` can veto user-initiated closes.
- **Alt vs AltGr**: SDL reports both as Alt. The terminal defers Alt+key to the next loop iteration and lets
  a following text-input event win when it is a different character (AltGr+Q = @ on Turkish keyboards).
- **Bracketed paste swallows newlines**: shells don't execute a pasted "\n", so demo commands are pasted and
  then a "\r" is typed.
- **Designated initializers and WindowOptions**: new fields need a default member initializer (`{}`), or GCC's
  `-Wmissing-field-initializers` fires in every example using `{.title = ...}`.
- **Windows popped up by screenshot runs take keyboard focus**, so typing elsewhere during a run lands in the
  app (type-to-select in the finder). Don't trust a surprising selection in a screenshot without a trace.

- **XDG_CONFIG_HOME also moves dconf.** GSettings reads `$XDG_CONFIG_HOME/dconf/user`, so pointing it at a
  scratch dir makes the system settings example show schema defaults (e.g. "No input sources"). Leave it alone
  for that example; screenshot runs are read-only anyway. Test writes with `GSETTINGS_BACKEND=memory`.
- **hostnamed, timedated and localed are D-Bus activated**: synchronous calls must allow auto-start
  (`G_DBUS_CALL_FLAGS_NONE`), or they fail while the service is idle. power-profiles-daemon may be masked
  (it is on the dev machine); the Power panel then hides Power Mode.
- **GNOME 50 has no brightness setter on the session bus** (gsd-power's Screen interface is gone; the Shell
  one only dims). The Displays panel writes the backlight through logind's `Session.SetBrightness`.
- **`pactl -f json` prints "Invalid ASCII character" to stderr** for some property values even with
  LC_ALL=C.UTF-8; stdout is still valid JSON.
- **Range-for over a temporary's member** (`for (auto& x : parse(t).array())`) dangles in C++20 — GCC's
  -Wdangling-reference caught it; bind the parsed value to a local first.

- **Task manager sampling**: one pass over /proc takes ~10 ms for ~400 processes, done on a worker thread;
  the uid comes from stat()ing /proc/<pid> (cheaper than parsing status), names from argv[0] because comm is
  truncated to 15 chars — but Chromium/Electron rewrite argv[0] into one space-separated title, so cut at
  the first space. Disk stats use whole devices that have /sys/block/<dev>/device (partitions, zram, loop and
  dm would double count). Never let a demo path call kill(): the quit demo only opens the confirmation.

- **Video player threading**: demux, video decode and audio decode threads; seeking bumps a serial number so
  every stage drops stale packets/frames without locks across stages. Audio is the clock
  (`audioEnd - queuedSeconds`), the wall clock is used for silent files. Frames are converted to RGBA on the
  decode thread and uploaded with `ImageData::update` (copy-free move); 4K is downscaled to 2560 wide.
- **Hardware decoding**: `setupHardware` picks the first device type (VAAPI, CUDA, Vulkan, VDPAU) that a decoder
  for the stream supports — the default AV1 decoder is libdav1d (software only), so it iterates decoders for
  one with a hw config. `get_format` falls back to software when the GPU refuses a stream (seen with a
  portrait VP9 file); `decoderDescription()` reports it. `extra_hw_frames` covers the display queue, and the
  frame on screen is kept alive so its surface isn't reused under the texture.
- **Zero-copy**: VAAPI → `av_hwframe_map(DRM_PRIME)` → two single-plane EGL images (R8/GR88, or R16/GR1616 for
  10-bit) bound with `glEGLImageTargetTexture2DOES`. Needs an EGL context, so SDL is forced to EGL on X11 too
  (`MAYO_GLX=1` restores GLX; then playback uses copy-back). EGL/DRM constants are defined locally so the build
  needs no EGL headers. The EGLImage is destroyed right after binding; the texture keeps the buffer.
- **YUV matrix guessing**: untagged streams use BT.709 at ≥720p, BT.601 below (like mpv). FFmpeg's own test
  encodes are BT.601 but untagged, so their colour bars look slightly off — that's the file, not the shader
  (verified against FFmpeg's bt709 conversion within ±2).
- **TSan on the video player** reports races inside FFmpeg's frame threads and PipeWire (uninstrumented
  atomics); none involve MediaPlayer's own state.
- **Widget::setOpacity is a no-op when unchanged** — fades call it every frame from draw(), and an
  unconditional invalidate() kept the window rendering forever.
- **Full screen is applied asynchronously by the compositor**; track the requested state yourself instead of
  reading `isFullscreen()` right after `setFullscreen()`.

- **File dialogs answer on SDL's terms** (portal reply during event pumping, zenity on a helper thread);
  `App::openFiles` always hops back with `post()`, which is why `post()`/`after()` are mutex-protected.
- **ImageData is CPU-only until first draw.** Decode on any thread, create textures only on the UI thread;
  viewer-sized images call `setRetainPixels(false)` so the CPU copy is freed after upload.
- **stb_image ignores EXIF orientation**; `render::decodeImage` parses the JPEG APP1 tag itself. Phone photos
  come out sideways without it.
- **ThreadSanitizer** reports dozens of races inside uninstrumented glib (SDL_Init → IBus/DBus) and Mesa's
  shader threads. Filter for our symbols (Loader, post, decode) — those paths are clean.
- **There is no "drag left the window" event**, so the drop highlight times out 0.3 s after the last
  DROP_POSITION.

- **Measure overlays with infinite space.** Proposing the window size made a popover containing a `Spacer`
  as wide as the window.
- **Nested clips must not inherit the outer radius onto the inner rect** — it rounded the corners of a text
  clip and ate the first glyph ("Lock" rendered as "' ock"). That's why there are two clip rects per primitive.
- **`theme()` works without an App** (falls back to the default light theme) so layout tests can run headless.
- **WidgetHost::time() is the wall clock outside frames.** Animations started from input handlers after an
  idle period would otherwise start "in the past" (the last frame time) and finish instantly.
- **Screenshot mode needs a time floor** (`MAYO_SCREENSHOT_DELAY`): without vsync throttling (hidden/occluded
  windows) N frames can finish before `App::after` timers fire.
- **Backdrop materials ignore the colour opacity stack by construction** (they output the blurred scene), so
  group opacity is passed separately (`clip2.y`) — otherwise fading popovers pop in at full opacity.
- **Dragging files out (Wayland)**: SDL3 can't start a drag, so `src/platform/wayland_drag.cpp` dlopen()s
  libwayland-client and puts its own wl_seat / wl_pointer on SDL's wl_display, on a separate event queue
  dispatched after every SDL pump. Its wl_data_device exists only during a drag: Mutter sends the clipboard
  selection (and drops) to just one data device per client, so a permanent second one starved SDL's — pasting
  from other apps silently returned "" (own copies still worked, SDL short-circuits them). The extra wl_pointer only exists to learn the serial of the button
  press, which `start_drag` requires. Widgets call `host()->startFileDrag(paths)` while the button is down (Finder
  does it when the pointer leaves the window); the compositor keeps the release, so a DragEnded platform event
  makes the dispatcher send the captured widget an onPointerUp far outside the window. Not available on X11.
- **Open in Terminal** passes the folder explicitly (`--working-directory` etc.): Ptyxis, Console and GNOME
  Terminal are single-process, so a new window ignores the child's cwd. Forma Terminal is tried first, and its
  binary is called `forma-terminal` (the CMake target is still `terminal`): the Finder looks for it next to its
  own executable and then on PATH, before `$TERMINAL` and the other emulators. Exactly one terminal is
  launched — a machine with several installed used to get one window per program. `openTerminal()` returns why
  nothing started, so the menu item reports the failure instead of doing nothing.
- **Wheel vs touchpad on Wayland comes from the compositor**: SDL ignores `wl_pointer.axis_source` and
  `axis_stop`, so `wayland_drag.cpp`'s extra wl_pointer (bound at v5) records them. A finger source makes events
  precise with `ScrollEvent::reportsEnd`, and the stop becomes a `ScrollPhase::Ended` event (pushed onto SDL's
  queue so it lands after the gesture's last movement) that starts momentum at once. A frame that only carries
  the stop doesn't wake `SDL_WaitEventTimeout`, so the loop waits at most 8 ms while a gesture is open.
- **Wheel vs touchpad elsewhere (X11)**: SDL3 gives no device type. Deltas that aren't multiples of 1/120 notch are touchpad
  (1:1 + momentum); whole notches and high-resolution wheel fractions are a wheel (animated glide). A touchpad
  event can land on a whole value, so events within 250 ms of a touchpad event stay touchpad — per-event
  classification made single events jump 40 pt mid-gesture. Gesture end = no events for 80 ms; the fling
  velocity comes from the last 80 ms of movement only, so a finger that rested before lifting doesn't drift.
- **LeakSanitizer** reports ~311 bytes in `<unknown module>` frames: Mesa allocations in driver modules that
  `SDL_Quit` unloads, reproducible with a bare SDL3 + GL program. Run the apps with
  `LSAN_OPTIONS=suppressions=$PWD/tools/lsan.supp`.
- **Canvas callbacks get local coordinates.** Capturing the canvas' own handle in its callback to read its frame
  creates a shared_ptr cycle (a real leak ASan caught in settings_app).
- **Static installs must export the glad library** (`mayonnaise_glad`); wrapping it in `$<BUILD_INTERFACE:>`
  produced undefined `glad_gl*` symbols in installed consumers.
- **SDL hit-test DRAGGABLE regions never deliver the button press to the app** (the compositor takes over), so
  title-bar double-click-to-zoom isn't available; traffic lights and toolbar controls must report
  `isInteractive()` to stay clickable.
