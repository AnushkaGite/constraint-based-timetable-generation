# timetable_dataset.csv — Constraint-Based Timetable Generation (DAA project)

## 1. Purpose

One CSV, **input data only** (no timetable, no solution, no results). Your C++17/20 program loads the rows for one `dataset_size`, derives the conflict graph itself, and runs Greedy → MRV → Backtracking → Pruning → Branch & Bound, sequentially and with OpenMP, at four problem sizes.

**One row = one course taught to one batch.**
185 rows total: SMALL 10, MEDIUM 25, LARGE 50, XLARGE 100.

## 2. Time grid (fixed, not in the CSV)

Days `MON TUE WED THU FRI`; slots `1=09-10, 2=10-11, 3=11-12, 4=12-13, (lunch), 5=14-15, 6=15-16, 7=16-17`.
Theory = 1 slot. Lab = 2 consecutive slots. **Valid lab start slots are 1, 2, 3, 5, 6** (start 4 would cross lunch, start 7 would overflow the day).

## 3. Columns

| Column | Meaning |
|---|---|
| `dataset_size` | `SMALL`, `MEDIUM`, `LARGE` or `XLARGE`. Filter on this to load one problem. |
| `course_id` | Unique row key (`SM-001`, `MD-001`, `LG-001`, `XL-001`…). One course-for-one-batch. |
| `course_name` | Subject name (for reporting only). |
| `course_type` | `THEORY` or `LAB`. |
| `faculty_id` | Teacher (`F01`…). Reused across many rows; creates faculty conflicts. |
| `batch_id` | Student batch (`B01`…). Several rows share a batch; creates batch conflicts. |
| `batch_strength` | Number of students. Room capacity must be ≥ this. |
| `sessions_per_week` | Sessions to schedule. Theory 2–4, lab 1–2. |
| `duration` | Slots per session: 1 (theory) or 2 (lab). |
| `room_options` | Candidate rooms, see §5. |
| `faculty_unavailable` | Slots the faculty cannot teach, see §6. `NONE` if free. |
| `batch_unavailable` | Slots the batch cannot attend, see §6. `NONE` if free. |
| `preferred_start_slot`, `preferred_end_slot` | The faculty's preferred slot window (1–7). Soft constraint only. |

Faculty and batch attributes are **denormalised**: every row with the same `faculty_id` carries identical unavailability/preference values, and every row with the same `batch_id` carries the same `batch_strength` and `batch_unavailable` (required by the single-file rule; checked). `faculty_id`, `batch_id`, and room IDs are **scoped to their `dataset_size`** (F01 in SMALL is unrelated to F01 in LARGE).

## 4. Dataset sizes

| | SMALL | MEDIUM | LARGE | XLARGE |
|---|---|---|---|---|
| Courses (rows) | 10 | 25 | 50 | 100 |
| Batches | 3 | 5 | 8 | 15 |
| Faculty | 6 | 10 | 20 | 40 |
| Rooms (theory + lab) | 5 (3+2) | 8 (6+2) | 12 (9+3) | 20 (16+4) |
| Labs | 3 (30%) | 6 (24%) | 13 (26%) | 25 (25%) |
| Sessions to place | 33 | 84 | 161 | 322 |
| Slot-units (sessions × duration) | 39 | 96 | 186 | 365 |
| Avg batch load (of its free slots) | 39% | 58% | 68% | 74% |
| Graph edges (same batch or same faculty) | 15 | 72 | 177 | 390 |
| Graph degree (min / mean / max) | 2 / 3.0 / 4 | 4 / 5.8 / 8 | 5 / 7.1 / 10 | 5 / 7.8 / 11 |
| Graph density | 0.33 | 0.24 | 0.14 | 0.08 |

Difficulty comes from: more sessions, shared faculty (workloads from 1 to 6 courses each), shared batches, big batches that fit only a few rooms, few lab rooms, faculty/batch unavailability, and labs needing two free consecutive slots at valid starts. Note that room scarcity makes the search harder through the *constraint checker*, not through extra graph edges (see §7). Faculty unavailability: about half have none, some have 2–4 slots, a few have 6–9.

Room inventory (capacity), all `R##` = THEORY, `L##` = LAB:

- SMALL: R01 60, R02 60, R03 50 · L01 60, L02 50
- MEDIUM: R01 90, R02 60, R03 60, R04 50, R05 50, R06 40 · L01 70, L02 50
- LARGE: R01 100, R02 90, R03–R05 60, R06–R07 50, R08–R09 40 · L01 70, L02 60, L03 40
- XLARGE: R01 120, R02 100, R03 90, R04 80, R05–R08 60, R09–R12 50, R13–R16 40 · L01 80, L02 70, L03 60, L04 50

You do not need this list; everything is also encoded in `room_options`.

## 5. `room_options`

`ROOMID:CAPACITY:TYPE` entries joined by `|`, e.g.

