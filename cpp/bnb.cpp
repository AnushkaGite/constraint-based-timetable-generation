#include "timetable.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <climits>
#include <array>
#include <map>
#include <numeric>

// ═══════════════════════════════════════════════════════════════════
// Valid slot tables (same as mrv.cpp — local to this TU)
// ═══════════════════════════════════════════════════════════════════
static const std::vector<int> BNB_LAB_SLOTS    = {1, 2, 3, 5, 6};
static const std::vector<int> BNB_THEORY_SLOTS = {1, 2, 3, 4, 5, 6, 7};

static int bnbDayIdx(const std::string& d) {
    if (d == "MON") return 0;
    if (d == "TUE") return 1;
    if (d == "WED") return 2;
    if (d == "THU") return 3;
    return 4; // FRI
}

// ═══════════════════════════════════════════════════════════════════
// BnBStore — per-day indexed committed sessions.
// Identical in purpose to CommittedStore in mrv.cpp; named differently
// to avoid any ODR ambiguity in this translation unit.
// ═══════════════════════════════════════════════════════════════════
struct BnBStore {
    std::array<std::vector<Session>, 5> byDay;
    std::vector<int>                    dayStack;   // LIFO day of each push

    void push(const Session& s) {
        int d = bnbDayIdx(s.day);
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
// Fast single-candidate hard-constraint check using BnBStore.
// Mirrors isCandidateValid() in mrv.cpp.
// ═══════════════════════════════════════════════════════════════════
static bool bnbCandidateValid(
    const Session&    sess,
    const std::string& day,
    int               slot,
    const std::string& roomId,
    const Course&     course,
    const BnBStore&   store,
    const std::unordered_map<std::string, Course>& courseMap)
{
    int dur = course.duration;

    if (!isValidDay(day)) return false;
    if (slot < 1 || (slot + dur - 1) > SLOTS_PER_DAY) return false;

    if (course.courseType == "LAB"    && dur != 2)                return false;
    if (course.courseType == "THEORY" && dur != 1)                return false;
    if (course.courseType == "LAB"    && slot == LUNCH_BREAK_AFTER) return false;

    // Room in options?
    const RoomOption* ro = nullptr;
    for (const auto& r : course.roomOptions)
        if (r.roomId == roomId) { ro = &r; break; }
    if (!ro) return false;
    if (ro->roomType != course.courseType)    return false;
    if (ro->capacity < course.batchStrength)  return false;

    // Faculty / batch availability
    for (int s = slot; s < slot + dur; ++s) {
        if (course.facultyUnavailable.count({day, s})) return false;
        if (course.batchUnavailable.count({day, s}))   return false;
    }

    // Conflict checks — only same-day sessions
    int di = bnbDayIdx(day);
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
// bnbGenerateCandidates — enumerate all valid (day,slot,room) for a session
// ═══════════════════════════════════════════════════════════════════
static std::vector<Candidate>
bnbGenerateCandidates(const Session& sess,
                      const Course&  course,
                      const BnBStore& store,
                      const std::unordered_map<std::string, Course>& courseMap)
{
    std::vector<Candidate> out;
    out.reserve(32);
    const std::vector<int>& slots =
        (course.courseType == "LAB") ? BNB_LAB_SLOTS : BNB_THEORY_SLOTS;

    for (const auto& day : orderedDays())
        for (int slot : slots)
            for (const auto& ro : course.roomOptions)
                if (bnbCandidateValid(sess, day, slot, ro.roomId, course, store, courseMap))
                    out.push_back({day, slot, ro.roomId});
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// globalPenalty
//
// Computes the FULL soft cost of a complete timetable.
//
// Components:
//  P1 – preferred window violation     : +10 per slot outside window
//  P2 – late slot cost                 : +startSlot per session
//  P3 – same course, same day (pairs)  : +20 per extra session on a day
//  P4 – batch intra-day gap            : +5 per empty slot between
//       any two sessions the same batch attends on the same day
//       (gap = slot_B - (slot_A + durA) when slot_B > slot_A + durA)
//
// Note: P3 is counted per-pair (not doubled) unlike the incremental
// softPenalty() which adds +20 from the second session's perspective.
// ═══════════════════════════════════════════════════════════════════
int globalPenalty(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap)
{
    int total = 0;

    // ── P1 + P2 ──────────────────────────────────────────────────
    for (const auto& s : sessions) {
        if (s.day.empty() || s.startSlot == -1) continue;
        auto cit = courseMap.find(s.courseId);
        if (cit == courseMap.end()) continue;
        const Course& c = cit->second;

        int endSlot = s.startSlot + c.duration - 1;
        // P1: preferred window
        if (s.startSlot < c.preferredStartSlot)
            total += 10 * (c.preferredStartSlot - s.startSlot);
        if (endSlot > c.preferredEndSlot)
            total += 10 * (endSlot - c.preferredEndSlot);
        // P2: late slot
        total += s.startSlot;
    }

    // ── P3: same-course same-day pairs ───────────────────────────
    // For each (courseId, day) bucket, penalty = 20 * (count - 1)
    std::map<std::pair<std::string,std::string>, int> cdCount;
    for (const auto& s : sessions) {
        if (s.day.empty()) continue;
        cdCount[{s.courseId, s.day}]++;
    }
    for (auto it = cdCount.begin(); it != cdCount.end(); ++it)
        if (it->second > 1) total += 20 * (it->second - 1);

    // ── P4: batch intra-day gap ──────────────────────────────────
    // Group sessions by (batchId, day); sort by start slot; gap between consecutive.
    struct SlotDur { int slot; int dur; };
    std::map<std::pair<std::string,std::string>, std::vector<SlotDur>> batchDay;
    for (const auto& s : sessions) {
        if (s.day.empty() || s.startSlot == -1) continue;
        auto cit = courseMap.find(s.courseId);
        if (cit == courseMap.end()) continue;
        const Course& c = cit->second;
        batchDay[{c.batchId, s.day}].push_back({s.startSlot, c.duration});
    }
    for (auto it = batchDay.begin(); it != batchDay.end(); ++it) {
        std::vector<SlotDur>& slots = it->second;
        std::sort(slots.begin(), slots.end(), [](const SlotDur& a, const SlotDur& b){
            return a.slot < b.slot;
        });
        for (int i = 1; i < (int)slots.size(); ++i) {
            int prevEnd = slots[i-1].slot + slots[i-1].dur;
            int gap     = slots[i].slot - prevEnd;
            if (gap > 0) total += 5 * gap;
        }
    }

    return total;
}

// ═══════════════════════════════════════════════════════════════════
// lowerBound — per-session optimistic minimum contribution to softPenalty
//
// For each UNASSIGNED session, the minimum possible addition to the
// incremental soft-cost (as used by Greedy and B&B currentCost) is:
//
//   min over valid (day, slot, room) of softPenalty(course, day, slot, room, [])
//
// Ignoring P3 (optimistic: no same-course-same-day clash) and P2
// (slot >= preferredStartSlot to keep P1=0):
//
//   minimum = preferredStartSlot   (place right at the window start)
//
// This never overestimates the actual softPenalty contribution, so
// the bound is ADMISSIBLE.
//
// We track this as a running sum `remainingLB` updated O(1) per assign/undo.
// ═══════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════
// B&B Context — everything shared across recursive calls
// ═══════════════════════════════════════════════════════════════════
struct BnBContext {
    std::vector<Session>&                          sessions;     // mutable: assigned in-place
    const std::unordered_map<std::string, Course>& courseMap;
    const ConflictGraph&                           graph;
    const std::unordered_map<std::string, int>&    courseIndex;  // courseId -> index
    BnBStore                                       store;        // committed, day-indexed
    int                                            currentCost{0};  // incremental softPenalty sum
    int                                            remainingLB{0};  // sum of lbPerSession[i] for unassigned i
    std::vector<int>                               lbPerSession;    // preferredStartSlot per session
    int                                            bestCost;        // best complete timetable cost so far
    std::vector<Session>                           bestSolution;    // sessions at bestCost
    BnBResult&                                     result;
    std::chrono::steady_clock::time_point          t0;
    double                                         limitMs;
};

// ═══════════════════════════════════════════════════════════════════
// bnbSolve — recursive B&B (void: continues searching after solutions)
// ═══════════════════════════════════════════════════════════════════
static void bnbSolve(BnBContext& ctx, int unassignedCount)
{
    ++ctx.result.nodesExplored;

    // ── Time-limit guard ─────────────────────────────────────────
    if (ctx.limitMs > 0.0) {
        double el = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ctx.t0).count();
        if (el >= ctx.limitMs) { ctx.result.timedOut = true; return; }
    }

    // ── Base case: complete timetable ────────────────────────────
    if (unassignedCount == 0) {
        if (ctx.currentCost < ctx.bestCost) {
            ctx.bestCost     = ctx.currentCost;
            ctx.bestSolution = ctx.sessions;   // deep copy of current assignment
            ++ctx.result.solutionsFound;
        }
        return;   // continue search — do NOT return true/short-circuit
    }

    // ── MRV: choose unassigned session with fewest valid candidates ─
    int  bestIdx   = -1;
    int  bestCount = INT_MAX;
    int  bestDeg   = -1;
    int  bestDur   = -1;
    std::vector<Candidate> bestCands;

    for (int i = 0; i < (int)ctx.sessions.size(); ++i) {
        if (!ctx.sessions[i].day.empty()) continue;   // already assigned

        auto cit = ctx.courseMap.find(ctx.sessions[i].courseId);
        if (cit == ctx.courseMap.end()) continue;
        const Course& course = cit->second;

        auto cands = bnbGenerateCandidates(ctx.sessions[i], course,
                                           ctx.store, ctx.courseMap);
        int cnt = (int)cands.size();
        if (cnt == 0) return;   // dead-end: this branch is infeasible

        int deg = 0;
        auto it = ctx.courseIndex.find(ctx.sessions[i].courseId);
        if (it != ctx.courseIndex.end())
            deg = (int)ctx.graph.adj[it->second].size();
        int dur = course.duration;

        // MRV tie-breaking: fewest cands > higher degree > longer duration > session ID
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
            bestCount = cnt;
            bestIdx   = i;
            bestDeg   = deg;
            bestDur   = dur;
            bestCands = std::move(cands);
        }
    }

    if (bestIdx == -1) return;   // all assigned (shouldn't happen here)

    // ── Pre-compute penalties for all candidates, then sort ascending ─
    //    This lets us break early once the pruning bound is hit.
    auto flatCommitted = ctx.store.flat();
    const Course& selCourse = ctx.courseMap.at(ctx.sessions[bestIdx].courseId);

    struct PenCand { int pen; Candidate cand; };
    std::vector<PenCand> penCands;
    penCands.reserve(bestCands.size());
    for (const auto& c : bestCands) {
        int p = softPenalty(selCourse, c.day, c.slot, c.roomId, flatCommitted);
        penCands.push_back({p, c});
    }
    std::sort(penCands.begin(), penCands.end(), [](const PenCand& a, const PenCand& b) {
        if (a.pen != b.pen) return a.pen < b.pen;
        // deterministic tie-break: day order, slot, room
        const auto& days = orderedDays();
        int da = (int)(std::find(days.begin(),days.end(),a.cand.day)-days.begin());
        int db = (int)(std::find(days.begin(),days.end(),b.cand.day)-days.begin());
        if (da != db) return da < db;
        if (a.cand.slot != b.cand.slot) return a.cand.slot < b.cand.slot;
        return a.cand.roomId < b.cand.roomId;
    });

    // ── Lower bound remaining AFTER assigning this session ─────────
    // remainingLB currently includes lbPerSession[bestIdx].
    // After assignment: newRemainingLB = remainingLB - lbPerSession[bestIdx].
    int sessLB         = ctx.lbPerSession[bestIdx];
    int newRemainingLB = ctx.remainingLB - sessLB;

    // ── Try each candidate in ascending penalty order ──────────────
    Session& sess = ctx.sessions[bestIdx];
    for (int ci = 0; ci < (int)penCands.size(); ++ci) {
        int pen = penCands[ci].pen;

        // ── B&B PRUNING ─────────────────────────────────────────
        // After assigning this candidate: newCurrentCost = currentCost + pen
        // Best possible total remaining cost = newRemainingLB
        // Prune if no strict improvement possible.
        // Since penCands is sorted ascending, all remaining candidates
        // have pen' >= pen, so if this one is prunable, all subsequent are too.
        if (ctx.currentCost + pen + newRemainingLB >= ctx.bestCost) {
            long pruned = (long)(penCands.size() - ci);
            ctx.result.prunedBranches += pruned;
            break;   // all remaining candidates pruned
        }

        ++ctx.result.attemptsCount;

        // ── Assign ────────────────────────────────────────────────
        const Candidate& cand = penCands[ci].cand;
        sess.day       = cand.day;
        sess.startSlot = cand.slot;
        sess.roomId    = cand.roomId;
        ctx.store.push(sess);
        ctx.currentCost  += pen;
        ctx.remainingLB  -= sessLB;

        bnbSolve(ctx, unassignedCount - 1);

        // ── Undo ──────────────────────────────────────────────────
        ctx.store.pop();
        sess.day       = "";
        sess.startSlot = -1;
        sess.roomId    = "";
        ctx.currentCost  -= pen;
        ctx.remainingLB  += sessLB;

        if (ctx.result.timedOut) return;
        ++ctx.result.backtracks;
    }
}

// ═══════════════════════════════════════════════════════════════════
// runBranchAndBound — public entry point
// ═══════════════════════════════════════════════════════════════════
BnBResult runBranchAndBound(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions,
    int                                            initialUpperBound,
    double                                         timeLimitMs)
{
    auto t0 = std::chrono::steady_clock::now();
    BnBResult result;

    // courseId -> index in courses[]
    std::unordered_map<std::string, int> courseIndex;
    courseIndex.reserve(courses.size());
    for (int i = 0; i < (int)courses.size(); ++i)
        courseIndex[courses[i].courseId] = i;

    // Generate all sessions (all unassigned)
    sessions = generateSessions(courses);
    result.totalSessions = (int)sessions.size();

    // Per-session lower-bound contribution: preferredStartSlot (admissible lb)
    std::vector<int> lbPerSession(sessions.size());
    for (int i = 0; i < (int)sessions.size(); ++i) {
        auto cit = courseMap.find(sessions[i].courseId);
        lbPerSession[i] = (cit != courseMap.end())
                          ? cit->second.preferredStartSlot
                          : 1;
    }
    int totalRemainingLB = 0;
    for (int lb : lbPerSession) totalRemainingLB += lb;

    // Build context
    BnBContext ctx{
        sessions, courseMap, graph, courseIndex,
        /*store=*/ {},
        /*currentCost=*/ 0,
        /*remainingLB=*/ totalRemainingLB,
        lbPerSession,
        /*bestCost=*/ initialUpperBound,
        /*bestSolution=*/ {},
        result, t0, timeLimitMs
    };

    bnbSolve(ctx, result.totalSessions);

    // Timing
    result.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    // Finalise output sessions
    if (!ctx.bestSolution.empty()) {
        sessions = ctx.bestSolution;
        result.bestCost = ctx.bestCost;
        result.feasible = true;
    } else {
        // No solution improved on initialUpperBound.
        // sessions remains as the empty/unassigned state from generateSessions.
        result.feasible = false;
    }

    // Count scheduled / unscheduled in the returned sessions
    result.scheduled   = 0;
    result.unscheduled = 0;
    for (const auto& s : sessions) {
        if (!s.day.empty() && s.startSlot != -1) ++result.scheduled;
        else                                      ++result.unscheduled;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// printBnBStats
// ═══════════════════════════════════════════════════════════════════
void printBnBStats(const std::string&         datasetSize,
                   const std::vector<Course>& courses,
                   const BnBResult&           result,
                   const ValidationReport&    report,
                   int                        greedyPenalty)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " BRANCH & BOUND RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Dataset              : " << datasetSize             << "\n";
    std::cout << "Courses              : " << courses.size()          << "\n";
    std::cout << "Total sessions       : " << result.totalSessions    << "\n";
    std::cout << "Sessions scheduled   : " << result.scheduled        << "\n";
    std::cout << "Sessions unscheduled : " << result.unscheduled      << "\n";
    std::cout << "Nodes explored       : " << result.nodesExplored    << "\n";
    std::cout << "Assignments tried    : " << result.attemptsCount    << "\n";
    std::cout << "Backtracks           : " << result.backtracks       << "\n";
    std::cout << "Pruned branches      : " << result.prunedBranches   << "\n";
    std::cout << "Solutions found      : " << result.solutionsFound   << "\n";
    std::cout << "Execution time (ms)  : " << std::fixed
              << std::setprecision(3) << result.elapsedMs             << "\n";

    if (result.timedOut)
        std::cout << "Status               : TIMED OUT"
                     " — best found so far is the reported solution\n";

    if (result.bestCost >= 0) {
        std::cout << "Best soft penalty    : " << result.bestCost << "\n";
        if (greedyPenalty >= 0) {
            int diff = greedyPenalty - result.bestCost;
            std::cout << "Greedy soft penalty  : " << greedyPenalty << "\n";
            if (diff > 0)
                std::cout << "Improvement vs Greedy: +" << diff << "\n";
            else if (diff == 0)
                std::cout << "Improvement vs Greedy: none (B&B matched Greedy)\n";
            else
                std::cout << "Improvement vs Greedy: " << diff
                          << " (B&B worse — Greedy bound was tighter)\n";
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
    std::cout << "\n";
    std::cout << "FEASIBLE: " << (report.feasible ? "YES" : "NO") << "\n";
    std::cout << "========================================\n\n";
}
