# AsteroidOS apps and UI on the Rufus Cuff: research report

Status, later on 2026-10-28: we went with option (c). RufusOS (`port/pupui/rufus.c`) borrows the AsteroidOS layout in our own C framebuffer UI, and `rufus-ancs` is our own ANCS client (no AsteroidOS code copied). Tested since this report: advertising the ANCS solicitation does **not** make iOS list the watch in Settings; the first connection still needs a BLE app such as nRF Connect, after which the iPhone reconnects by itself.

Date: 2026-10-28. Sources were read from GitHub (AsteroidOS org), wiki.asteroidos.org, pkgs.alpinelinux.org and doc.qt.io. Anything I did not check directly is marked **unverified**.

## Summary

- AsteroidOS is now a **Qt 6** stack, not Qt 5. Upstream `asteroid-launcher`, `qml-asteroid`, `asteroid-btsyncd` and the apps build with `find_package(Qt6 ...)` / `AsteroidApp6`. Lipstick has been folded into `asteroid-launcher` ("upgraded to run on Qt 6.11"). Some upstream descriptions and the April 2026 2.1 release notes still say Qt 5. Those are out of date.
- **Alpine already packages almost the whole stack, built against Qt 6.11 and musl.** Alpine edge `testing` has asteroid-launcher, asteroid-btsyncd, asteroid-settings and 14 apps. Alpine 3.24 `community` has qml-asteroid plus the Nemo middleware (mce, timed, sensorfw, mlite, nemo-keepalive, nemo-qml-plugin-{dbus,configuration,alarms}, amber-mpris, mapplauncherd(-qt), libmce-qt, libconnman-qt, usb-moded) and Qt 6.11.1. Our main porting job is mixing edge/testing into 3.24 and writing OpenRC glue. We would not be porting middleware.
- **iPhone notifications:** `asteroid-btsyncd` already contains an ANCS client (`src/ancs.cpp`, about 440 lines with `ancs_notification.cpp`). It also has a Current Time Service client (`src/cts.cpp`). Both use BlueZ's D-Bus GATT client API, and both turn ANCS notifications into `org.freedesktop.Notifications.Notify` calls. That makes it a direct reference for our own ANCS work, licensed GPL-3.0.
- **Main risk is rendering.** With no GPU, Qt Quick has to use the software scene graph or Mesa llvmpipe. Under the software backend, 18 of the 19 stock watchfaces use `DropShadow`, `OpacityMask` or `ShaderEffect`, and those effects are silently dropped. The compositor's circle-mask shader is dropped too. I found no measured frame rates for Qt Quick software rendering at 640x480 on Cortex-A53 (**unverified**). The UI is also designed for round/square ~400px screens, not 640x480 landscape.
- **Recommendation:**
  1. Keep pupui as the kid UI. Re-implement the few app ideas worth having (timer, stopwatch, alarm, flashlight, a 2048-style game, weather card, notification list) natively in C. Use the AsteroidOS QML only as a design and behaviour reference.
  2. Use btsyncd's ANCS code as the reference for our own ANCS implementation, or reuse it outright under GPL-3.0. Adopting the AsteroidOS BLE profile (XML notification characteristic, weather, media, time) would also let AsteroidOSSync and Gadgetbridge on Android talk to the watch for free.
  3. If the grown-up mode should be a richer UI, a 1-2 day spike is the cheap way to measure. Install edge/testing `asteroid-launcher` + apps on the watch with `QT_QUICK_BACKEND=software`, then check FPS and RAM before committing to anything.

## 1. Components and apps

Licences come from the GitHub API (`/orgs/AsteroidOS/repos`). Dependencies come from each repo's `CMakeLists.txt` and QML `import` lines at HEAD. Every app below links `AsteroidApp6` (qml-asteroid) and imports `org.asteroid.controls`. `qml-asteroid` itself needs Qt6 Quick/Svg/ShaderTools/WaylandClient, QtQuick.VirtualKeyboard, QtQuick.Effects and **mapplauncherd-qt6** (the "booster" fast-launch daemon).

