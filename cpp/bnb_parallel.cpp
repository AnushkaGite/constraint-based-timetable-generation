// bnb_parallel.cpp — OpenMP parallel Branch & Bound
//
// Strategy: "root splitting"
//   1. Use MRV to choose the most-constrained unassigned session.
//   2. Generate all valid (day,slot,room) candidates for that session.
//   3. Distribute those candidates across OpenMP threads via a dynamic
//      parallel-for loop.
//   4. Each thread runs a SEQUENTIAL sub-tree search (identical to the
//      sequential B&B in bnb.cpp) from its assigned top-level candidate.
//   5. A shared atomic<int> bestCost (read without lock for pruning) and
//      an omp_lock-protected bestSolution allow safe cross-thread pruning
//      and solution updates.
//
// The sequential B&B in bnb.cpp is NOT modified.

#include "timetable.h"
#include <omp.h>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <climits>
#include <array>
#include <numeric>

// ═══════════════════════════════════════════════════════════════════
// Local slot tables and helpers (mirrors bnb.cpp, prefixed PB_)
// ═══════════════════════════════════════════════════════════════════

static const std::vector<int> PB_LAB_SLOTS    = {1, 2, 3, 5, 6};
static const std::vector<int> PB_THEORY_SLOTS = {1, 2, 3, 4, 5, 6, 7};

static int pbDayIdx(const std::string& d) {
    if (d == "MON") return 0;
    if (d == "TUE") return 1;
    if (d == "WED") return 2;
    if (d == "THU") return 3;
    return 4; // FRI
}

// ═══════════════════════════════════════════════════════════════════
// PBStore — per-day indexed committed sessions (thread-local copy)
// ═══════════════════════════════════════════════════════════════════
struct PBStore {
    std::array<std::vector<Session>, 5> byDay;
    std::vector<int>                    dayStack;

    void push(const Session& s) {
        int d = pbDayIdx(s.day);
        byDay[d].push_back(s);
        dayStack.push_back(d);
    }
    void pop() {
        if (dayStack.empty()) return;
        byDay[dayStack.back()].pop_back();
        dayStack.pop_back();
    }
    std::vector<Session> flat() const {
        std::vector<Session> out;
        for (const auto& bkt : byDay)
            for (const auto& s : bkt) out.push_back(s);
        return out;
    }
};

