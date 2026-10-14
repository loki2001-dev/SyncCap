#!/usr/bin/env python3
"""Generate the simulation inputs: four 1080p test videos and a sensor CSV.

    python scripts/make_samples.py [--duration 20] [--ffmpeg ffmpeg]

samples/video/ch0.mp4   H.264 1920x1080 30 fps, testsrc2 pattern
samples/video/ch1.mp4   H.264 1920x1080 30 fps, testsrc pattern
samples/video/ch2.avi   MPEG-4 Part 2 in AVI 1920x1080 30 fps, SMPTE HD bars with a rotating hue
samples/video/ch3_25fps.mp4  H.264 1920x1080 25 fps: a source slower than the 30 fps tick (shows duplicates)
samples/video/small_720p.mp4 H.264 1280x720 30 fps: exercises the scaler (source size != output size)
samples/sensor/sensor.csv  600 rows (60 s at 100 ms), deterministic (seed 42)

Video files are not committed (see .gitignore); the CSV is.
"""
import argparse
import math
import os
import random
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VIDEOS = [
    ("ch0.mp4", "testsrc2=size=1920x1080:rate=30", ["-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p"]),
    ("ch1.mp4", "testsrc=size=1920x1080:rate=30", ["-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p"]),
    ("ch2.avi", "smptehdbars=size=1920x1080:rate=30,hue=H=2*PI*t/10", ["-c:v", "mpeg4", "-q:v", "6"]),
    ("ch3_25fps.mp4", "testsrc2=size=1920x1080:rate=25", ["-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p"]),
    ("small_720p.mp4", "testsrc2=size=1280x720:rate=30", ["-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p"]),
]


def make_videos(ffmpeg, duration, directory):
    os.makedirs(directory, exist_ok=True)
    for name, source, codec in VIDEOS:
        path = os.path.join(directory, name)
        command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", source,
                   "-t", str(duration)] + codec + [path]
        print("generating", os.path.relpath(path, ROOT))
        subprocess.run(command, check=True)


def make_sensor_csv(path, rows=600, period_ms=100, seed=42):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rng = random.Random(seed)
    temperature = 24.5
    humidity = 60.0
    with open(path, "w", newline="\n") as file:
        file.write("timestamp_ms,temperature_c,humidity_pct,accel_x_g,accel_y_g,accel_z_g\n")
        for index in range(rows):
            temperature = min(35.0, max(15.0, temperature + rng.gauss(0.0, 0.02)))
            humidity = min(90.0, max(30.0, humidity + rng.gauss(0.0, 0.05)))
            # a slow 0.5 Hz sway on x plus noise, gravity on z
            sway = 0.05 * math.sin(2.0 * math.pi * 0.5 * index * period_ms / 1000.0)
            ax = sway + rng.gauss(0.0, 0.01)
            ay = rng.gauss(0.0, 0.01)
            az = 1.0 + rng.gauss(0.0, 0.01)
            file.write("%d,%.3f,%.3f,%.5f,%.5f,%.5f\n" % (index * period_ms, temperature, humidity, ax, ay, az))
    print("generated", os.path.relpath(path, ROOT), "rows=%d" % rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duration", type=int, default=20, help="seconds per video (default 20)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable with libx264")
    parser.add_argument("--csv-only", action="store_true", help="only write the sensor CSV")
    args = parser.parse_args()

    make_sensor_csv(os.path.join(ROOT, "samples", "sensor", "sensor.csv"))
    if args.csv_only:
        return 0
    if shutil.which(args.ffmpeg) is None:
        print("ffmpeg not found: install it or pass --ffmpeg <path>", file=sys.stderr)
        return 1
    make_videos(args.ffmpeg, args.duration, os.path.join(ROOT, "samples", "video"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
