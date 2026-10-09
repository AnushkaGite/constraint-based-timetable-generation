#!/usr/bin/env python3
"""
ui/server.py — Local Dashboard Backend for Constraint-Based Timetable Generation
Python 3.9+ standard library only.
Serves static frontend at ui/public/ and JSON REST API at /api/*
"""

import sys
import os
import json
import time
import argparse
import platform
import subprocess
import threading
from pathlib import Path
from http import HTTPStatus
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import urllib.parse

# ── Paths ─────────────────────────────────────────────────────────────
BASE_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = BASE_DIR.parent
CPP_DIR = PROJECT_ROOT / "cpp"
RUNS_DIR = BASE_DIR / "data" / "runs"
PUBLIC_DIR = BASE_DIR / "public"
BENCH_CSV = CPP_DIR / "benchmark_results.csv"

RUNS_DIR.mkdir(parents=True, exist_ok=True)
PUBLIC_DIR.mkdir(parents=True, exist_ok=True)

def find_solver_exe() -> Path:
    candidates = [
        CPP_DIR / "timetable_solver.exe",
        CPP_DIR / "timetable_solver",
        PROJECT_ROOT / "timetable_solver.exe",
        PROJECT_ROOT / "timetable_solver",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return CPP_DIR / "timetable_solver.exe"

def find_dataset_csv() -> Path:
    candidates = [
        PROJECT_ROOT / "timetable_dataset.csv",
        CPP_DIR / "timetable_dataset.csv",
        PROJECT_ROOT / "data" / "timetable_dataset.csv",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return PROJECT_ROOT / "timetable_dataset.csv"


# ── Job Manager ───────────────────────────────────────────────────────
class JobManager:
    def __init__(self):
        self._lock = threading.RLock()
        self.jobs = {}
        self.active_job_id = None

    def is_busy(self) -> bool:
        with self._lock:
            if not self.active_job_id:
                return False
            active = self.jobs.get(self.active_job_id)
            return active is not None and active.get("status") == "running"

    def start_job(self, job_type: str, metadata: dict) -> str:
        with self._lock:
            if self.is_busy():
                return None
            job_id = f"job_{int(time.time() * 1000)}"
            self.jobs[job_id] = {
                "id": job_id,
                "type": job_type,
                "status": "running",
                "startTime": time.time(),
                "elapsedMs": 0.0,
                "runId": None,
                "runIds": [],
                "stderrTail": "",
                "currentStep": 0,
                "totalSteps": 1,
                "stepDesc": "Starting...",
                "error": None,
                "process": None,
                "metadata": metadata,
            }
            self.active_job_id = job_id
            return job_id

    def update_job(self, job_id: str, **kwargs):
        with self._lock:
            if job_id in self.jobs:
                self.jobs[job_id].update(kwargs)
                if kwargs.get("status") in ("done", "error", "cancelled"):
                    if self.active_job_id == job_id:
                        self.active_job_id = None

    def get_job(self, job_id: str):
        with self._lock:
            job = self.jobs.get(job_id)
            if not job:
                return None
            copy = dict(job)
            if copy.get("status") == "running":
                copy["elapsedMs"] = (time.time() - copy["startTime"]) * 1000.0
            copy.pop("process", None)
            return copy

    def cancel_job(self, job_id: str) -> bool:
        with self._lock:
            job = self.jobs.get(job_id)
            if not job or job.get("status") != "running":
                return False
            proc = job.get("process")
            job["status"] = "cancelled"
            job["error"] = "Cancelled by user"
            if self.active_job_id == job_id:
                self.active_job_id = None

        if proc and proc.poll() is None:
            try:
                if platform.system() == "Windows":
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                                   capture_output=True, check=False)
                else:
                    proc.kill()
            except Exception as e:
                print(f"[WARN] Error killing process: {e}", file=sys.stderr)
        return True

job_manager = JobManager()


# ── Subprocess Runner Thread ──────────────────────────────────────────
def run_solver_process(job_id: str, size: str, algo: str, threads: int, time_limit_ms: int,
                       output_json: Path, run_id: str):
    exe = find_solver_exe()
    if not exe.is_file():
        job_manager.update_job(job_id, status="error",
                               error=f"Executable not found at {exe}. Please compile first.")
        return

    cmd = [
        str(exe),
        size,
        "--algo", algo,
        "--time-limit", str(time_limit_ms),
        "--export-json", str(output_json),
    ]
    if algo == "pbnb" and threads > 0:
        cmd.extend(["--threads", str(threads)])

    t0 = time.time()
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(CPP_DIR),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            universal_newlines=True
        )
        job_manager.update_job(job_id, process=proc, stepDesc=f"Running {algo.upper()} on {size}...")

        timeout_sec = (time_limit_ms / 1000.0) + 20.0

        # Wait with watchdog
        while proc.poll() is None:
            time.sleep(0.1)
            elapsed = time.time() - t0
            curr_job = job_manager.get_job(job_id)
            if curr_job and curr_job.get("status") == "cancelled":
                return
            if elapsed > timeout_sec:
                if platform.system() == "Windows":
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                                   capture_output=True, check=False)
                else:
                    proc.kill()
                job_manager.update_job(job_id, status="error",
                                       error=f"Process exceeded hard safety watchdog limit of {timeout_sec:.1f}s")
                return

        stdout_data, stderr_data = proc.communicate()
        elapsed_ms = (time.time() - t0) * 1000.0

        if proc.returncode != 0:
            tail = (stderr_data or stdout_data or "")[-1000:]
            job_manager.update_job(job_id, status="error", elapsedMs=elapsed_ms,
                                   stderrTail=tail,
                                   error=f"Solver exited with code {proc.returncode}")
            return

        if not output_json.is_file():
            tail = (stdout_data or "")[-1000:]
            job_manager.update_job(job_id, status="error", elapsedMs=elapsed_ms,
                                   stderrTail=tail,
                                   error="Solver finished but output JSON was not produced.")
            return

        job_manager.update_job(job_id, status="done", elapsedMs=elapsed_ms,
                               runId=run_id, stderrTail=stderr_data[-500:] if stderr_data else "")

    except Exception as exc:
        job_manager.update_job(job_id, status="error", error=str(exc))


