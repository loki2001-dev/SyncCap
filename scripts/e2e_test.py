#!/usr/bin/env python3
"""End-to-end test: broker + OpenAPI server + acquisition system as real processes.

    python scripts/e2e_test.py [--build-dir build] [--mosquitto <path to mosquitto>] [--inputs synthetic|files]

Without --mosquitto a broker must already listen on 127.0.0.1:1883 (for example `docker compose up mosquitto`).
Verdicts come from what the server actually received and from the recorded files, not from log lines.

Scenario
  1. server healthy, collector starts recording (autostart)
  2. GET /status: 4 channels, sync locked, status received over MQTT and over HTTP POST
  3. POST /control stop  -> ack accepted, acquisition idle
  4. POST /control stop  -> ack rejected_not_running
  5. POST /control start -> ack accepted, acquisition running, a second session directory
  6. collector stopped by signal -> exit code 0, server sees collector_online=false
  7. verify_sync.py passes on both sessions (offsets <= 2 ms, channel pictures aligned, SPS+IDR per file)
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import verify_sync  # noqa: E402

SERVER = "http://127.0.0.1:8080"
IS_WINDOWS = os.name == "nt"


class Failure(Exception):
    pass


def http(method, path, body=None, timeout=5.0):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(SERVER + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            text = response.read().decode()
            return response.status, (json.loads(text) if text else None)
    except urllib.error.HTTPError as error:
        text = error.read().decode()
        return error.code, (json.loads(text) if text else None)


def wait_for(description, predicate, timeout_s, interval_s=0.5):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        try:
            result = predicate()
            if result:
                return result
            last = "condition false"
        except (urllib.error.URLError, ConnectionError, OSError, KeyError, TypeError) as error:
            last = repr(error)
        time.sleep(interval_s)
    raise Failure("timed out after %ss waiting for %s (%s)" % (timeout_s, description, last))


def start_process(command, log_path):
    log = open(log_path, "w")
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if IS_WINDOWS else 0
    process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    process.log = log
    return process


def stop_process(process, timeout_s=25):
    if process is None or process.poll() is not None:
        return process.returncode if process else None
    process.send_signal(signal.CTRL_BREAK_EVENT if IS_WINDOWS else signal.SIGTERM)
    try:
        return process.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise Failure("process %s did not stop within %ss" % (process.args[0], timeout_s))
    finally:
        process.log.close()


def executable(build_dir, name):
    path = os.path.join(build_dir, name + (".exe" if IS_WINDOWS else ""))
    if not os.path.exists(path):
        raise Failure("missing %s (build first)" % path)
    return path


def make_collector_config(inputs, recordings, logs):
    with open(os.path.join(ROOT, "config", "collector.json")) as file:
        config = json.load(file)
    if inputs == "synthetic":
        config["video"]["channels"] = [{"id": index, "type": "synthetic"} for index in range(4)]
    config["storage"] = {"root": recordings, "segment_seconds": 10}
    config["sensor"] = {"type": "simulated", "period_ms": 100, "seed": 42}
    config["report"]["interval_s"] = 20
    config["log"] = {"dir": logs, "level": "info"}
    handle, path = tempfile.mkstemp(prefix="synccap_e2e_", suffix=".json")
    with os.fdopen(handle, "w") as file:
        json.dump(config, file, indent=2)
    return path


def control(command, expect_result, expect_reason=None):
    status, record = http("POST", "/control", {"command": command})
    if status != 202:
        raise Failure("POST /control %s answered %s %s" % (command, status, record))
    request_id = record["request_id"]

    def answered():
        code, body = http("GET", "/control/" + request_id)
        return body if code == 200 and body["result"] != "pending" else None

    body = wait_for("ack of %s" % request_id, answered, 10)
    if body["result"] != expect_result or (expect_reason and body["reason"] != expect_reason):
        raise Failure("control %s expected %s/%s got %s" % (command, expect_result, expect_reason, body))
    print("  control %-5s -> %s %s" % (command, body["result"], body["reason"]))
    return body


def acquisition_state():
    code, body = http("GET", "/status")
    return body["acquisition"]["state"] if code == 200 else None


def run(args):
    build_dir = os.path.abspath(args.build_dir)
    work = os.path.join(ROOT, "e2e_output")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    recordings = os.path.join(work, "recordings")
    processes = {}
    try:
        if args.mosquitto:
            processes["broker"] = start_process([args.mosquitto, "-p", "1883"], os.path.join(work, "mosquitto.log"))
            time.sleep(1.0)
        processes["server"] = start_process([executable(build_dir, "synccap_server"), "config/server.json"],
                                            os.path.join(work, "server.log"))
        wait_for("server health", lambda: http("GET", "/health")[1]["mqtt_connected"], 15)
        print("1 server healthy and connected to the broker")

        config_path = make_collector_config(args.inputs, recordings, os.path.join(work, "logs"))
        processes["collector"] = start_process([executable(build_dir, "synccap_collector"), "--config", config_path],
                                               os.path.join(work, "collector.log"))

        def running_and_locked():
            code, body = http("GET", "/status")
            if code != 200 or body["acquisition"]["state"] != "running":
                return None
            if len(body["channel_status"]) != 4 or not body["sync_status"]["locked"]:
                return None
            # fps is 0 until the first full measurement window after start
            if any(channel["fps"] <= 0.0 for channel in body["channel_status"]):
                return None
            return body

        body = wait_for("running and locked status", running_and_locked, 30)
        fps = [round(channel["fps"], 2) for channel in body["channel_status"]]
        print("2 status running: fps=%s max_offset_ms=%s via=%s" %
              (fps, body["sync_status"]["max_offset_ms"], body["server_meta"]["received_via"]))
        if any(value < 20.0 or value > 30.5 for value in fps):
            raise Failure("channel fps outside 20-30: %s" % fps)
        wait_for("a status over HTTP POST", lambda: http("GET", "/status")[1]["server_meta"]["received_http"] >= 1, 15)
        print("  status also received over HTTP POST /status")

        time.sleep(args.record_s)
        print("3 stop")
        control("stop", "accepted")
        wait_for("idle", lambda: acquisition_state() == "idle", 15)
        print("4 stop again")
        control("stop", "rejected", "rejected_not_running")
        print("5 start")
        control("start", "accepted")
        wait_for("running again", lambda: acquisition_state() == "running", 15)
        time.sleep(args.record_s)

        print("6 collector shutdown by signal")
        code = stop_process(processes.pop("collector"))
        if code != 0:
            raise Failure("collector exit code %s" % code)
        wait_for("collector offline", lambda: http("GET", "/status")[1]["server_meta"]["collector_online"] is False, 10)
        print("  exit code 0, server sees collector_online=false")

        print("7 offline verification")
        sessions = sorted(os.path.join(recordings, name) for name in os.listdir(recordings))
        if len(sessions) != 2:
            raise Failure("expected 2 sessions, found %s" % sessions)
        for session in sessions:
            report = verify_sync.verify_session(session, 2.0)
            verify_sync.print_report(report)
            if report["failures"]:
                raise Failure("verification failed for %s" % session)
        print("E2E PASS")
        return 0
    except Failure as failure:
        print("E2E FAIL: %s" % failure)
        print("logs in %s" % work)
        return 1
    finally:
        for name in ("collector", "server", "broker"):
            if name in processes:
                try:
                    stop_process(processes[name])
                except Failure as failure:
                    print("warning: %s" % failure)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    parser.add_argument("--mosquitto", help="start this mosquitto binary on port 1883")
    parser.add_argument("--inputs", choices=["synthetic", "files"], default="synthetic")
    parser.add_argument("--record-s", type=float, default=12.0, help="seconds to record per session")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
