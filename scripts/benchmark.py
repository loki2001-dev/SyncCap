#!/usr/bin/env python3
"""Measure CPU and memory of the acquisition system for 3 and 4 channels.

    python scripts/benchmark.py [--build-dir build] [--seconds 60] [--inputs files|synthetic] [--channels 3 4]

Runs synccap_collector without broker or server (their reconnect timers are idle), samples the process
every second and prints CPU (percent of one core and of the whole machine), peak and mean resident memory,
plus the encoder and sync figures the collector logged. No third-party Python packages needed.
"""
import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import verify_sync  # noqa: E402

IS_WINDOWS = os.name == "nt"
FILE_CHANNELS = [
    {"type": "file", "uri": "samples/video/ch0.mp4", "loop": True},
    {"type": "file", "uri": "samples/video/ch1.mp4", "loop": True},
    {"type": "file", "uri": "samples/video/ch2.avi", "loop": True},
    {"type": "file", "uri": "samples/video/ch0.mp4", "loop": True},
]


def sample_process(pid):
    """(cpu_seconds_total, resident_bytes) of one process."""
    if IS_WINDOWS:
        command = ["powershell", "-NoProfile", "-Command",
                   "$p = Get-Process -Id %d; '{0} {1}' -f $p.TotalProcessorTime.TotalSeconds, $p.WorkingSet64" % pid]
        output = subprocess.run(command, capture_output=True, text=True, check=True).stdout.split()
        return float(output[0]), int(output[1])
    with open("/proc/%d/stat" % pid) as file:
        fields = file.read().rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    cpu = (int(fields[11]) + int(fields[12])) / ticks
    with open("/proc/%d/status" % pid) as file:
        rss = next(int(line.split()[1]) * 1024 for line in file if line.startswith("VmRSS:"))
    return cpu, rss


def make_config(channels, inputs, work, preview):
    with open(os.path.join(ROOT, "config", "collector.json")) as file:
        config = json.load(file)
    if inputs == "files":
        config["video"]["channels"] = [dict(FILE_CHANNELS[index], id=index) for index in range(channels)]
    else:
        config["video"]["channels"] = [{"id": index, "type": "synthetic"} for index in range(channels)]
    config["storage"] = {"root": os.path.join(work, "recordings"), "segment_seconds": 30}
    config["sensor"] = {"type": "simulated", "period_ms": 100, "seed": 42}
    config["mqtt"]["host"] = "127.0.0.1"
    config["mqtt"]["port"] = 1
    config["report"]["url"] = "http://127.0.0.1:1/status"
    config["log"] = {"dir": os.path.join(work, "logs"), "level": "info"}
    config["preview"] = {"enabled": preview, "port": 0}
    path = os.path.join(work, "collector.json")
    with open(path, "w") as file:
        json.dump(config, file, indent=2)
    return path


def run_once(build_dir, channels, inputs, seconds, preview):
    work = tempfile.mkdtemp(prefix="synccap_bench_%dch_" % channels)
    config = make_config(channels, inputs, work, preview)
    binary = os.path.join(build_dir, "synccap_collector" + (".exe" if IS_WINDOWS else ""))
    log_path = os.path.join(work, "collector.log")
    with open(log_path, "w") as log:
        process = subprocess.Popen([binary, "--config", config, "--duration", str(seconds + 8)], cwd=ROOT,
                                   stdout=log, stderr=subprocess.STDOUT)
        time.sleep(5)  # warm-up: sources open, encoders settle
        samples = []
        previous_cpu, _ = sample_process(process.pid)
        previous_time = time.time()
        end = previous_time + seconds
        # by elapsed time, not sample count: one sample may itself take most of a second on Windows
        while time.time() < end and process.poll() is None:
            time.sleep(1)
            cpu, rss = sample_process(process.pid)
            now = time.time()
            samples.append((100.0 * (cpu - previous_cpu) / (now - previous_time), rss))
            previous_cpu, previous_time = cpu, now
        process.wait(timeout=60)
    with open(log_path, encoding="utf-8", errors="replace") as file:
        text = file.read()
    encode = [float(value) for value in re.findall(r"encode_ms_avg=([0-9.]+)", text)]
    worst_encode = [float(value) for value in re.findall(r"encode_ms_worst=([0-9.]+)", text)]
    fps = [float(value) for value in re.findall(r"channel id=\d+ fps=([0-9.]+)", text)]
    sessions = os.listdir(os.path.join(work, "recordings"))
    report = verify_sync.verify_session(os.path.join(work, "recordings", sessions[0]), 2.0)
    cpus = [cpu for cpu, _ in samples]
    rss = [value for _, value in samples]
    cores = os.cpu_count() or 1
    return {
        "channels": channels,
        "inputs": inputs + ("+preview" if preview else ""),
        "seconds": seconds,
        "cpu_core_pct_mean": sum(cpus) / len(cpus),
        "cpu_core_pct_max": max(cpus),
        "cpu_machine_pct_mean": sum(cpus) / len(cpus) / cores,
        "rss_mb_mean": sum(rss) / len(rss) / 2 ** 20,
        "rss_mb_peak": max(rss) / 2 ** 20,
        "encode_ms_avg": (sum(encode[-channels:]) / channels) if encode else None,
        "encode_ms_worst": max(worst_encode) if worst_encode else None,
        "fps_min": min(fps[-channels:]) if fps else None,
        "sync": report["offset_us"],
        "verify": "PASS" if not report["failures"] else "FAIL",
        "work": work,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--inputs", choices=["files", "synthetic"], default="files")
    parser.add_argument("--channels", type=int, nargs="+", default=[3, 4])
    parser.add_argument("--preview", choices=["on", "off", "both"], default="off",
                        help="live preview encoding during the run (default off: the recording path alone)")
    parser.add_argument("--json", help="write results as JSON")
    args = parser.parse_args()

    print("machine: %s, %s, %d logical cores, python %s" %
          (platform.platform(), platform.processor() or platform.machine(), os.cpu_count() or 0,
           platform.python_version()))
    results = []
    previews = {"on": [True], "off": [False], "both": [False, True]}[args.preview]
    for channels in args.channels:
        for preview in previews:
            print("running %d channels (%s, preview %s) for %d s ..." %
                  (channels, args.inputs, "on" if preview else "off", args.seconds))
            results.append(run_once(os.path.abspath(args.build_dir), channels, args.inputs, args.seconds, preview))
    print()
    print("| channels | inputs | CPU mean (% of 1 core) | CPU max | CPU mean (% of machine) | RSS mean MB | RSS peak MB"
          " | encode ms avg | encode ms worst | fps min | offset max us | offset p99 us | verify |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for result in results:
        print("| %d | %s | %.0f | %.0f | %.1f | %.0f | %.0f | %.2f | %.2f | %.2f | %.0f | %.0f | %s |" % (
            result["channels"], result["inputs"], result["cpu_core_pct_mean"], result["cpu_core_pct_max"],
            result["cpu_machine_pct_mean"], result["rss_mb_mean"], result["rss_mb_peak"],
            result["encode_ms_avg"] or 0.0, result["encode_ms_worst"] or 0.0, result["fps_min"] or 0.0,
            result["sync"]["max"], result["sync"]["p99"], result["verify"]))
    if args.json:
        with open(args.json, "w") as file:
            json.dump(results, file, indent=2)
    return 0 if all(result["verify"] == "PASS" for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