| Component | What it does | Language | Extra system dependencies | Licence |
|---|---|---|---|---|
| asteroid-launcher (includes lipstick) | Wayland compositor, home screen, watchfaces, notification server (`org.freedesktop.Notifications`), quick panel, screen lock, volume, BT pairing agent, first-run tutorial | C++ + QML, Qt6 WaylandCompositor | timed, mlite6, mce + mce-qt6, keepalive, dbus-glib, ngf-qt6, libsystemd (`sd_notify`), usb-moded-qt6, Qt5Compat.GraphicalEffects, Nemo.{Mce,DBus,Configuration,Ngf}, org.nemomobile.calendar | BSD (launcher), LGPL-2.1 (lipstick part), MIT (bits) |
| qml-asteroid | Shared QML controls (Application, LayerStack, IntSelector, CircularSpinner, HandWritingKeyboard…), `org.asteroid.utils`, app CMake modules | QML + C++ | mapplauncherd-qt6, Qt VirtualKeyboard | LGPL-2.1 |
| asteroid-btsyncd | BLE GATT server (battery, media, notifications, screenshot, time, weather) + ANCS and CTS clients | C++ (Qt6 Core/DBus, ~2000 lines) | BlueZ (D-Bus), org.freedesktop.Notifications (the launcher), amber-mpris (MPRIS), mlite6/dconf (weather), `org.freedesktop.timedate1` (time set) | GPL-3.0 |
| asteroid-settings | Time, date, timezone, language, BT, Wi-Fi, display, sound, watchface/wallpaper picker, nightstand, tap/tilt-to-wake, USB mode | QML + C++ | Nemo.Mce, Nemo.DBus, Nemo.Configuration, **Connman** QML, ngf-qt6, org.asteroid.settings | GPL-3.0 |
| asteroid-alarmclock | Alarms | QML | Nemo.Alarms (needs **timed** daemon), Nemo.Ngf, Nemo.DBus, Nemo.Configuration, mapplauncherd | GPL-3.0 |
| asteroid-timer | Countdown timer | QML | Nemo.Alarms (timed), Nemo.Configuration | GPL-3.0 |
| asteroid-stopwatch | Stopwatch with laps | QML + small C++ controller | Nemo.Configuration | GPL-3.0 |
| asteroid-calculator | Calculator | QML + JS | none beyond qml-asteroid | GPL-3.0 |
| asteroid-calendar | Month view and events | QML | org.nemomobile.calendar (mkcal backend), Nemo.Configuration | GPL-3.0 |
| asteroid-compass | Compass | QML | QtSensors (magnetometer) | GPL-3.0 |
| asteroid-flashlight | White screen / colour beacon | QML | Nemo.KeepAlive, Nemo.Ngf, org.asteroid.settings | GPL-3.0 |
| asteroid-hrm | Heart-rate readout | QML | QtSensors (HRM), Nemo.KeepAlive | GPL-3.0 |
| asteroid-music | Remote control for phone media | QML | Amber.Mpris (talks to the MPRIS player btsyncd exposes) | GPL-3.0 |
| asteroid-weather | Five-day forecast pushed from the phone | QML | Nemo.Configuration (reads `/org/asteroidos/weather/*` written by btsyncd) | GPL-3.0 |
| asteroid-diamonds | 2048-style game | QML | Nemo.Configuration (high score) | GPL-3.0 |
| asteroid-health + asteroid-sensorlogd | Step/HR/weight graphs; logging daemon | QML; C++ daemon | QtSensors, mlite6, sensorlogd QML plugin | GPL-3.0 |
| asteroid-toolwatch | Compass/altimeter/barometer | QML | QtSensors, Qt5Compat.GraphicalEffects | GPL-3.0 |
| asteroid-map | Offline-ish map with waypoints | QML | QtLocation, QtPositioning (GPS) | GPL-3.0 |
| asteroid-camera, -gps-test, -imu-test, -helloworld, -qmltester | Demos/tests | QML | — | GPL-3.0 |
| Assets: asteroid-icons-ion / -fonts / -sounds / -wallpapers | Icons (ionicons 3), fonts, sounds, wallpapers | — | — | MIT / Apache-2.0 / CC0 / CC BY 2.0 |