```
R01:60:THEORY|R02:50:THEORY|R03:40:THEORY
L01:60:LAB|L02:40:LAB
```

- Only rooms of the matching type are listed, and each batch only gets a subset of them (its "wing"), so options are not "every room".
- Options include rooms with capacity down to about 85% of `batch_strength`. Such a room is a **near miss**: the solver must still reject it with `capacity >= batch_strength`. Every course has at least one truly valid room (theory: at least two).
- The same room ID in different rows is the same physical room. Room clashes are **not** conflict-graph edges; the constraint checker detects them (see §7).

## 6. Unavailable slots

`DAY-SLOT` tokens joined by `|`, e.g. `MON-3|WED-5|FRI-6`. `NONE` means no restriction. For a lab, **both** occupied slots must be free of unavailability.

## 7. How the C++ program should interpret it

**Parsing.** Read the CSV, keep rows where `dataset_size == X`, and split the `|` fields (`std::getline` with `'|'`, then `':'` / `'-'`). No field contains commas or quotes. Each course needs `sessions_per_week` sessions, and each session gets `(day, start_slot, room)`. Simple indexing: `t = day*7 + (slot-1)`, which fits a 35-bit mask per faculty/batch/room.

### 7.1 Conflict Graph (derived by your code, not stored in the CSV)

- **Vertex** = one course (one CSV row).
- **Edge** between courses *i* and *j* if and only if they have the **same `batch_id`** OR the **same `faculty_id`**.

That is the whole definition. Sharing possible rooms does **not** create an edge: two courses that can use the same room may still run at the same time if they are assigned different rooms. Edges mean "these two courses can never be in the same time slot", which is why they can be used for greedy ordering, MRV tie-breaking (degree) and pruning.

### 7.2 Constraint Checking (done by the timetable validator / solver, not by graph edges)

| Constraint | How to check |
|---|---|
| Room conflict | A physical room (`room_id`) cannot host two sessions in the same day and slot. Rooms are tracked per slot, independently of the graph. |
| Room capacity | Assigned room capacity ≥ `batch_strength`. |
| Room type | Room `TYPE` equals `course_type` (`THEORY` rooms for theory, `LAB` rooms for labs). The room must also appear in the course's `room_options`. |
| Faculty availability | No session overlaps a slot in `faculty_unavailable`. |
| Batch availability | No session overlaps a slot in `batch_unavailable`. |
| Lab duration | A lab occupies `duration` = 2 consecutive slots in the same day, starting at slot 1, 2, 3, 5 or 6, so it never crosses lunch or the end of the day. |
| Required sessions | Exactly `sessions_per_week` sessions are placed for every course. |
| Faculty clash | One faculty teaches at most one session per slot. (The graph edge says the two courses are related; the checker enforces the per-slot rule on the actual sessions.) |
| Batch clash | One batch attends at most one session per slot. (Same remark.) |

Multi-slot labs must have the faculty, batch and room free for both slots.

### 7.3 Soft constraints (penalties for Branch & Bound)

Batch gaps, faculty gaps, long consecutive runs, sessions of one course on the same day, late slots (for example 6–7), and slots outside `[preferred_start_slot, preferred_end_slot]`.

### 7.4 Suggested heuristics

MRV domain size = remaining valid `(time, room)` pairs for the session. Course degree in the conflict graph (7.1) is a good tie-breaker for most-constrained-first ordering. Compare sequential and OpenMP versions on the same rows per size and report speedup `T_seq/T_par` and efficiency `speedup/threads`.

## 8. Feasibility and difficulty checks performed

- Every size was solved by a separate search that checks all hard constraints of §7.2 (the room, availability and lab checks are done on actual sessions, not through graph edges); the resulting timetables were re-verified against the parsed CSV (no double-booking, unavailability respected, capacity/type, lab start slots, session counts). So **all four sizes have at least one feasible timetable.**
- A naive first-fit greedy (no backtracking) leaves sessions unplaced: MEDIUM 2, LARGE 9, XLARGE 14 (SMALL 0). A dynamic-MRV greedy still fails on LARGE (2) and XLARGE (6). LARGE and XLARGE needed genuine backtracking/restarts to find a full timetable, while SMALL is solved without any backtracking.
- Other checks: IDs are consistent, faculty/batch attributes are identical on every row that shares an ID, every faculty teaches at least one course, durations and session counts are valid, theory rows only list theory rooms and labs only lab rooms, and the batch/faculty conflict graph is sparse (density 0.33 down to 0.08), is not complete, and its vertex degrees spread over a range (for example 5–11 in XLARGE), with low-degree courses (single-course faculty) and higher-degree ones (heavily loaded faculty in larger batches).
- The feasibility and greedy-failure results do not depend on how the graph is defined, so the room-edge correction did not change them.
- Limits: the dataset is tuned for feasibility plus hardness, not proven hardest. If you change the load (more sessions or fewer rooms), re-check feasibility.
