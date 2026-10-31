# Roadmap

Where WET Recompiled goes after 0.1.0. Each release has a theme; items within a release
are roughly in priority order. Effort: S = an evening, M = a few sessions, L = a larger
project.

## 0.2 — Stability and correctness

Make a full playthrough reliable before adding more features.

| Item | Why | Effort |
|---|---|---|
| **Full playthrough check**, chapter by chapter (boss fights, Rage mode, car hopping, slow-motion, Challenges, every cutscene) | Later levels use console features nothing has exercised yet; every run's `wet.log` shows crashes, stalls and missing functions | M, ongoing |
| **Clear the remaining stubs** the log reports (system version, `MmMapIoSpace`, network and XMsg calls) | Unimplemented functions are where hidden bugs live | M |
| **Fix the video race at its cause** (decoder vs. GPU emulation), so videos can run at any frame rate | Video mode hides it; the real fix removes the 30 FPS cap during videos | M |
| **Audit host writes into guest memory** (audio, content, anything besides file reads) for the write-watch race fixed in file loading | The same race could cause rarer crashes elsewhere | S |
| **Import earned achievements** from the game's own save (`ACHIEVE.DAT`) | Achievements earned before 0.1.0 show as locked | M |
| **Automated soak test**: scripted input (`WET_AUTOPRESS`) through menus and the first level, run before each release | Catches regressions without a manual playthrough | S |

## 0.3 — Controls and feel

| Item | Why | Effort |
|---|---|---|
| **Direct mouse aim**: drive the game's camera angles instead of the right stick | True 1:1 aim, no stick speed cap or acceleration | M–L |
| **Keyboard rebinding** in wet.ini and the launcher | Everything except the mouse buttons is fixed today | S |
| **Keep sound on the speakers** when a PlayStation pad registers as an audio device (opt-in) | Windows switches the default output to the controller | S |
| **PlayStation button prompts** (swap the game's Xbox button icons) | Matches the controller in hand | L |
| **DualSense / DualShock gyro aim** (optional) | Fine aim on PlayStation pads | M |

## 0.4 — Performance and visuals

| Item | Why | Effort |
|---|---|---|
| ~~Don't wait for shaders~~ (done: `async_shaders`) | Removes the first-time stutter the shader cache can't | M |
| **Widescreen and ultrawide** (21:9, 32:9) with correct aspect and HUD placement | The game renders 16:9 only | M–L |
| **Field of view** slider | Common request, especially with wide screens | M |
| **Any frame rate** (165, 240, uncapped with VRR / G-Sync / FreeSync) | Today it's 30 / 60 / 120 / 144 | M |
| **Better anti-aliasing** (SMAA, or MSAA through Xenia's render targets) | FXAA is soft | M |
| **Motion blur and other post effects** as toggles, like film grain | Personal taste; some effects look worse at high frame rates | S per effect |
| **Further GPU-thread work** (fewer memory traps, cheaper fence wake-ups) | Headroom for 4K at 144 FPS on slower CPUs | M |

## 0.5 — Packaging and ease of use

| Item | Why | Effort |
|---|---|---|
| **One-click setup**: the launcher (or a setup window) takes the ISO and runs extraction and the build | No command prompt needed | M |
| **Update check** in the launcher (new tag on GitHub) | Players know when to rebuild | S |
| **Bug report button**: zips `wet.log`, `wet.ini` and system info | Better reports, less back-and-forth | S |
| **Launcher extras**: display / monitor choice, controller test, achievements list | Fewer reasons to open wet.ini | S–M |
| **Linux / Steam Deck**: test under Proton, then a native Linux build (the runtime already builds on Linux) | Many players are on Deck | L |

## Ongoing

- Issue templates (crash / graphics / audio) asking for `wet.log` and an F8 capture.
- Keep the README, player README and this roadmap current with each release.
- Every push is built by GitHub Actions on Windows with both toolchains.
