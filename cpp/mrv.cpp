#include "timetable.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <climits>
#include <array>
#include <random>

// ═══════════════════════════════════════════════════════════════════
// Valid slot lists
// ═══════════════════════════════════════════════════════════════════
static const std::vector<int> MRV_LAB_SLOTS    = {1, 2, 3, 5, 6};
static const std::vector<int> MRV_THEORY_SLOTS = {1, 2, 3, 4, 5, 6, 7};

// Map day string -> index 0-4
static int dayToIdx(const std::string& d) {
    if (d == "MON") return 0;
    if (d == "TUE") return 1;
    if (d == "WED") return 2;
    if (d == "THU") return 3;
    return 4; // FRI
}

// ═══════════════════════════════════════════════════════════════════
// CommittedIndex
//
// Maintains per-day buckets of committed sessions so conflict checks
// only scan the sessions that share a day.  Assignment = push_back;
// undo = pop_back (order preserved because backtracking is LIFO).
// ═══════════════════════════════════════════════════════════════════
struct CommittedIndex {
    // 5 days × committed sessions on that day
    std::array<std::vector<Session>, 5> byDay;

    void add(const Session& s) {
        byDay[dayToIdx(s.day)].push_back(s);
    }
    void remove() {
        // LIFO: we remove from whichever day was last added —
        // but we don't know which day without extra bookkeeping.
        // Simpler: store a separate "last day" stack.
    }
};

// To support LIFO undo properly we store (dayIdx, entry) on a
// parallel stack instead of the 2-level structure above.
struct CommittedStore {
    std::array<std::vector<Session>, 5> byDay;
    std::vector<int> dayStack;   // day of each successive push

    void push(const Session& s) {
        int d = dayToIdx(s.day);
        byDay[d].push_back(s);
        dayStack.push_back(d);
    }
    void pop() {
        if (dayStack.empty()) return;
        int d = dayStack.back();
        dayStack.pop_back();
        byDay[d].pop_back();
    }
    // Flat view (needed for softPenalty which takes vector<Session>)
    std::vector<Session> flat() const {
        std::vector<Session> out;
        for (const auto& bkt : byDay)
            for (const auto& s : bkt)
                out.push_back(s);
        return out;
    }
};

