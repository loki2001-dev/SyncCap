#!/usr/bin/env python3
"""Offline verification of a recorded session: synchronization and channel alignment.

    python scripts/verify_sync.py [session_dir] [--tolerance-ms 2.0] [--json report.json]

With no session_dir the newest directory under recordings/ is used.

Checks, per segment and for the whole session:
  1. every locked frame set in sync.csv has offset <= tolerance (the collector's own verdict re-checked)
  2. offset statistics: max, p99, mean over all complete sets
  3. every chN.h264 holds exactly as many pictures as the segment has locked sets
     (the H.264 stream is parsed directly; no ffprobe needed)
  4. every chN.h264 starts with SPS and an IDR picture (each segment file decodes on its own)
  5. sensor.csv rows per segment (informational)

Exit code 0 when every check passes, 1 otherwise.
"""
import argparse
import csv
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def inspect_h264(path):
    """Count pictures (slices with first_mb_in_slice == 0) and find the first NAL and picture types."""
    with open(path, "rb") as file:
        data = file.read()
    pictures = 0
    idr = 0
    first_nal = None
    first_picture_idr = None
    index = 0
    size = len(data)
    while True:
        index = data.find(b"\x00\x00\x01", index)
        if index < 0 or index + 4 >= size:
            break
        header = index + 3
        nal_type = data[header] & 0x1F
        if first_nal is None:
            first_nal = nal_type
        if nal_type in (1, 5) and (data[header + 1] & 0x80):
            if first_picture_idr is None:
                first_picture_idr = nal_type == 5
            pictures += 1
            if nal_type == 5:
                idr += 1
        index = header + 1
    return {"pictures": pictures, "idr": idr, "first_nal": first_nal, "first_picture_idr": bool(first_picture_idr),
            "bytes": size}


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    position = min(len(sorted_values) - 1, max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[position]


def verify_session(session, tolerance_ms):
    tolerance_us = tolerance_ms * 1000.0
    segments = sorted(d for d in glob.glob(os.path.join(session, "seg_*")) if os.path.isdir(d))
    report = {"session": session, "tolerance_ms": tolerance_ms, "segments": [], "failures": []}
    all_offsets = []
    totals = {"locked": 0, "violation": 0, "incomplete": 0, "backpressure": 0}
    for segment in segments:
        name = os.path.basename(segment)
        entry = {"segment": name, "verdicts": {}, "channels": {}}
        sync_path = os.path.join(segment, "sync.csv")
        locked = 0
        if os.path.exists(sync_path):
            with open(sync_path, newline="") as file:
                for row in csv.DictReader(file):
                    verdict = row["verdict"]
                    entry["verdicts"][verdict] = entry["verdicts"].get(verdict, 0) + 1
                    totals[verdict] = totals.get(verdict, 0) + 1
                    offset = float(row["offset_us"])
                    if verdict != "incomplete":
                        all_offsets.append(offset)
                    if verdict == "locked":
                        locked += 1
                        if offset > tolerance_us:
                            report["failures"].append("%s tick %s locked with offset %.0f us > %.0f us"
                                                      % (name, row["tick"], offset, tolerance_us))
        else:
            report["failures"].append("%s has no sync.csv" % name)
        for video in sorted(glob.glob(os.path.join(segment, "ch*.h264"))):
            channel = os.path.basename(video)
            summary = inspect_h264(video)
            entry["channels"][channel] = summary
            if summary["pictures"] != locked:
                report["failures"].append("%s/%s has %d pictures, segment has %d locked sets"
                                          % (name, channel, summary["pictures"], locked))
            if summary["first_nal"] != 7 or not summary["first_picture_idr"]:
                report["failures"].append("%s/%s does not start with SPS + IDR" % (name, channel))
        sensor_path = os.path.join(segment, "sensor.csv")
        if os.path.exists(sensor_path):
            with open(sensor_path) as file:
                entry["sensor_rows"] = max(0, sum(1 for _ in file) - 1)
        report["segments"].append(entry)

    all_offsets.sort()
    report["totals"] = totals
    report["offset_us"] = {
        "max": all_offsets[-1] if all_offsets else 0.0,
        "p99": percentile(all_offsets, 0.99),
        "p50": percentile(all_offsets, 0.50),
        "mean": (sum(all_offsets) / len(all_offsets)) if all_offsets else 0.0,
        "sets": len(all_offsets),
        "within_tolerance_pct": (100.0 * sum(1 for v in all_offsets if v <= tolerance_us) / len(all_offsets))
        if all_offsets else 0.0,
    }
    if not segments:
        report["failures"].append("no segments in %s" % session)
    return report


def print_report(report):
    print("session  %s" % report["session"])
    print("tolerance %.3f ms" % report["tolerance_ms"])
    for entry in report["segments"]:
        channels = ", ".join("%s=%d" % (name.replace(".h264", ""), summary["pictures"])
                             for name, summary in entry["channels"].items())
        verdicts = ", ".join("%s=%d" % item for item in sorted(entry["verdicts"].items()))
        print("  %-28s sets[%s]  pictures[%s]  sensor_rows=%s"
              % (entry["segment"], verdicts, channels, entry.get("sensor_rows", "-")))
    offsets = report["offset_us"]
    print("offset   max=%.0f us  p99=%.0f us  p50=%.0f us  mean=%.1f us  sets=%d  within=%.3f%%"
          % (offsets["max"], offsets["p99"], offsets["p50"], offsets["mean"], offsets["sets"],
             offsets["within_tolerance_pct"]))
    print("verdicts %s" % ", ".join("%s=%d" % item for item in sorted(report["totals"].items())))
    if report["failures"]:
        print("RESULT   FAIL (%d)" % len(report["failures"]))
        for failure in report["failures"][:20]:
            print("  - " + failure)
    else:
        print("RESULT   PASS")


def newest_session(root):
    sessions = [d for d in glob.glob(os.path.join(root, "*")) if os.path.isdir(d)]
    return max(sessions, key=os.path.getmtime) if sessions else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("session", nargs="?", help="session directory (default: newest under recordings/)")
    parser.add_argument("--recordings", default=os.path.join(ROOT, "recordings"))
    parser.add_argument("--tolerance-ms", type=float, default=2.0)
    parser.add_argument("--json", help="write the full report as JSON")
    args = parser.parse_args()

    session = args.session or newest_session(args.recordings)
    if not session or not os.path.isdir(session):
        print("no session directory found", file=sys.stderr)
        return 1
    report = verify_session(session, args.tolerance_ms)
    print_report(report)
    if args.json:
        with open(args.json, "w") as file:
            json.dump(report, file, indent=2)
    return 0 if not report["failures"] else 1


if __name__ == "__main__":
    sys.exit(main())
