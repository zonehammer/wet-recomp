# Roadmap

Where WET Recompiled goes after 0.1.0. Each release has a theme; items within a release
are roughly in priority order. Effort: S = an evening, M = a few sessions, L = a larger
project.

## 0.2 — Stability and correctness

Make a full playthrough reliable before adding more features.

| Item | Why | Effort |
|---|---|---|
| **Full playthrough check**, chapter by chapter (boss fights, Rage mode, car hopping, slow-motion, Challenges, every cutscene) | Later levels use console features nothing has exercised yet; every run's `wet.log` shows crashes, stalls and missing functions | M, ongoing |
| ~~Clear the remaining stubs~~ the log reports (done for the menus and first level: system version, random, DNS, XMP / XGI messages; later chapters may reach others) | Unimplemented functions are where hidden bugs live | M |
| **Fix the video race at its cause** (decoder vs. GPU emulation), so videos can run at any frame rate | Video mode hides it; the real fix removes the 30 FPS cap during videos | M |
| **Audit host writes into guest memory** (audio, content, anything besides file reads) for the write-watch race fixed in file loading | The same race could cause rarer crashes elsewhere | S |
| **Import earned achievements** from the game's own save (`ACHIEVE.DAT`) | Achievements earned before 0.1.0 show as locked | M |
| **Automated soak test**: scripted input (`WET_AUTOPRESS`) through menus and the first level, run before each release | Catches regressions without a manual playthrough | S |

## 0.3 — Controls and feel

| Item | Why | Effort |
|---|---|---|
| ~~Direct mouse aim~~ (done: `mouse_direct`, uncapped stick values written into the game's pad object) | True 1:1 aim, no stick speed cap or acceleration | M–L |
| ~~Keyboard rebinding~~ (done: wet.ini `key_*`, the F1 overlay and the launcher) | Everything except the mouse buttons is fixed today | S |
| ~~PlayStation button prompts~~ (done: `button_prompts`; the game's button textures redrawn with PlayStation symbols as they load) | Matches the controller in hand | L |
| ~~Keyboard / mouse button prompts~~ (done: the prompts show the bound key or mouse button; automatic with no controller) | Prompts match the keys you press | M |
| ~~Language option~~ (done: `language`, the disc's five languages, text and speech) | Play in your own language | S |
| ~~Settings overlay redesign and quit to desktop~~ (done: tabbed F1 overlay; hold Back in the pause menu or the F1 button) | Less clutter; no Alt+F4 needed | S |

## 0.4 — Performance and visuals

| Item | Why | Effort |
|---|---|---|
| ~~Don't wait for shaders~~ (done: `async_shaders`) | Removes the first-time stutter the shader cache can't | M |
| ~~Widescreen and ultrawide~~ (done: `widescreen`, Hor+ view, extra horizontal resolution, HUD kept 16:9) | The game renders 16:9 only | M–L |
| ~~Field of view slider~~ (done: `fov`, overlay and launcher) | Common request, especially with wide screens | M |
| ~~Any frame rate~~ (done: 30-360 or match the monitor; VRR works. Truly uncapped would need a variable time step) | Today it's 30 / 60 / 120 / 144 | M |
| ~~Better anti-aliasing~~ (done: SMAA, `antialiasing = 3`) | FXAA is soft | M |
| Lower-spec performance, first pass (done in 0.2.5: the console's 4x clears drawn at 1x, shader constants written in bulk, vblank thread sleeps, adaptive vblank; +25% at 4K, 720p at the 360 cap here) | Lower-end GPUs and CPUs | M |
| Shader cache built in the background (done in 0.2.6; `shader_preload = 1` builds it up front instead) | No half-minute black screen when the driver's cache is cold | S |
| **Further GPU-thread work** (resolves and texture loads at high resolution (partial reloads done in 0.2.6; resolving straight into textures next), driver submission cost, fewer memory traps) | Headroom for 4K at 144 FPS on slower PCs | M |

## 0.5 — Packaging and ease of use

| Item | Why | Effort |
|---|---|---|
| ~~One-click setup~~ (done: `WetSetup.exe`, attached to releases: ISO in, tools fetched, built, desktop shortcut) | No command prompt needed | M |
| ~~Update check~~ (done: launcher, Help > Check for updates) | Players know when to rebuild | S |
| ~~Bug report button~~ (done: launcher, Help > Report a bug: zip of logs, settings and system info) | Better reports, less back-and-forth | S |
| ~~Launcher extras~~ (done: monitor choice, controller test, achievements list) | Fewer reasons to open wet.ini | S–M |
| **Linux / Steam Deck**: Windows build made on Linux (`tools/setup_linux.py`) to run with Proton - needs testing on a Deck / Bazzite; a native Linux build later | Many players are on Deck | L |

## Ongoing

- ~~Issue templates~~ (done: crash / graphics / audio forms asking for the bug report zip and an F8 capture).
- Keep the README, player README and this roadmap current with each release.
- Every push is built by GitHub Actions on Windows with both toolchains.