def run_sweep_thread(job_id: str, size: str, thread_list: list, time_limit_ms: int):
    exe = find_solver_exe()
    if not exe.is_file():
        job_manager.update_job(job_id, status="error",
                               error=f"Executable not found at {exe}. Please compile first.")
        return

    total_steps = 1 + len(thread_list)
    job_manager.update_job(job_id, totalSteps=total_steps, currentStep=0)
    run_ids = []

    # Step 0: Baseline BNB
    job_manager.update_job(job_id, currentStep=1,
                           stepDesc=f"Running Sequential B&B baseline on {size}...")
    ts = int(time.time() * 1000)
    bnb_run_id = f"{size}_bnb_t1_{ts}"
    bnb_json = RUNS_DIR / f"{bnb_run_id}.json"

    cmd_bnb = [
        str(exe), size, "--algo", "bnb", "--threads", "1",
        "--time-limit", str(time_limit_ms), "--export-json", str(bnb_json)
    ]

    t0 = time.time()
    try:
        proc = subprocess.Popen(cmd_bnb, cwd=str(CPP_DIR), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        job_manager.update_job(job_id, process=proc)
        proc.communicate(timeout=(time_limit_ms / 1000.0) + 20.0)
        if proc.returncode == 0 and bnb_json.is_file():
            run_ids.append(bnb_run_id)
    except Exception as e:
        job_manager.update_job(job_id, status="error", error=f"Sequential B&B failed: {e}")
        return

    curr_job = job_manager.get_job(job_id)
    if curr_job and curr_job.get("status") == "cancelled":
        return

    # Steps 1..N: PBNB at each thread count
    step_num = 1
    for t in thread_list:
        step_num += 1
        curr_job = job_manager.get_job(job_id)
        if curr_job and curr_job.get("status") == "cancelled":
            return

        job_manager.update_job(job_id, currentStep=step_num,
                               stepDesc=f"Running Parallel B&B ({t} threads) on {size}...")
        ts = int(time.time() * 1000)
        pbnb_run_id = f"{size}_pbnb_t{t}_{ts}"
        pbnb_json = RUNS_DIR / f"{pbnb_run_id}.json"

        cmd_pbnb = [
            str(exe), size, "--algo", "pbnb", "--threads", str(t),
            "--time-limit", str(time_limit_ms), "--export-json", str(pbnb_json)
        ]
        try:
            proc = subprocess.Popen(cmd_pbnb, cwd=str(CPP_DIR), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            job_manager.update_job(job_id, process=proc)
            proc.communicate(timeout=(time_limit_ms / 1000.0) + 20.0)
            if proc.returncode == 0 and pbnb_json.is_file():
                run_ids.append(pbnb_run_id)
        except Exception as e:
            job_manager.update_job(job_id, status="error", error=f"Parallel B&B (t={t}) failed: {e}")
            return

    job_manager.update_job(job_id, status="done", runIds=run_ids,
                           stepDesc="Sweep completed successfully.")


# ── Benchmark CSV Parser & Deduplicator ──────────────────────────────
def parse_benchmark_csv():
    if not BENCH_CSV.is_file():
        return []

    rows = []
    header = None
    with open(BENCH_CSV, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            if header is None:
                header = parts
                continue
            if len(parts) != len(header):
                continue
            row_dict = dict(zip(header, parts))
            rows.append(row_dict)

    def to_int(v):
        try:
            return None if v in ("N/A", "", None) else int(float(v))
        except (ValueError, TypeError):
            return None

    def to_float(v):
        try:
            return None if v in ("N/A", "", None) else float(v)
        except (ValueError, TypeError):
            return None

    # Deduplicate keeping LAST row per (Dataset, Algorithm, Threads)
    dedup_map = {}
    for r in rows:
        ds = r.get("Dataset", "").upper()
        algo = r.get("Algorithm", "").upper()
        thr = to_int(r.get("Threads")) or 1
        key = (ds, algo, thr)

        runtime = to_float(r.get("Runtime_ms"))
        nodes = to_int(r.get("Nodes"))
        nodes_sec = None
        if nodes is not None and runtime is not None and runtime > 0:
            nodes_sec = round(nodes / (runtime / 1000.0), 2)

        eff = to_float(r.get("Efficiency"))

        typed_row = {
            "dataset": ds,
            "algorithm": algo,
            "threads": thr,
            "courses": to_int(r.get("Courses")),
            "sessions": to_int(r.get("Sessions")),
            "scheduled": to_int(r.get("Scheduled")),
            "feasible": (r.get("Feasible", "").upper() == "YES"),
            "timedOut": (r.get("TimedOut", "").upper() == "YES"),
            "runtimeMs": runtime,
            "nodes": nodes,
            "attempts": to_int(r.get("Attempts")),
            "backtracks": to_int(r.get("Backtracks")),
            "pruned": to_int(r.get("Pruned")),
            "penalty": to_int(r.get("Penalty")),
            "sequentialRuntimeMs": to_float(r.get("Sequential_Runtime_ms")),
            "parallelRuntimeMs": to_float(r.get("Parallel_Runtime_ms")),
            "speedup": to_float(r.get("Speedup")),
            "efficiency": eff,
            "nodesPerSec": nodes_sec,
        }
        dedup_map[key] = typed_row

    # Preserve consistent dataset/algorithm order
    order_ds = {"SMALL": 1, "MEDIUM": 2, "LARGE": 3, "XLARGE": 4}
    order_algo = {"GREEDY": 1, "MRV": 2, "BNB": 3, "PBNB": 4}

    result = list(dedup_map.values())
    result.sort(key=lambda x: (
        order_ds.get(x["dataset"], 99),
        order_algo.get(x["algorithm"], 99),
        x["threads"]
    ))
    return result


# ── HTTP Request Handler ──────────────────────────────────────────────
class DashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(PUBLIC_DIR), **kwargs)

    def log_message(self, format, *args):
        # Concise logging
        sys.stderr.write(f"[{self.log_date_time_string()}] {self.address_string()} {format%args}\n")

    def send_json(self, data, status=HTTPStatus.OK):
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, message: str, status=HTTPStatus.BAD_REQUEST):
        self.send_json({"error": message, "status": status.value}, status=status)

    def do_OPTIONS(self):
        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/health":
            exe = find_solver_exe()
            csv_file = find_dataset_csv()
            self.send_json({
                "status": "ok",
                "exeFound": exe.is_file(),
                "exePath": str(exe),
                "csvFound": csv_file.is_file(),
                "csvPath": str(csv_file),
                "benchCsvFound": BENCH_CSV.is_file(),
                "cpuCount": os.cpu_count() or 4,
                "pythonVersion": platform.python_version(),
                "platform": platform.platform(),
                "buildHelp": (
                    "cd cpp; g++ -std=c++17 -O2 -fopenmp main.cpp timetable.cpp "
                    "constraint_checker.cpp greedy.cpp mrv.cpp bnb.cpp bnb_parallel.cpp "
                    "-o timetable_solver.exe"
                ) if not exe.is_file() else None
            })
            return

        elif path == "/api/benchmarks":
            data = parse_benchmark_csv()
            self.send_json(data)
            return

        elif path == "/api/runs":
            runs = []
            for jf in RUNS_DIR.glob("*.json"):
                try:
                    with open(jf, "r", encoding="utf-8") as f:
                        content = json.load(f)
                    meta = content.get("meta", {})
                    metrics = content.get("metrics", {})
                    validation = content.get("validation", {})
                    runs.append({
                        "id": jf.stem,
                        "filename": jf.name,
                        "dataset": meta.get("dataset"),
                        "algorithm": meta.get("algorithm"),
                        "threads": meta.get("threads"),
                        "timeLimitMs": meta.get("timeLimitMs"),
                        "feasible": metrics.get("feasible"),
                        "timedOut": metrics.get("timedOut"),
                        "runtimeMs": metrics.get("runtimeMs"),
                        "scheduled": metrics.get("scheduled"),
                        "sessions": metrics.get("sessions"),
                        "penalty": metrics.get("penalty"),
                        "nodes": metrics.get("nodes"),
                        "mtime": jf.stat().st_mtime,
                        "dateStr": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(jf.stat().st_mtime))
                    })
                except Exception as e:
                    continue
            runs.sort(key=lambda r: r["mtime"], reverse=True)
            self.send_json(runs)
            return

        elif path.startswith("/api/runs/"):
            run_id = path[len("/api/runs/"):]
            # Whitelist filename check
            if not run_id.replace("_", "").replace("-", "").isalnum():
                self.send_error_json("Invalid run ID format", HTTPStatus.BAD_REQUEST)
                return
            target = RUNS_DIR / f"{run_id}.json"
            if not target.is_file():
                self.send_error_json("Run not found", HTTPStatus.NOT_FOUND)
                return
            try:
                with open(target, "r", encoding="utf-8") as f:
                    content = json.load(f)
                self.send_json(content)
            except Exception as e:
                self.send_error_json(f"Error reading run JSON: {e}", HTTPStatus.INTERNAL_SERVER_ERROR)
            return

        elif path.startswith("/api/jobs/"):
            job_id = path[len("/api/jobs/"):]
            info = job_manager.get_job(job_id)
            if not info:
                self.send_error_json("Job not found", HTTPStatus.NOT_FOUND)
                return
            self.send_json(info)
            return

        # Fallback to static files
        super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        # Read JSON body
        content_length = int(self.headers.get("Content-Length", 0))
        body = {}
        if content_length > 0:
            try:
                raw = self.rfile.read(content_length)
                body = json.loads(raw.decode("utf-8"))
            except Exception as e:
                self.send_error_json(f"Malformed JSON body: {e}", HTTPStatus.BAD_REQUEST)
                return

        if path == "/api/run":
            if job_manager.is_busy():
                self.send_error_json("Another solver run is active. Please wait or cancel it.",
                                     HTTPStatus.CONFLICT)
                return

            size = str(body.get("size", "SMALL")).upper()
            algo = str(body.get("algo", "greedy")).lower()
            try:
                threads = int(body.get("threads", 4))
                time_limit_ms = int(body.get("timeLimitMs", 30000))
            except (ValueError, TypeError):
                self.send_error_json("Invalid numeric parameter for threads or timeLimitMs",
                                     HTTPStatus.BAD_REQUEST)
                return

            # Whitelist validations
            if size not in ("SMALL", "MEDIUM", "LARGE", "XLARGE"):
                self.send_error_json("Invalid size. Must be SMALL, MEDIUM, LARGE, or XLARGE",
                                     HTTPStatus.BAD_REQUEST)
                return
            if algo not in ("greedy", "mrv", "bnb", "pbnb"):
                self.send_error_json("Invalid algo. Must be greedy, mrv, bnb, or pbnb",
                                     HTTPStatus.BAD_REQUEST)
                return
            if not (1 <= threads <= 64):
                self.send_error_json("threads must be between 1 and 64", HTTPStatus.BAD_REQUEST)
                return
            if not (100 <= time_limit_ms <= 300000):
                self.send_error_json("timeLimitMs must be between 100 and 300000",
                                     HTTPStatus.BAD_REQUEST)
                return

            timestamp = int(time.time() * 1000)
            run_id = f"{size}_{algo}_t{threads}_{timestamp}"
            output_json = RUNS_DIR / f"{run_id}.json"

            job_id = job_manager.start_job("run", {
                "size": size,
                "algo": algo,
                "threads": threads,
                "timeLimitMs": time_limit_ms,
                "runId": run_id
            })
            if not job_id:
                self.send_error_json("Concurrent job race detected", HTTPStatus.CONFLICT)
                return

            t = threading.Thread(
                target=run_solver_process,
                args=(job_id, size, algo, threads, time_limit_ms, output_json, run_id),
                daemon=True
            )
            t.start()

            self.send_json({"jobId": job_id, "runId": run_id}, HTTPStatus.ACCEPTED)
            return

        elif path.startswith("/api/jobs/") and path.endswith("/cancel"):
            # /api/jobs/<id>/cancel
            parts = path.strip("/").split("/")
            if len(parts) == 4 and parts[1] == "jobs" and parts[3] == "cancel":
                job_id = parts[2]
                ok = job_manager.cancel_job(job_id)
                if not ok:
                    self.send_error_json("Job not running or not found", HTTPStatus.NOT_FOUND)
                    return
                self.send_json({"status": "cancelled", "jobId": job_id})
                return
            else:
                self.send_error_json("Invalid cancel URL", HTTPStatus.BAD_REQUEST)
                return

        elif path == "/api/sweep":
            if job_manager.is_busy():
                self.send_error_json("Another solver run is active. Please wait or cancel it.",
                                     HTTPStatus.CONFLICT)
                return

            size = str(body.get("size", "SMALL")).upper()
            thread_list = body.get("threadList", [1, 2, 4, 8])
            try:
                time_limit_ms = int(body.get("timeLimitMs", 5000))
            except (ValueError, TypeError):
                self.send_error_json("Invalid timeLimitMs", HTTPStatus.BAD_REQUEST)
                return

            if size not in ("SMALL", "MEDIUM", "LARGE", "XLARGE"):
                self.send_error_json("Invalid size", HTTPStatus.BAD_REQUEST)
                return
            if not isinstance(thread_list, list) or not thread_list:
                self.send_error_json("threadList must be a non-empty list of integers",
                                     HTTPStatus.BAD_REQUEST)
                return
            cleaned_threads = []
            for t in thread_list:
                try:
                    it = int(t)
                    if 1 <= it <= 64:
                        cleaned_threads.append(it)
                except (ValueError, TypeError):
                    pass
            if not cleaned_threads:
                self.send_error_json("No valid thread counts provided (1..64)",
                                     HTTPStatus.BAD_REQUEST)
                return
            if not (100 <= time_limit_ms <= 300000):
                self.send_error_json("timeLimitMs must be between 100 and 300000",
                                     HTTPStatus.BAD_REQUEST)
                return

            job_id = job_manager.start_job("sweep", {
                "size": size,
                "threadList": cleaned_threads,
                "timeLimitMs": time_limit_ms
            })
            if not job_id:
                self.send_error_json("Concurrent job race detected", HTTPStatus.CONFLICT)
                return

            t = threading.Thread(
                target=run_sweep_thread,
                args=(job_id, size, cleaned_threads, time_limit_ms),
                daemon=True
            )
            t.start()

            self.send_json({"jobId": job_id}, HTTPStatus.ACCEPTED)
            return

        self.send_error_json("Endpoint not found", HTTPStatus.NOT_FOUND)


# ── Main Entry Point ──────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Live Timetable Solver Web Server")
    parser.add_argument("--port", type=int, default=8000, help="Port to bind (default: 8000)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Host interface (default: 127.0.0.1)")
    args = parser.parse_args()

    server_address = (args.host, args.port)
    httpd = ThreadingHTTPServer(server_address, DashboardHandler)

    print(f"============================================================")
    print(f" Timetable Solver Dashboard Server")
    print(f" Serving UI from : {PUBLIC_DIR}")
    print(f" Solver EXE      : {find_solver_exe()}")
    print(f" URL             : http://{args.host}:{args.port}")
    print(f"============================================================")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[INFO] Server stopped by user.")
        httpd.server_close()

if __name__ == "__main__":
    main()