// ═══════════════════════════════════════════════════════════════════
// pbCandidateValid — hard-constraint check (mirrors bnbCandidateValid)
// ═══════════════════════════════════════════════════════════════════
static bool pbCandidateValid(
    const Session&    sess,
    const std::string& day,
    int               slot,
    const std::string& roomId,
    const Course&     course,
    const PBStore&    store,
    const std::unordered_map<std::string, Course>& courseMap)
{
    int dur = course.duration;

    if (!isValidDay(day)) return false;
    if (slot < 1 || (slot + dur - 1) > SLOTS_PER_DAY) return false;

    if (course.courseType == "LAB"    && dur != 2)               return false;
    if (course.courseType == "THEORY" && dur != 1)               return false;
    if (course.courseType == "LAB"    && slot == LUNCH_BREAK_AFTER) return false;

    const RoomOption* ro = nullptr;
    for (const auto& r : course.roomOptions)
        if (r.roomId == roomId) { ro = &r; break; }
    if (!ro)                                   return false;
    if (ro->roomType != course.courseType)     return false;
    if (ro->capacity < course.batchStrength)   return false;

    for (int s = slot; s < slot + dur; ++s) {
        if (course.facultyUnavailable.count({day, s})) return false;
        if (course.batchUnavailable.count({day, s}))   return false;
    }

    int di = pbDayIdx(day);
    for (const auto& other : store.byDay[di]) {
        auto oit = courseMap.find(other.courseId);
        if (oit == courseMap.end()) continue;
        const Course& oc = oit->second;
        if (!slotsOverlap(slot, dur, other.startSlot, oc.duration)) continue;
        if (course.facultyId == oc.facultyId) return false;
        if (course.batchId   == oc.batchId)   return false;
        if (other.roomId     == roomId)        return false;
        if (other.courseId   == sess.courseId) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// pbGenerateCandidates
// ═══════════════════════════════════════════════════════════════════
static std::vector<Candidate>
pbGenerateCandidates(const Session& sess,
                     const Course&  course,
                     const PBStore& store,
                     const std::unordered_map<std::string, Course>& courseMap)
{
    std::vector<Candidate> out;
    out.reserve(32);
    const std::vector<int>& slots =
        (course.courseType == "LAB") ? PB_LAB_SLOTS : PB_THEORY_SLOTS;

    for (const auto& day : orderedDays())
        for (int slot : slots)
            for (const auto& ro : course.roomOptions)
                if (pbCandidateValid(sess, day, slot, ro.roomId, course, store, courseMap))
                    out.push_back({day, slot, ro.roomId});
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// PBThreadCtx — all state for one thread's sequential sub-search.
//
// Mutable fields are thread-private.
// Shared fields are const pointers/references to data in the caller.
// ═══════════════════════════════════════════════════════════════════
struct PBThreadCtx {
    // ── Thread-local (each thread owns its own copy) ──────────────
    std::vector<Session> sessions;      // full sessions array (copy)
    PBStore              store;         // committed sessions
    std::vector<int>     lbPerSession;  // admissible lb per session (copy)
    int  currentCost{0};
    int  remainingLB{0};

    // ── Thread-local stats ────────────────────────────────────────
    long nodesExplored{0};
    long attemptsCount{0};
    long backtracks{0};
    long prunedBranches{0};
    int  solutionsFound{0};
    bool timedOut{false};

    // ── Shared read-only data (pointers into caller's scope) ──────
    const std::unordered_map<std::string, Course>* courseMap{nullptr};
    const ConflictGraph*                            graph{nullptr};
    const std::unordered_map<std::string, int>*     courseIndex{nullptr};

    // ── Shared mutable best-solution state ───────────────────────
    // bestCost: read with relaxed order for pruning; written under lock.
    // bestSolution + lock: protected together by bestLock.
    std::atomic<int>*    sharedBestCost{nullptr};
    std::vector<Session>* sharedBestSolution{nullptr};
    omp_lock_t*          bestLock{nullptr};

    // ── Timing ───────────────────────────────────────────────────
    std::chrono::steady_clock::time_point t0;
    double limitMs{0.0};
};

// ═══════════════════════════════════════════════════════════════════
// pbSeqSolve — sequential recursive B&B running inside ONE thread.
//
// Identical in structure to bnbSolve() in bnb.cpp.
// Differences:
//   • Reads/writes sharedBestCost (atomic) for pruning and solution updates.
//   • Acquires bestLock only when writing a new best solution.
//   • Accumulates stats into PBThreadCtx (thread-local).
// ═══════════════════════════════════════════════════════════════════
static void pbSeqSolve(PBThreadCtx& ctx, int unassignedCount)
{
    ++ctx.nodesExplored;

    // ── Time-limit guard ─────────────────────────────────────────
    if (ctx.limitMs > 0.0) {
        double el = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ctx.t0).count();
        if (el >= ctx.limitMs) { ctx.timedOut = true; return; }
    }

    // ── Base case: all sessions assigned — record if best ─────────
    if (unassignedCount == 0) {
        int gb = ctx.sharedBestCost->load(std::memory_order_relaxed);
        if (ctx.currentCost < gb) {
            // Double-checked locking: re-read inside the lock
            omp_set_lock(ctx.bestLock);
            int confirmed = ctx.sharedBestCost->load(std::memory_order_relaxed);
            if (ctx.currentCost < confirmed) {
                ctx.sharedBestCost->store(ctx.currentCost,
                                          std::memory_order_relaxed);
                *ctx.sharedBestSolution = ctx.sessions;   // deep copy
                ++ctx.solutionsFound;
            }
            omp_unset_lock(ctx.bestLock);
        }
        return;
    }

    // ── MRV: choose unassigned session with fewest candidates ─────
    int bestIdx   = -1;
    int bestCount = INT_MAX;
    int bestDeg   = -1;
    int bestDur   = -1;
    std::vector<Candidate> bestCands;

    for (int i = 0; i < (int)ctx.sessions.size(); ++i) {
        if (!ctx.sessions[i].day.empty()) continue;   // already assigned

        auto cit = ctx.courseMap->find(ctx.sessions[i].courseId);
        if (cit == ctx.courseMap->end()) continue;
        const Course& course = cit->second;

        auto cands = pbGenerateCandidates(ctx.sessions[i], course,
                                          ctx.store, *ctx.courseMap);
        int cnt = (int)cands.size();
        if (cnt == 0) return;   // dead-end

        int deg = 0;
        auto it = ctx.courseIndex->find(ctx.sessions[i].courseId);
        if (it != ctx.courseIndex->end())
            deg = (int)ctx.graph->adj[it->second].size();
        int dur = course.duration;

        bool better = false;
        if (cnt < bestCount) {
            better = true;
        } else if (cnt == bestCount) {
            if (deg > bestDeg) better = true;
            else if (deg == bestDeg) {
                if (dur > bestDur) better = true;
                else if (dur == bestDur &&
                         ctx.sessions[i].sessionId <
                         (bestIdx >= 0 ? ctx.sessions[bestIdx].sessionId : ""))
                    better = true;
            }
        }
        if (better) {
            bestCount = cnt; bestIdx = i; bestDeg = deg;
            bestDur = dur; bestCands = std::move(cands);
        }
    }

    if (bestIdx == -1) return;

    // ── Sort candidates ascending by softPenalty ──────────────────
    auto flatCommitted = ctx.store.flat();
    const Course& selCourse = ctx.courseMap->at(ctx.sessions[bestIdx].courseId);

    struct PenCand { int pen; Candidate cand; };
    std::vector<PenCand> penCands;
    penCands.reserve(bestCands.size());
    for (const auto& c : bestCands) {
        int p = softPenalty(selCourse, c.day, c.slot, c.roomId, flatCommitted);
        penCands.push_back({p, c});
    }
    std::sort(penCands.begin(), penCands.end(),
              [](const PenCand& a, const PenCand& b) {
                  if (a.pen != b.pen) return a.pen < b.pen;
                  const auto& days = orderedDays();
                  int da = (int)(std::find(days.begin(),days.end(),a.cand.day)-days.begin());
                  int db = (int)(std::find(days.begin(),days.end(),b.cand.day)-days.begin());
                  if (da != db) return da < db;
                  if (a.cand.slot != b.cand.slot) return a.cand.slot < b.cand.slot;
                  return a.cand.roomId < b.cand.roomId;
              });

    int sessLB         = ctx.lbPerSession[bestIdx];
    int newRemainingLB = ctx.remainingLB - sessLB;

    Session& sess = ctx.sessions[bestIdx];

    for (int ci = 0; ci < (int)penCands.size(); ++ci) {
        int pen = penCands[ci].pen;

        // ── Pruning: read shared best (relaxed — for efficiency) ──
        // If even the cheapest remaining path can't beat the current
        // global best, prune this and all subsequent candidates
        // (sorted ascending, so pen' >= pen for all remaining).
        int gb = ctx.sharedBestCost->load(std::memory_order_relaxed);
        if (ctx.currentCost + pen + newRemainingLB >= gb) {
            ctx.prunedBranches += (long)(penCands.size() - ci);
            break;
        }

        ++ctx.attemptsCount;

        // ── Assign ────────────────────────────────────────────────
        const Candidate& cand = penCands[ci].cand;
        sess.day       = cand.day;
        sess.startSlot = cand.slot;
        sess.roomId    = cand.roomId;
        ctx.store.push(sess);
        ctx.currentCost += pen;
        ctx.remainingLB -= sessLB;

        pbSeqSolve(ctx, unassignedCount - 1);

        // ── Undo ──────────────────────────────────────────────────
        ctx.store.pop();
        sess.day       = "";
        sess.startSlot = -1;
        sess.roomId    = "";
        ctx.currentCost -= pen;
        ctx.remainingLB += sessLB;

        if (ctx.timedOut) return;
        ++ctx.backtracks;
    }
}

// ═══════════════════════════════════════════════════════════════════
// runParallelBranchAndBound — public entry point
//
// numThreads == 0 → use OMP_NUM_THREADS / hardware concurrency.
// ═══════════════════════════════════════════════════════════════════
PBnBResult runParallelBranchAndBound(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions,
    int                                            numThreads,
    int                                            initialUpperBound,
    double                                         timeLimitMs)
{
    auto t0 = std::chrono::steady_clock::now();
    PBnBResult result;

    // Resolve thread count
    if (numThreads <= 0) numThreads = omp_get_max_threads();

    // Get actual threads that will be used (query inside a parallel region)
    int actualThreads = numThreads;
    #pragma omp parallel num_threads(numThreads)
    {
        #pragma omp single
        actualThreads = omp_get_num_threads();
    }
    result.threadsUsed = actualThreads;

    // courseId -> index
    std::unordered_map<std::string, int> courseIndex;
    courseIndex.reserve(courses.size());
    for (int i = 0; i < (int)courses.size(); ++i)
        courseIndex[courses[i].courseId] = i;

    // Generate all sessions (all unassigned)
    std::vector<Session> baseSessions = generateSessions(courses);
    result.totalSessions = (int)baseSessions.size();

    // Admissible per-session lower bound: preferredStartSlot
    std::vector<int> lbPerSession(baseSessions.size());
    int totalRemainingLB = 0;
    for (int i = 0; i < (int)baseSessions.size(); ++i) {
        auto cit = courseMap.find(baseSessions[i].courseId);
        lbPerSession[i] = (cit != courseMap.end())
                          ? cit->second.preferredStartSlot : 1;
        totalRemainingLB += lbPerSession[i];
    }

    // ── MRV: pick the first session to split on (empty store) ─────
    PBStore emptyStore;
    int splitIdx   = -1;
    int splitCount = INT_MAX;
    int splitDeg   = -1;
    int splitDur   = -1;
    std::vector<Candidate> splitCands;

    for (int i = 0; i < (int)baseSessions.size(); ++i) {
        auto cit = courseMap.find(baseSessions[i].courseId);
        if (cit == courseMap.end()) continue;
        const Course& course = cit->second;

        auto cands = pbGenerateCandidates(baseSessions[i], course,
                                          emptyStore, courseMap);
        int cnt = (int)cands.size();
        if (cnt == 0) {
            // Infeasible at root
            result.feasible = false;
            sessions = baseSessions;
            return result;
        }

        int deg = 0;
        auto it = courseIndex.find(baseSessions[i].courseId);
        if (it != courseIndex.end())
            deg = (int)graph.adj[it->second].size();
        int dur = course.duration;

        bool better = false;
        if (cnt < splitCount) {
            better = true;
        } else if (cnt == splitCount) {
            if (deg > splitDeg) better = true;
            else if (deg == splitDeg) {
                if (dur > splitDur) better = true;
                else if (dur == splitDur &&
                         baseSessions[i].sessionId <
                         (splitIdx >= 0 ? baseSessions[splitIdx].sessionId : ""))
                    better = true;
            }
        }
        if (better) {
            splitCount = cnt; splitIdx = i; splitDeg = deg;
            splitDur = dur; splitCands = std::move(cands);
        }
    }

    if (splitIdx == -1 || splitCands.empty()) {
        sessions = baseSessions;
        return result;
    }

    // Sort split candidates ascending by softPenalty
    std::vector<Session> noCommitted;
    const Course& splitCourse = courseMap.at(baseSessions[splitIdx].courseId);

    struct PenCand { int pen; Candidate cand; };
    std::vector<PenCand> splitPenCands;
    splitPenCands.reserve(splitCands.size());
    for (const auto& c : splitCands) {
        int p = softPenalty(splitCourse, c.day, c.slot, c.roomId, noCommitted);
        splitPenCands.push_back({p, c});
    }
    std::sort(splitPenCands.begin(), splitPenCands.end(),
              [](const PenCand& a, const PenCand& b){ return a.pen < b.pen; });

    // ── Shared state ──────────────────────────────────────────────
    std::atomic<int>     sharedBestCost{initialUpperBound};
    std::vector<Session> sharedBestSolution;
    omp_lock_t           bestLock;
    omp_init_lock(&bestLock);

    int sessLBforSplit   = lbPerSession[splitIdx];
    int remainingLBafter = totalRemainingLB - sessLBforSplit;
    int N                = (int)splitPenCands.size();

    // ── Parallel loop — one branch per iteration ──────────────────
    // Dynamic scheduling: threads pick up branches as they finish,
    // giving load-balanced parallelism even when branches vary widely.
    #pragma omp parallel for schedule(dynamic, 1) num_threads(actualThreads)
    for (int bi = 0; bi < N; ++bi) {

        int pen = splitPenCands[bi].pen;

        // Pre-check: if this top-level candidate can't beat the current
        // global best even with the most optimistic remaining cost, skip it.
        int gb = sharedBestCost.load(std::memory_order_relaxed);
        if (pen + remainingLBafter >= gb) {
            #pragma omp critical(pbstats)
            { result.prunedBranches += 1; }
            continue;
        }

        // Build thread-local context
        PBThreadCtx ctx;
        ctx.sessions     = baseSessions;       // independent copy
        ctx.lbPerSession = lbPerSession;
        ctx.currentCost  = pen;
        ctx.remainingLB  = remainingLBafter;
        ctx.courseMap    = &courseMap;
        ctx.graph        = &graph;
        ctx.courseIndex  = &courseIndex;
        ctx.sharedBestCost     = &sharedBestCost;
        ctx.sharedBestSolution = &sharedBestSolution;
        ctx.bestLock     = &bestLock;
        ctx.t0           = t0;
        ctx.limitMs      = timeLimitMs;

        // Apply this top-level assignment
        const Candidate& cand = splitPenCands[bi].cand;
        ctx.sessions[splitIdx].day       = cand.day;
        ctx.sessions[splitIdx].startSlot = cand.slot;
        ctx.sessions[splitIdx].roomId    = cand.roomId;
        ctx.store.push(ctx.sessions[splitIdx]);
        ++ctx.attemptsCount;

        // Run sequential B&B on remaining unassigned sessions
        pbSeqSolve(ctx, result.totalSessions - 1);

        // Merge thread-local stats into result (under critical section)
        #pragma omp critical(pbstats)
        {
            result.nodesExplored  += ctx.nodesExplored;
            result.attemptsCount  += ctx.attemptsCount;
            result.backtracks     += ctx.backtracks;
            result.prunedBranches += ctx.prunedBranches;
            result.solutionsFound += ctx.solutionsFound;
            if (ctx.timedOut) result.timedOut = true;
        }
    }

    omp_destroy_lock(&bestLock);

    result.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    // Finalise output sessions
    int finalBest = sharedBestCost.load(std::memory_order_relaxed);
    if (!sharedBestSolution.empty() && finalBest < initialUpperBound) {
        sessions        = sharedBestSolution;
        result.bestCost = finalBest;
        result.feasible = true;
    } else {
        sessions        = baseSessions;
        result.feasible = false;
    }

    result.scheduled   = 0;
    result.unscheduled = 0;
    for (const auto& s : sessions) {
        if (!s.day.empty() && s.startSlot != -1) ++result.scheduled;
        else                                      ++result.unscheduled;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// printPBnBStats
// ═══════════════════════════════════════════════════════════════════
void printPBnBStats(const std::string&         datasetSize,
                    const std::vector<Course>& courses,
                    const PBnBResult&          result,
                    const ValidationReport&    report,
                    int                        seqPenalty)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " PARALLEL B&B RESULTS  (OpenMP)\n";
    std::cout << "========================================\n";
    std::cout << "Dataset              : " << datasetSize          << "\n";
    std::cout << "Courses              : " << courses.size()       << "\n";
    std::cout << "OpenMP threads used  : " << result.threadsUsed   << "\n";
    std::cout << "Total sessions       : " << result.totalSessions << "\n";
    std::cout << "Sessions scheduled   : " << result.scheduled     << "\n";
    std::cout << "Sessions unscheduled : " << result.unscheduled   << "\n";
    std::cout << "Nodes explored       : " << result.nodesExplored << "\n";
    std::cout << "Assignments tried    : " << result.attemptsCount << "\n";
    std::cout << "Backtracks           : " << result.backtracks    << "\n";
    std::cout << "Pruned branches      : " << result.prunedBranches<< "\n";
    std::cout << "Solutions found      : " << result.solutionsFound<< "\n";
    std::cout << "Execution time (ms)  : "
              << std::fixed << std::setprecision(3) << result.elapsedMs << "\n";

    if (result.timedOut)
        std::cout << "Status               : TIMED OUT"
                     " — best-found solution reported\n";

    if (result.bestCost >= 0) {
        std::cout << "Best soft penalty    : " << result.bestCost << "\n";
        if (seqPenalty >= 0) {
            int diff = seqPenalty - result.bestCost;
            std::cout << "Sequential B&B pen.  : " << seqPenalty  << "\n";
            if      (diff > 0) std::cout << "Improvement vs Seq   : +" << diff << "\n";
            else if (diff == 0) std::cout << "Improvement vs Seq   : none (matched)\n";
            else                std::cout << "Improvement vs Seq   : " << diff << "\n";
        }
    } else {
        std::cout << "Best soft penalty    : N/A (no solution found)\n";
    }

    std::cout << "\n--- Independent Validator ---\n";
    std::cout << "Faculty conflicts      : " << report.facultyConflicts       << "\n";
    std::cout << "Batch conflicts        : " << report.batchConflicts         << "\n";
    std::cout << "Room conflicts         : " << report.roomConflicts          << "\n";
    std::cout << "Capacity violations    : " << report.capacityViolations     << "\n";
    std::cout << "Room type violations   : " << report.roomTypeViolations     << "\n";
    std::cout << "Availability violations: " << report.availabilityViolations << "\n";
    std::cout << "Lab violations         : " << report.labViolations          << "\n";
    std::cout << "Missing sessions       : " << report.missingSessions        << "\n";
    std::cout << "\nFEASIBLE: " << (report.feasible ? "YES" : "NO") << "\n";
    std::cout << "========================================\n\n";
}