Middleware:
- **sensorfw** (sensor daemon over D-Bus, used by QtSensors). Alpine's build includes `libiiosensorsadaptor-qt6.so`, so a raw IIO accelerometer should be usable. Whether it works with our specific IIO device is **unverified**.
- **timed** (time and alarm daemon behind Nemo.Alarms).
- **mce** (display, blanking, tap-to-wake policy).
- **ngfd** (non-graphic feedback: vibration and sounds for notifications).
- **mlite/dconf** (settings store).
- **statefs**: AsteroidOS still has a `timed` fork marked "not used as of writing" and a `libiphb-timerfd` reimplementation. I found no AsteroidOS repo for statefs, and it does not appear in the current launcher deps.

Source: https://github.com/AsteroidOS (repo list), e.g. https://github.com/AsteroidOS/asteroid-launcher/blob/master/CMakeLists.txt, https://github.com/AsteroidOS/asteroid-btsyncd/blob/master/CMakeLists.txt, https://github.com/AsteroidOS/qml-asteroid.

## 2. How notifications reach the watch

**Android and desktop path.** The watch runs `asteroid-btsyncd` as a BLE peripheral. It advertises `00000000-0000-0000-0000-00a57e401d05` and exposes custom services. A phone client writes to them. The Notification Update characteristic `00009001-0000-0000-0000-00a57e401d05` takes UTF-8 XML, either `<insert><id/><an/><ai/><su/><bo/><vb/></insert>` or `<removed><id/></removed>`. `vb` (vibration) is one of `strong`/`normal`/`ringtone`/`none`. Other services cover weather (city, 5 OpenWeatherMap IDs, min/max in Kelvin), media (title/artist/album/playing plus a command characteristic), time set (6 bytes), screenshot and battery (standard 0x180F). Source: https://wiki.asteroidos.org/index.php/BLE_profiles.

btsyncd converts each insert into a D-Bus `org.freedesktop.Notifications.Notify` call. It maps `vb` to a lipstick hint (`x-nemo-feedback` = `notif_strong`, etc.), and the launcher shows the notification and asks ngfd to vibrate. Weather is stored in dconf keys `/org/asteroidos/weather/...` that the weather app and watchfaces read. Time set goes through `org.freedesktop.timedate1`, which is systemd-timedated. We don't have that on OpenRC. Sources: `src/notificationservice.cpp`, `src/weatherservice.cpp`, `src/settime.cpp` in https://github.com/AsteroidOS/asteroid-btsyncd.

Clients: AsteroidOSSync (official, Android, F-Droid), Gadgetbridge (Android, early support, no screenshots or volume), Amazfish (Sailfish/Ubuntu Touch/Flathub desktop), Buran and AsteroidOSLinux (desktop), and libasteroid (C++/QtBluetooth client library). Source: https://wiki.asteroidos.org/index.php/Synchronization_Clients.

