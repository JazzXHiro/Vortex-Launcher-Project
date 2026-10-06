"""Minecraft play history out of a PrismLauncher install, for a one-time import.

Usage: prism_history.py <PRISM_INSTALL_DIR>

Prints, one per line:
    TOTAL <seconds>                         Prism's own lifetime figure
    SESSION <start_epoch> <end_epoch> <instance>

Vortex can only follow Minecraft from the moment it starts tracking Prism, so
everything played before that would read as never played. Prism keeps the two
halves of that history in different places:

1. `totalTimePlayed` in every instance.cfg -- exact, but a bare total, and a
   total cannot be weighted by recency.

2. Minecraft's own logs, one per launch: latest.log, then YYYY-MM-DD-N.log.gz
   once the next launch rolls it over. The file name is the session's date and
   the first and last "[HH:MM:SS]" lines are when it started and ended. Summed,
   they reproduce Prism's figure to within seconds -- Prism starts its clock as
   the process starts, the log a moment later.

Logs get deleted and crash short, so the C++ side records these sessions and
takes whatever Prism counts beyond them as an unlogged baseline.

Exits 1 when no Prism data directory can be found, so the caller retries on a
later scan rather than marking the game imported with nothing.
"""

import gzip
import os
import re
import sys
import time
from datetime import date, datetime, timedelta

# "[16:22:27] [main/INFO]: ..." (vanilla, Fabric) and
# "[06Oct2026 20:16:30.123] [main/INFO] ..." (Forge, NeoForge).
_PLAIN = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\]")
_DATED = re.compile(r"^\[(\d{2})([A-Za-z]{3})(\d{4}) (\d{2}):(\d{2}):(\d{2})")
_ARCHIVE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})-(\d+)\.log\.gz$")

# log4j rolls the log over at midnight, so one session that crosses it is two
# files: the first ends just before 00:00, the next starts just after.
_MIDNIGHT_SLACK = timedelta(minutes=2)


def read_cfg(path):
    """key=value pairs from a Prism .cfg file (INI without sections)."""
    values = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line and not line.startswith(("[", "#", ";")):
                    key, _, value = line.partition("=")
                    values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def data_dir(install_dir):
    # Portable installs keep everything beside the exe.
    if os.path.exists(os.path.join(install_dir, "portable.txt")):
        return install_dir
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidate = os.path.join(appdata, "PrismLauncher")
        if os.path.isdir(candidate):
            return candidate
    return None


def instances_dir(data):
    configured = read_cfg(os.path.join(data, "prismlauncher.cfg")).get("InstanceDir")
    path = configured or "instances"
    return path if os.path.isabs(path) else os.path.join(data, path)


def read_lines(path):
    opener = gzip.open if path.endswith(".gz") else open
    try:
        with opener(path, "rt", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except (OSError, EOFError):
        return []


def stamp(line, day):
    """The line's timestamp as a datetime, or None. `day` dates a time-only one."""
    m = _DATED.match(line)
    if m:
        try:
            return datetime.strptime(
                f"{m.group(1)}{m.group(2)}{m.group(3)} "
                f"{m.group(4)}:{m.group(5)}:{m.group(6)}", "%d%b%Y %H:%M:%S")
        except ValueError:
            return None
    m = _PLAIN.match(line)
    if m:
        return datetime.combine(day, datetime.min.time()).replace(
            hour=int(m.group(1)), minute=int(m.group(2)), second=int(m.group(3)))
    return None


def log_span(path, day, day_is_end=False):
    """(start, end) of one log file, or None if it holds no timestamps.

    `day` dates time-only stamps: the start's day for an archive (log4j names
    it after the period it covers), the end's day for latest.log (dated by
    when it was last written)."""
    lines = read_lines(path)
    first = next((t for t in (stamp(l, day) for l in lines) if t), None)
    last = next((t for t in (stamp(l, day) for l in reversed(lines)) if t), None)
    if not first or not last:
        return None
    if last < first:
        # Time-only stamps that crossed midnight inside one file.
        if day_is_end:
            first -= timedelta(days=1)
        else:
            last += timedelta(days=1)
    return first, last


def instance_sessions(instance):
    logs = os.path.join(instance, "minecraft", "logs")
    if not os.path.isdir(logs):
        logs = os.path.join(instance, ".minecraft", "logs")
    if not os.path.isdir(logs):
        return []

    dated = []
    for name in os.listdir(logs):
        path = os.path.join(logs, name)
        m = _ARCHIVE.match(name)
        if m:
            day = date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            dated.append(((day, int(m.group(4))), path, day, False))
        elif name == "latest.log":
            # Always the newest, and dated by its last write -- the end.
            day = datetime.fromtimestamp(os.path.getmtime(path)).date()
            dated.append(((date.max, 0), path, day, True))
    dated.sort()

    sessions = []
    for _, path, day, day_is_end in dated:
        span = log_span(path, day, day_is_end)
        if not span:
            continue
        start, end = span
        if end - start > timedelta(days=2):
            continue
        if sessions:
            prev_start, prev_end = sessions[-1]
            midnight = datetime.combine(start.date(), datetime.min.time())
            if (start - midnight < _MIDNIGHT_SLACK
                    and midnight - prev_end < _MIDNIGHT_SLACK
                    and prev_end <= start):
                sessions[-1] = (prev_start, end)
                continue
        sessions.append((start, end))
    return sessions


def main():
    if len(sys.argv) < 2:
        print("usage: prism_history.py <PRISM_INSTALL_DIR>", file=sys.stderr)
        return 2

    data = data_dir(sys.argv[1])
    if not data:
        print("no PrismLauncher data directory found", file=sys.stderr)
        return 1
    root = instances_dir(data)
    if not os.path.isdir(root):
        print(f"no instances directory at {root}", file=sys.stderr)
        return 1

    total = 0
    out = []
    for name in sorted(os.listdir(root)):
        instance = os.path.join(root, name)
        cfg_path = os.path.join(instance, "instance.cfg")
        if not os.path.isfile(cfg_path):
            continue
        try:
            total += max(0, int(read_cfg(cfg_path).get("totalTimePlayed", "0")))
        except ValueError:
            pass
        for start, end in instance_sessions(instance):
            out.append((start, end, name))

    print(f"TOTAL {total}")
    for start, end, name in sorted(out):
        print(f"SESSION {int(time.mktime(start.timetuple()))} "
              f"{int(time.mktime(end.timetuple()))} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
