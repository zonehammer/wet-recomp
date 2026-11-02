#!/usr/bin/env python3
"""Soak test: plays through the menus and into the first level with scripted
input (WET_AUTOPRESS), then checks wet.log for crashes, hangs and new stubs.

    python tools/soak_test.py                 # one run, about 6 minutes
    python tools/soak_test.py --runs 3        # three runs back to back
    python tools/soak_test.py --script "START,WAIT:5,A,EXIT"

Run it before tagging a release. Each run uses a fresh save folder (so the game
always starts with no saves and goes New Game -> first level), keeps its log in
soak/run-N.log and prints PASS or FAIL with the reason. Your own saves and
settings aren't touched (wet.ini is read but the in-game settings aren't changed).

Exit code 0 when every run passed.
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Title screen -> New Game -> difficulty -> first level, then a bit of play:
# moving, looking, shooting, jumping, aiming, pausing. The waits are generous
# (loading and the opening cutscene); A also skips cutscenes.
DEFAULT_SCRIPT = ",".join([
    "WAIT:5", "START", "WAIT:4",                  # press start
    "A", "WAIT:4", "A", "WAIT:4", "A", "WAIT:4",  # storage / profile prompts, main menu: New Game
    "A", "WAIT:4", "A",                           # difficulty, confirm
    "WAIT:45",                                    # loading and the opening cutscene
    "A", "WAIT:5", "START", "WAIT:3", "A",        # skip whatever is still playing
    "WAIT:30",                                    # loading into the level
    # Play: about two minutes of moving, turning, shooting and jumping.
    *(["MOVE:UP:4", "LOOK:LEFT:1", "HOLD:RT:2", "A", "MOVE:LEFT:2", "HOLD:LT:1.5", "HOLD:RT:2",
       "MOVE:UP:3", "LOOK:RIGHT:1.5", "A", "MOVE:RIGHT:2", "X", "MOVE:DOWN:2"] * 4),
    "START", "WAIT:4", "DOWN", "UP", "START", "WAIT:5",   # pause menu and back
    "EXIT",
])


def script_seconds(script):
    total = 0.0
    for tok in script.split(","):
        part = tok.strip().upper().split(":")
        if part[0] == "EXIT" or not part[0]:
            continue
        if part[0] == "WAIT" and len(part) == 2:
            total += float(part[1])
        elif part[0] in ("HOLD", "MOVE", "LOOK") and len(part) == 3:
            total += float(part[2])
        else:
            total += 2.0
    return total


def find_exe():
    for path in ("build/runtime/WetRecomp.exe", "build/windows/runtime/WetRecomp.exe", "build/runtime/WetRecomp"):
        full = os.path.join(REPO, path)
        if os.path.exists(full):
            return full
    return None


def analyse(log_text):
    """What a run's log says: crash lines, stubs, frame rates, how far the script got."""
    result = {
        "fatal": [l for l in log_text.splitlines() if " FATAL:" in l],
        "finished": "Autopress: script finished" in log_text,
        "stubs": sorted(set(re.findall(r"STUB (\w+)\(", log_text))),
        "fps": [float(m) for m in re.findall(r"Performance: (\d+) fps average", log_text)],
        "worst_ms": [float(m) for m in re.findall(r"slowest frame ([\d.]+) ms", log_text)],
        "frames": 0,
        "step": None,
    }
    frames = re.findall(r"VdSwap: frame (\d+)", log_text)
    if frames:
        result["frames"] = int(frames[-1])
    steps = re.findall(r"Autopress: step (\d+) of (\d+)", log_text)
    if steps:
        result["step"] = steps[-1]
    return result