**iPhone path.** The FAQ only says notifications "can be shared from an iPhone" (https://wiki.asteroidos.org/index.php/Frequently_Asked_Questions_(FAQ)). The answer is **ANCS, implemented inside asteroid-btsyncd**. There is no iOS app.

- `src/ancs.cpp`, `src/ancs_notification.cpp` and `src/ancs_protocol_constants.h` (ANCS service `7905F431-…`, Notification Source `9FBF120D-…`, Control Point `69D1D8F3-…`, Data Source `22EAC6E9-…`) were first committed in April 2021 and last touched June 2026 (GitHub commits API).
- Once BlueZ reports `ServicesResolved` on the connected iPhone, `BlueZManager::onServicesResolvedChanged()` calls `mAncs.searchForAncsCharacteristics()` and `mCts.searchForTimeCharacteristics()`. The ANCS code finds the remote characteristics via `GetManagedObjects`, subscribes, and on each Notification Source event writes a Get Notification Attributes request to the Control Point. It then parses the Data Source reply: title capped at 50 chars, message at 100, date. It keeps a cache of 10 notifications, maps the ANCS category to an icon, and calls `Notify`/`CloseNotification`. It suppresses vibration for notifications older than the previous session's newest timestamp for 5 s after reconnect, so the iPhone's replay of old notifications does not buzz the wrist.
- `src/cts.cpp` reads the iPhone's Current Time Service (`0x2A2B`) and sets the clock. The wiki still says time sync isn't supported on iOS, so the wiki seems to lag the code. Whether CTS actually works with current iOS is **unverified**.
- Pairing: the wiki says iOS needs a BLE app such as nRF Connect to initiate the connection, because BLE-only accessories don't show in iOS Bluetooth settings. btsyncd has plumbing for "solicit UUIDs" in its advertisement but nothing calls it, so it does not advertise ANCS solicitation. Adding the ANCS UUID to the advertisement's `SolicitUUIDs` is the usual way to get iOS to offer the device. That this would remove the need for nRF Connect is **unverified**.
- Weather and media are not available from an iPhone (no companion app). AMS (Apple Media Service) is not implemented.

This matters for our ANCS work. The logic is BlueZ-D-Bus-only, with no Nemo dependency beyond the Notify sink, so it can be read as a ~450-line spec of the edge cases. Reusing the code means GPL-3.0 for the daemon. Our C UI would only talk to it over D-Bus, so the UI itself would not need to be GPL.

## 3. Feasibility on our hardware and Alpine 3.24

**Qt version.** Qt 6. Alpine 3.24 has qt6-qtbase/qtdeclarative/qtwayland/qtsensors/qtvirtualkeyboard/qt5compat/qtshadertools/qtconnectivity/qtmultimedia **6.11.1**, which matches upstream's target (pkgs.alpinelinux.org).

**Alpine packaging** (aarch64, checked 2026-10-28 on https://pkgs.alpinelinux.org/packages):
- 3.24 community: `qml-asteroid 2.1_git20260512`, `mce 1.116.4` (+`mce-openrc`), `timed 3.6.24` (Qt6 libs; **no openrc subpackage**), `sensorfw 0.15.1` (+openrc), `mlite 0.5.5` (`mlite6.pc`), `nemo-keepalive`, `nemo-qml-plugin-dbus/-configuration/-alarms` (Qt6 QML modules), `amber-mpris`, `mapplauncherd 4.2.16`, `mapplauncherd-qt 1.2.0`, `libmce-qt`, `libconnman-qt`, `usb-moded`, `elogind` (its `-dev` provides `libsystemd.pc`, which satisfies the launcher's `sd_notify` use), `connman`.
- edge testing only: `asteroid-launcher 2.1_git20260909`, `asteroid-btsyncd`, `asteroid-settings`, alarmclock, calendar, calculator, compass, diamonds, flashlight, hrm, map, music, stopwatch, timer, toolwatch, weather, camera, icons/languages/wallpapers; plus `libngf-qt` and `nemo-qml-plugin-calendar`. The launcher's package depends on `libelogind`, `qt6-qtwayland`, `mce`, `timed`, `libngf-qt`, etc.: https://pkgs.alpinelinux.org/package/edge/testing/aarch64/asteroid-launcher. The asteroid-* packages were in `community` for 3.21/3.22 (version 2.1.0, Qt5 era), are absent from 3.23/3.24, and are back in edge testing on Qt6. The maintainer is Bart Ribbers (postmarketOS).
- **Not packaged anywhere I found: `ngfd`** (the feedback daemon; only the client libs `libngf`/`libngf-qt` exist). Without it notifications won't vibrate through the normal path. A small ngfd stand-in that drives our vibrator would be needed, or the hint could be ignored and we vibrate ourselves.
- postmarketOS once shipped `postmarketos-ui-asteroid` (https://tuxphones.com/postmarketos-linux-targets-smartwatches-wearables-watch-friendly-asteroid-os-ui/). It is not in the current pmaports `main/` tree I listed, so its status is **unverified**.

**Glue we would still write:** OpenRC services for timed, btsyncd and the launcher (the upstream units are systemd). We'd also need a substitute for `org.freedesktop.timedate1` (btsyncd time set), a Wi-Fi decision (asteroid-settings uses ConnMan; it's packaged, but it would replace whatever we use now), and sensorfw configuration for our IIO accelerometer. Mixing edge/testing packages into 3.24 is possible because the Qt version is the same, but it isn't supported. Rebuilding the APKBUILDs against 3.24 is the cleaner route. Rough size: a few days for a working boot, then open-ended polish.

**Rendering without a GPU.**
- The Qt Quick software adaptation (`QT_QUICK_BACKEND=software`) exists in Qt 6. Qt's documentation says it cannot render `ShaderEffect` or particles, ignores unsupported features silently, renders transformed text worse, and only does partial updates when scaling is integral: https://doc.qt.io/qt-6/qtquick-visualcanvas-adaptations-software.html. `Qt5Compat.GraphicalEffects` (DropShadow, OpacityMask) and `QtQuick.Effects` (MultiEffect) are shader-based, so they are lost too. In asteroid-launcher at HEAD, 18 of 19 watchfaces, all 6 app-launcher layouts, the quick panel and the compositor (`CircleMaskShader`, for round screens) use these. The UI would run, but with missing shadows and masks. Some watchfaces might look broken rather than plain (**unverified**). Canvas-drawn elements (67 uses) do work in software but are CPU-expensive.
- The compositor side: AsteroidOS's normal path is hwcomposer + libhybris (https://wiki.asteroidos.org/index.php/Graphic_Stack), which we can't use. QtWayland's compositor with shm clients under the software backend has one forum report of working on Qt 6.5 (https://forum.qt.io/post/779830). Officially it is undocumented, so treat it as **unverified**. Running the launcher on our `/dev/fb0` would need a Qt QPA such as `linuxfb` (no DRM needed). Whether QtWaylandCompositor works on linuxfb + software is **unverified** and is the first thing a spike must test.
- Mesa llvmpipe (packaged in `mesa-dri-gallium`) would restore shader effects through Qt's RHI/GLES path, at a much higher CPU cost per frame. I found no benchmarks for llvmpipe or Qt Quick software at 640x480 on 4x Cortex-A53 (**unverified**). For reference, our sway uses pixman rendering at this resolution today, so plain 2D compositing is clearly achievable.
- Layout: the AsteroidOS UI targets small round or square displays and uses `Dims` helpers (percent-of-screen sizing). On a 640x480 4:3 landscape screen most apps would scale. Things placed for round screens will look off-centre or cramped, and some layouts will waste the sides (**unverified**; assumption based on the code style).

**Licensing.** Apps, settings and btsyncd are GPL-3.0. qml-asteroid and lipstick are LGPL-2.1. The launcher shell is BSD/MIT. Assets are MIT/Apache/CC0/CC BY. Running them unmodified or modified on our own watch is fine. Redistributing modified binaries requires offering source under the same licence. Using the QML only as a reference for C re-implementations does not carry GPL obligations, provided we don't copy code. Copying the ANCS parsing logic line by line into our C daemon would make that daemon GPL-3.0.

## 4. Options

**(a) Run asteroid-launcher + apps on Alpine/our kernel**
- Effort: spike 1-2 days (install edge/testing packages, `QT_QPA_PLATFORM=linuxfb` or run the launcher nested under cage/sway, `QT_QUICK_BACKEND=software`). Making it a daily driver is 1-3 weeks: OpenRC services, ngfd stand-in, timedated substitute, sensorfw config, input mapping for the two buttons, watchface fixes for software rendering, and 640x480 layout fixes.
- Gains: a complete, maintained watch UI with notifications, alarms, settings, BLE companion support (Android) and ANCS (iPhone) out of the box.
- Costs: Qt6 + Nemo stack RAM and CPU on a GPU-less SoC. Unknown performance and battery impact, since timed/mce/sensorfw/btsyncd/launcher all stay resident. Effects missing. Tracking an edge/testing package set. Two UI stacks to maintain next to pupui. Not suitable as the kid UI: it isn't designed for parental control, and settings are fully exposed.

**(b) Port selected QML apps standalone (on sway/cage, or eglfs/linuxfb)**
- Effort: per simple app (calculator, stopwatch, diamonds, weather) about half a day to a day. qml-asteroid is already in 3.24 community; `Nemo.Configuration` and `Nemo.KeepAlive` are packaged. Apps that need Nemo.Alarms need timed running (alarmclock, timer). Calendar needs nemo-qml-plugin-calendar/mkcal from edge. Music needs an MPRIS source.
- Gains: reuse of polished app UIs inside the Sxmo/sway grown-up desktop, without adopting the launcher.
- Costs: each app is a full Qt6 QML process. Cold start on A53 without the mapplauncherd booster could take seconds (**unverified**). qml-asteroid hard-requires mapplauncherd-qt6 at build time, which is packaged. Effects are still lost under software rendering. This doesn't help the kid UI at all.

**(c) Rewrite selected app ideas natively in pupui (C, framebuffer)**
- Effort: timer, stopwatch, flashlight and calculator each take about 0.5-1 day in an existing framebuffer toolkit. Alarm clock is about 1-2 days, because it needs RTC wake alarms (`/sys/class/rtc/rtc0/wakealarm`) and persistence. A 2048-style game is about 1 day. Weather card is about 0.5 day once a data source exists. Notification list and pop-up is 1-2 days plus the ANCS daemon.
- Gains: fast, tiny, fully under our control, kid-appropriate design, no Qt runtime, and it fits the existing architecture and power profile.
- Costs: re-implementing what already exists, and no shared look with AsteroidOS.

**Which apps are worth it**

| App idea | Kid mode | Grown-up mode | Notes |
|---|---|---|---|
| Notifications (ANCS) | Filtered/limited (e.g. only messages from family; no body text?) | Full list | Use btsyncd's ANCS code as the reference |
| Timer / countdown | Yes (visual "time left" bar) | Yes | No middleware needed natively |
| Alarm clock | Yes (wake-up, simple) | Yes | Use RTC wakealarm + vibrator; AsteroidOS needs timed |
| Stopwatch | Yes (races) | Yes | Trivial |
| Flashlight | Yes | Yes | Full-white screen + max backlight |
| Game (2048-style "diamonds") | Yes | Optional | Good touch exercise |
| Calculator | Maybe (older kids) | Yes | Trivial |
| Weather | Picture-only card | Yes | Needs a data source: Wi-Fi fetch, or Android companion via the AsteroidOS weather characteristic; iPhone can't push it |
| Music remote | No | Yes | iPhone needs AMS (not in AsteroidOS) |
| Compass | Fun, if a magnetometer exists | Yes | We have no confirmed magnetometer (**unverified**); our accelerometer alone cannot do it |
| HRM / health / steps | No (HR not working) | Steps maybe | Step counting from raw accelerometer is possible but needs its own work |
| Calendar, map, settings | No | Settings already covered by our own UIs | Calendar needs a sync source; map needs GPS (not working) |

**Bottom line:** do (c) for the kid UI and most grown-up app ideas. Treat btsyncd's ANCS/CTS code and the AsteroidOS BLE profile as specs worth copying in behaviour. Consider exposing the same GATT services so Android users get AsteroidOSSync/Gadgetbridge support. Run option (a) only as a short measured spike. Its value depends entirely on Qt Quick software-rendering performance on this SoC, and nobody has published numbers for that.

## Sources

- AsteroidOS GitHub org and per-repo CMake/QML: https://github.com/AsteroidOS
- BLE profiles: https://wiki.asteroidos.org/index.php/BLE_profiles
- Sync clients: https://wiki.asteroidos.org/index.php/Synchronization_Clients
- FAQ: https://wiki.asteroidos.org/index.php/Frequently_Asked_Questions_(FAQ)
- Graphic stack: https://wiki.asteroidos.org/index.php/Graphic_Stack
- AsteroidOS 2.1 release (Qt 6 migration note): https://asteroidos.org/news/2-1-release/index.html
- btsyncd ANCS/CTS: https://github.com/AsteroidOS/asteroid-btsyncd/tree/master/src
- Alpine packages: https://pkgs.alpinelinux.org/packages (e.g. https://pkgs.alpinelinux.org/package/edge/testing/aarch64/asteroid-launcher)
- Qt Quick software adaptation: https://doc.qt.io/qt-6/qtquick-visualcanvas-adaptations-software.html
- QtWayland compositor + software backend (forum, anecdotal): https://forum.qt.io/post/779830
- postmarketOS Asteroid UI (2023 coverage): https://tuxphones.com/postmarketos-linux-targets-smartwatches-wearables-watch-friendly-asteroid-os-ui/