// ═══════════════════════════════════════════════════════════════════
// Fast single-candidate validity check (replaces checkAssignment for
// the inner loop — uses the day-indexed store directly).
// ═══════════════════════════════════════════════════════════════════
static bool isCandidateValid(
    const Session&       sess,
    const std::string&   day,
    int                  slot,
    const std::string&   roomId,
    const Course&        course,
    const CommittedStore& store,
    const std::unordered_map<std::string, Course>& courseMap)
{
    int dur = course.duration;

    // ── Range check ──────────────────────────────────────────────
    if (!isValidDay(day)) return false;
    if (slot < 1 || (slot + dur - 1) > SLOTS_PER_DAY) return false;

    // ── Duration / lunch check ───────────────────────────────────
    if (course.courseType == "LAB"    && dur != 2)               return false;
    if (course.courseType == "THEORY" && dur != 1)               return false;
    if (course.courseType == "LAB"    && slot == LUNCH_BREAK_AFTER) return false;

    // ── Room listed in options ───────────────────────────────────
    const RoomOption* ro = nullptr;
    for (const auto& r : course.roomOptions)
        if (r.roomId == roomId) { ro = &r; break; }
    if (!ro) return false;

    // ── Room type ────────────────────────────────────────────────
    if (ro->roomType != course.courseType) return false;

    // ── Room capacity ────────────────────────────────────────────
    if (ro->capacity < course.batchStrength) return false;

    // ── Faculty availability ─────────────────────────────────────
    for (int s = slot; s < slot + dur; ++s)
        if (course.facultyUnavailable.count({day, s})) return false;

    // ── Batch availability ───────────────────────────────────────
    for (int s = slot; s < slot + dur; ++s)
        if (course.batchUnavailable.count({day, s})) return false;

    // ── Conflict checks (only same-day sessions) ─────────────────
    int di = dayToIdx(day);
    for (const auto& other : store.byDay[di]) {
        auto oit = courseMap.find(other.courseId);
        if (oit == courseMap.end()) continue;
        const Course& oc = oit->second;

        if (!slotsOverlap(slot, dur, other.startSlot, oc.duration)) continue;

        if (course.facultyId == oc.facultyId) return false;    // faculty conflict
        if (course.batchId   == oc.batchId)   return false;    // batch conflict
        if (other.roomId     == roomId)        return false;    // room conflict
        if (other.courseId   == sess.courseId) return false;    // same-course overlap
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// generateCandidates — uses CommittedStore
// ═══════════════════════════════════════════════════════════════════
static std::vector<Candidate>
generateCandidates(const Session&    sess,
                   const Course&     course,
                   const CommittedStore& store,
                   const std::unordered_map<std::string, Course>& courseMap)
{
    std::vector<Candidate> out;
    out.reserve(32);

    const std::vector<int>& slots =
        (course.courseType == "LAB") ? MRV_LAB_SLOTS : MRV_THEORY_SLOTS;

    for (const auto& day : orderedDays()) {
        for (int slot : slots) {
            for (const auto& ro : course.roomOptions) {
                if (isCandidateValid(sess, day, slot, ro.roomId,
                                     course, store, courseMap))
                    out.push_back({day, slot, ro.roomId});
            }
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// MRV Context
// ═══════════════════════════════════════════════════════════════════
struct MRVContext {
    std::vector<Session>&                          sessions;
    const std::unordered_map<std::string, Course>& courseMap;
    const ConflictGraph&                           graph;
    const std::unordered_map<std::string, int>&    courseIndex;
    CommittedStore                                 store;
    MRVResult&                                     result;
    std::chrono::steady_clock::time_point          t0;
    double                                         limitMs;
    std::chrono::steady_clock::time_point          restartT0;
    double                                         restartLimitMs{0.0};
    bool                                           restartExpired{false};
    bool                                           diversify{false};
    std::mt19937                                  rng{0x5EED};
};

// ═══════════════════════════════════════════════════════════════════
// Recursive backtracking with MRV
// ═══════════════════════════════════════════════════════════════════
static bool btSolve(MRVContext& ctx, int unassignedCount)
{
    ++ctx.result.nodesExplored;

    if (ctx.limitMs > 0.0) {
        const auto now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double, std::milli>(
            now - ctx.t0).count();
        if (el >= ctx.limitMs) { ctx.result.timedOut = true; return false; }
        if (ctx.restartLimitMs > 0.0 &&
            std::chrono::duration<double, std::milli>(
                now - ctx.restartT0).count() >= ctx.restartLimitMs) {
            ctx.restartExpired = true;
            return false;
        }
    }

    if (unassignedCount == 0) return true;

    // ── MRV: find session with fewest valid candidates ────────────
    int  bestIdx   = -1;
    int  bestCount = INT_MAX;
    int  bestDeg   = -1;
    int  bestDur   = -1;
    int  bestTieCount = 0;
    std::vector<Candidate> bestCands;

    for (int i = 0; i < (int)ctx.sessions.size(); ++i) {
        if (!ctx.sessions[i].day.empty()) continue;

        auto cit = ctx.courseMap.find(ctx.sessions[i].courseId);
        if (cit == ctx.courseMap.end()) continue;
        const Course& course = cit->second;

        auto cands = generateCandidates(ctx.sessions[i], course,
                                        ctx.store, ctx.courseMap);
        int cnt = (int)cands.size();

        if (cnt == 0) return false;   // dead-end prune

        int deg = 0;
        auto it = ctx.courseIndex.find(ctx.sessions[i].courseId);
        if (it != ctx.courseIndex.end())
            deg = (int)ctx.graph.adj[it->second].size();
        int dur = course.duration;

        bool better = false;
        bool equalPriority = false;
        if (cnt < bestCount) {
            better = true;
        } else if (cnt == bestCount) {
            if (deg > bestDeg) better = true;
            else if (deg == bestDeg) {
                if (dur > bestDur) better = true;
                else if (dur == bestDur) {
                    equalPriority = true;
                    if (!ctx.diversify &&
                        ctx.sessions[i].sessionId <
                        (bestIdx >= 0 ? ctx.sessions[bestIdx].sessionId : ""))
                        better = true;
                }
            }
        }

        if (better) {
            bestCount = cnt;
            bestIdx   = i;
            bestDeg   = deg;
            bestDur   = dur;
            bestTieCount = 1;
            bestCands = std::move(cands);
        } else if (equalPriority && ctx.diversify) {
            ++bestTieCount;
            std::uniform_int_distribution<int> chooseTie(1, bestTieCount);
            if (chooseTie(ctx.rng) == 1) {
                bestIdx   = i;
                bestCands = std::move(cands);
            }
        }
    }

    if (bestIdx == -1) return true;

    // ── Order candidates by soft penalty ─────────────────────────
    {
        const Course& course = ctx.courseMap.at(ctx.sessions[bestIdx].courseId);
        auto flatCommitted = ctx.store.flat();   // for softPenalty
        if (ctx.diversify) {
            struct ScoredCandidate {
                int penalty;
                Candidate candidate;
            };
            std::vector<ScoredCandidate> scored;
            scored.reserve(bestCands.size());
            for (const auto& candidate : bestCands) {
                scored.push_back({
                    softPenalty(course, candidate.day, candidate.slot,
                                candidate.roomId, flatCommitted),
                    candidate
                });
            }
            std::shuffle(scored.begin(), scored.end(), ctx.rng);
            std::stable_sort(scored.begin(), scored.end(),
                [](const ScoredCandidate& a, const ScoredCandidate& b) {
                    return a.penalty < b.penalty;
                });
            for (size_t i = 0; i < scored.size(); ++i)
                bestCands[i] = std::move(scored[i].candidate);
        } else {
            std::sort(bestCands.begin(), bestCands.end(),
                [&](const Candidate& a, const Candidate& b) {
                    int pa = softPenalty(course, a.day, a.slot, a.roomId, flatCommitted);
                    int pb = softPenalty(course, b.day, b.slot, b.roomId, flatCommitted);
                    if (pa != pb) return pa < pb;
                    const auto& days = orderedDays();
                    int da = (int)(std::find(days.begin(),days.end(),a.day)-days.begin());
                    int db = (int)(std::find(days.begin(),days.end(),b.day)-days.begin());
                    if (da != db) return da < db;
                    if (a.slot != b.slot) return a.slot < b.slot;
                    return a.roomId < b.roomId;
                });
        }
    }

    // ── Try each candidate ───────────────────────────────────────
    Session& sess = ctx.sessions[bestIdx];
    for (const auto& cand : bestCands) {
        ++ctx.result.attemptsCount;

        sess.day       = cand.day;
        sess.startSlot = cand.slot;
        sess.roomId    = cand.roomId;
        ctx.store.push(sess);

        if (btSolve(ctx, unassignedCount - 1)) return true;

        ctx.store.pop();
        sess.day       = "";
        sess.startSlot = -1;
        sess.roomId    = "";
        if (ctx.result.timedOut || ctx.restartExpired) return false;
        ++ctx.result.backtracks;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════
// runMRV
// ═══════════════════════════════════════════════════════════════════
MRVResult runMRV(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions,
    double                                         timeLimitMs)
{
    auto t0 = std::chrono::steady_clock::now();
    MRVResult result;

    std::unordered_map<std::string, int> courseIndex;
    courseIndex.reserve(courses.size());
    for (int i = 0; i < (int)courses.size(); ++i)
        courseIndex[courses[i].courseId] = i;

    sessions = generateSessions(courses);
    result.totalSessions = (int)sessions.size();

    const bool diversify = sessions.size() >= 100 && timeLimitMs > 0.0;
    MRVContext ctx{sessions, courseMap, graph, courseIndex,
                   {}, result, t0, timeLimitMs, t0, 0.0, false, diversify};

    bool solved = false;
    int restart = 0;
    double restartBudgetMs = std::min(1500.0, timeLimitMs);
    // Larger searches rotate their tie-breaking order without exceeding the total budget.
    do {
        if (restart > 0) {
            sessions = generateSessions(courses);
            ctx.store = CommittedStore{};
            ctx.restartExpired = false;
            ctx.rng.seed(0x5EEDu + static_cast<unsigned int>(restart) * 0x9E3779B9u);
        }
        ctx.restartT0 = std::chrono::steady_clock::now();
        ctx.restartLimitMs = diversify ? restartBudgetMs : 0.0;
        solved = btSolve(ctx, result.totalSessions);
        if (solved || result.timedOut || !ctx.restartExpired || !diversify)
            break;

        ++restart;
        double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        double remainingMs = timeLimitMs - elapsedMs;
        if (remainingMs <= 0.0) {
            result.timedOut = true;
            break;
        }
        restartBudgetMs = std::min(restartBudgetMs * 2.0, remainingMs);
    } while (true);

    result.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    result.scheduled   = 0;
    result.unscheduled = 0;
    for (const auto& s : sessions) {
        if (!s.day.empty() && s.startSlot != -1) ++result.scheduled;
        else                                      ++result.unscheduled;
    }

    result.feasible = solved && (result.unscheduled == 0);

    if (result.feasible) {
        auto flat = ctx.store.flat();
        for (const auto& s : flat) {
            const Course& c = courseMap.at(s.courseId);
            result.totalPenalty += softPenalty(c, s.day, s.startSlot, s.roomId, flat);
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// printMRVStats
// ═══════════════════════════════════════════════════════════════════
void printMRVStats(const std::string&         datasetSize,
                   const std::vector<Course>& courses,
                   const MRVResult&           result,
                   const ValidationReport&    report)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " MRV + BACKTRACKING RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Dataset              : " << datasetSize             << "\n";
    std::cout << "Courses              : " << courses.size()          << "\n";
    std::cout << "Total sessions       : " << result.totalSessions    << "\n";
    std::cout << "Sessions scheduled   : " << result.scheduled        << "\n";
    std::cout << "Sessions unscheduled : " << result.unscheduled      << "\n";
    std::cout << "Nodes explored       : " << result.nodesExplored    << "\n";
    std::cout << "Assignments tried    : " << result.attemptsCount    << "\n";
    std::cout << "Backtracks           : " << result.backtracks       << "\n";
    std::cout << "Execution time (ms)  : " << std::fixed
              << std::setprecision(3) << result.elapsedMs             << "\n";
    if (result.timedOut)
        std::cout << "Status               : TIMED OUT (partial result)\n";
    if (result.feasible)
        std::cout << "Total soft penalty   : " << result.totalPenalty  << "\n";
    else
        std::cout << "Total soft penalty   : N/A (incomplete)\n";
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