def run_once(exe, script, delay, index, out_dir, timeout_margin, hang_seconds, keep_saves):
    log_path = os.path.join(out_dir, f"run-{index}.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    hdd = None
    args = [exe, "--log", log_path]
    if not keep_saves:
        hdd = tempfile.mkdtemp(prefix="wet-soak-hdd-")
        args += ["--hdd", hdd]
    env = dict(os.environ)
    env.update({"WET_AUTOPRESS": script, "WET_AUTOPRESS_DELAY": str(delay), "WET_NO_DIALOGS": "1"})
    limit = delay + script_seconds(script) + timeout_margin
    print(f"== Run {index}: up to {limit / 60:.1f} minutes", flush=True)

    start = time.time()
    proc = subprocess.Popen(args, cwd=os.path.dirname(exe), env=env)
    last_size, last_change, verdict = -1, time.time(), None
    try:
        while proc.poll() is None:
            time.sleep(2)
            now = time.time()
            size = os.path.getsize(log_path) if os.path.exists(log_path) else 0
            if size != last_size:
                last_size, last_change = size, now
            elif now - last_change > hang_seconds:
                verdict = f"hang: wet.log stopped growing for {hang_seconds} s"
                break
            if now - start > limit:
                verdict = f"timeout: the script didn't finish within {limit:.0f} s"
                break
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if hdd:
            shutil.rmtree(hdd, ignore_errors=True)
    elapsed = time.time() - start

    text = ""
    if os.path.exists(log_path):
        with open(log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    a = analyse(text)
    if a["fatal"]:
        verdict = "crash: " + a["fatal"][0].split(" FATAL:", 1)[1].strip()
    elif verdict is None and not a["finished"]:
        verdict = f"exited early (exit code {proc.returncode})"
    elif verdict is None and proc.returncode not in (0, None):
        verdict = f"exit code {proc.returncode}"

    print(f"   {'PASS' if verdict is None else 'FAIL - ' + verdict}  ({elapsed / 60:.1f} min, {a['frames']} frames)")
    if a["step"]:
        print(f"   script reached step {a['step'][0]} of {a['step'][1]}")
    if a["fps"]:
        print(f"   FPS: {min(a['fps']):.0f} lowest / {sum(a['fps']) / len(a['fps']):.0f} average over "
              f"{len(a['fps'])} samples; slowest frame {max(a['worst_ms']):.1f} ms")
    for line in a["fatal"][:6]:
        print("   " + line.strip())
    print(f"   log: {log_path}")
    return verdict, a


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", help="WetRecomp.exe to test (default: the one in build/)")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--script", default=DEFAULT_SCRIPT, help="WET_AUTOPRESS steps (see runtime/src/input.cpp)")
    ap.add_argument("--delay", type=float, default=40, help="seconds before the script starts (intro videos)")
    ap.add_argument("--hang", type=float, default=90, help="seconds without log output that count as a hang")
    ap.add_argument("--keep-saves", action="store_true", help="use the normal save folder instead of an empty one")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe) if args.exe else find_exe()
    if not exe or not os.path.exists(exe):
        sys.exit("No WetRecomp.exe found - build first (python tools/build.py) or pass --exe.")
    script = args.script if "EXIT" in args.script.upper() else args.script + ",EXIT"
    out_dir = os.path.join(REPO, "soak")
    os.makedirs(out_dir, exist_ok=True)
    print(f"Soak test of {exe}\n{len(script.split(','))} steps, about {script_seconds(script) / 60:.1f} minutes of input "
          f"after {args.delay:.0f} s")

    failures, stubs = 0, set()
    for i in range(1, args.runs + 1):
        verdict, a = run_once(exe, script, args.delay, i, out_dir, 120, args.hang, args.keep_saves)
        failures += verdict is not None
        stubs.update(a["stubs"])

    report = os.path.join(out_dir, "report.txt")
    with open(report, "w") as f:
        f.write(f"Soak test {datetime.datetime.now():%Y-%m-%d %H:%M}: {args.runs - failures} of {args.runs} passed\n")
        f.write("Unimplemented functions the game called: " + (", ".join(sorted(stubs)) or "none") + "\n")
    print(f"\n{args.runs - failures} of {args.runs} runs passed.")
    if stubs:
        print("Unimplemented functions the game called (worth a look if new): " + ", ".join(sorted(stubs)))
    print(f"Report: {report}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
