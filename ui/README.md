# Timetable Solver Dashboard — Phase 4

Interactive web dashboard and live visualizer for the **Constraint-Based Timetable Generation** DAA project (C++17 + OpenMP).

---

## 1. Quick Start

### Prerequisites
- Windows 10/11
- Python 3.9+ (standard library only; no pip packages needed)
- C++ compiler (`g++` with OpenMP support, e.g. MSYS2 UCRT64)

### One-Click Launch
Run the PowerShell launcher script:
```powershell
.\ui\run_ui.ps1
```
This checks the environment, builds the solver if missing, starts the local server, and opens your browser at:
**`http://127.0.0.1:8000`**

### Manual Server Launch
```powershell
python ui\server.py --port 8000
```

---

## 2. Features & Architecture

### Tab 1: Run & Timetable
- **Live Solver Execution**: Run Greedy, MRV, Sequential B&B, or OpenMP Parallel B&B with custom thread counts and time limits.
- **Computed Feasibility Badge**: 
  - 🟢 **FEASIBLE**: Complete timetable with 0 hard violations.
  - 🟡 **PARTIAL**: Constructive heuristic placed some sessions before resource exhaustion.
  - 🔴 **NO TIMETABLE**: Search exhausted time budget before finding a feasible leaf node.
- **Detailed Constraint Breakdown**: Expandable drawer with pass/fail indicators across all 8 hard constraints.
- **Interactive Schedule Matrix**: 5-day (MON–FRI) $\times$ 7-slot matrix with clear lunch divider (slots 4–5), multi-hour lab row spanning, and categorical color-coding. Filterable by Batch, Faculty, Room, or All.
- **Side Inspector Drawer**: Click any session chip to inspect batch strength vs. room capacity, faculty ID, and duration.
- **Unscheduled Sessions Panel**: Lists courses unable to find conflict-free slots.

### Tab 2: Benchmarks
- Reads and deduplicates `cpp/benchmark_results.csv` keeping the latest run per configuration.
- **Interactive Log-Scale Visualizations**:
  - Runtime by Algorithm (with hatched styling for timed-out runs).
  - Search Nodes Explored.
  - Soft-Constraint Penalty (lower is better).
  - Session Completion Ratio.
  - Parallel vs. Sequential B&B Throughput (nodes/sec) & Speedup.
- **Sortable Historical Table** with one-click CSV export.

### Tab 3: Scaling & Amdahl's Law
- **Thread Scaling Sweep**: Automates sequential baseline followed by multi-threaded executions.
- **Throughput Speedup**: Computes $S(p) = \frac{\text{Throughput}(p)}{\text{Throughput}(1)}$ and Parallel Efficiency $E(p) = \frac{S(p)}{p}$.
- **Amdahl Curve Fit**: Fits parallel fraction $f$ and plots theoretical bounds, annotating super-linear scaling effects.

---

## 3. REST API Reference

| Endpoint | Method | Description |
| :--- | :---: | :--- |
| `/api/health` | `GET` | Health check: solver `.exe`, CSV status, CPU cores, Python version. |
| `/api/run` | `POST` | Dispatches live solver job. Body: `{size, algo, threads, timeLimitMs}`. |
| `/api/jobs/<id>` | `GET` | Queries real-time status (`running`, `done`, `error`, `cancelled`), elapsed time, and logs. |
| `/api/jobs/<id>/cancel` | `POST` | Terminate process tree (`taskkill /T /F`). |
| `/api/runs` | `GET` | Lists previously saved JSON runs (newest first). |
| `/api/runs/<runId>` | `GET` | Retrieves full JSON for a specific run. |
| `/api/benchmarks` | `GET` | Parses & deduplicates `benchmark_results.csv` with derived throughput metrics. |
| `/api/sweep` | `POST` | Runs a multi-thread scaling sweep. Body: `{size, threadList, timeLimitMs}`. |

---

## 4. Screenshots

<!-- Screenshots will be saved here -->
- **Timetable View (SMALL - Greedy)**: `ui/docs/screenshots/tab1_timetable.png`
- **Partial Timetable (LARGE - Greedy)**: `ui/docs/screenshots/tab1_partial.png`
- **Benchmark Analytics**: `ui/docs/screenshots/tab2_benchmarks.png`
- **Amdahl Scaling Curves**: `ui/docs/screenshots/tab3_scaling.png`
