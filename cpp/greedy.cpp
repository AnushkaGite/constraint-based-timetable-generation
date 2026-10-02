#include "timetable.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <map>

// ═══════════════════════════════════════════════════════════════════
// softPenalty
//
// Returns an integer penalty (lower is better) for placing `course`
// on (day, startSlot, roomId) given already-committed sessions.
//
// Components (additive):
//  P1 – Outside preferred window           : +10 per slot outside
//  P2 – Late-slot preference               : +slot  (earlier is cheaper)
//  P3 – Same course already this day       : +20    (spread sessions)
//  P4 – Batch already has a session this
//       day at an adjacent slot (gap cost) : +5
// ═══════════════════════════════════════════════════════════════════
int softPenalty(
    const Course&               course,
    const std::string&          day,
    int                         startSlot,
    const std::string&          /*roomId*/,    // reserved for future use
    const std::vector<Session>& committed)
{
    int penalty = 0;

    // P1: outside preferred window
    int endSlot = startSlot + course.duration - 1;
    if (startSlot < course.preferredStartSlot)
        penalty += 10 * (course.preferredStartSlot - startSlot);
    if (endSlot > course.preferredEndSlot)
        penalty += 10 * (endSlot - course.preferredEndSlot);

    // P2: later slots cost more (slots are 1..7)
    penalty += startSlot;

    // P3 & P4: scan already-committed sessions
    for (const auto& s : committed) {
        if (s.day != day) continue;
        if (s.courseId == course.courseId) {
            // P3: same course already scheduled this day
            penalty += 20;
        }
        if (s.startSlot == -1) continue;
    }

    return penalty;
}

// ═══════════════════════════════════════════════════════════════════
// runGreedy
// ═══════════════════════════════════════════════════════════════════
GreedyResult runGreedy(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions)
{
    auto t0 = std::chrono::steady_clock::now();

    GreedyResult result;

    // ── Build index: courseId -> index in courses[] ────────────────
    std::unordered_map<std::string, int> courseIndex;
    for (int i = 0; i < (int)courses.size(); ++i)
        courseIndex[courses[i].courseId] = i;

    // ── Generate all sessions ──────────────────────────────────────
    sessions = generateSessions(courses);
    result.totalSessions = (int)sessions.size();

    // ── Ordering: sort sessions for scheduling ─────────────────────
    // Primary  : higher conflict degree (more constrained first)
    // Secondary: longer duration first (labs before theories)
    // Tertiary : lexicographic session ID
    std::vector<int> order(sessions.size());
    for (int i = 0; i < (int)sessions.size(); ++i) order[i] = i;

    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const std::string& cidA = sessions[a].courseId;
        const std::string& cidB = sessions[b].courseId;

        int degA = 0, degB = 0;
        auto itA = courseIndex.find(cidA);
        auto itB = courseIndex.find(cidB);
        if (itA != courseIndex.end())
            degA = (int)graph.adj[itA->second].size();
        if (itB != courseIndex.end())
            degB = (int)graph.adj[itB->second].size();

        if (degA != degB) return degA > degB;   // higher degree first

        // duration
        const Course& cA = courseMap.at(cidA);
        const Course& cB = courseMap.at(cidB);
        if (cA.duration != cB.duration) return cA.duration > cB.duration;

        // lexicographic
        return sessions[a].sessionId < sessions[b].sessionId;
    });

    // ── Valid LAB starting slots (cannot start at 4 → would cross lunch) ──
    // slots 1,2,3 → end at 2,3,4  (OK)
    // slot  4     → end at 5      (crosses lunch — ILLEGAL)
    // slots 5,6   → end at 6,7   (OK)
    static const std::vector<int> labSlots  = {1,2,3,5,6};
    static const std::vector<int> theorySlots = {1,2,3,4,5,6,7};

    // ── Committed sessions so far (for conflict checking) ──────────
    std::vector<Session> committed;   // grows as we assign

    // ── Main greedy loop ───────────────────────────────────────────
    for (int idx : order) {
        Session& sess = sessions[idx];

        const auto it = courseMap.find(sess.courseId);
        if (it == courseMap.end()) continue;
        const Course& course = it->second;

        const std::vector<int>& slots =
            (course.courseType == "LAB") ? labSlots : theorySlots;

        bool assigned = false;
        int  bestPenalty = INT32_MAX;
        std::string bestDay; int bestSlot = -1; std::string bestRoom;

        // ── Enumerate candidates deterministically ─────────────────
        for (const auto& day : orderedDays()) {
            for (int slot : slots) {
                for (const auto& ro : course.roomOptions) {
                    ++result.attemptsCount;

                    CheckResult cr = checkAssignment(
                        sess, day, slot, ro.roomId, courseMap, committed);
                    if (!cr.valid) continue;

                    int pen = softPenalty(course, day, slot, ro.roomId, committed);

                    // Deterministic tie-breaking:
                    //   lower penalty wins; ties broken by earlier day,
                    //   earlier slot, then smaller room ID
                    bool better = false;
                    if (pen < bestPenalty) {
                        better = true;
                    } else if (pen == bestPenalty) {
                        // compare day index
                        const auto& days = orderedDays();
                        int di = (int)(std::find(days.begin(),days.end(),day) - days.begin());
                        int bi = (int)(std::find(days.begin(),days.end(),bestDay) - days.begin());
                        if (di < bi) better = true;
                        else if (di == bi) {
                            if (slot < bestSlot) better = true;
                            else if (slot == bestSlot && ro.roomId < bestRoom)
                                better = true;
                        }
                    }

                    if (better) {
                        bestPenalty = pen;
                        bestDay  = day;
                        bestSlot = slot;
                        bestRoom = ro.roomId;
                        assigned = true;
                    }
                }
            }
        }

        if (assigned) {
            sess.day       = bestDay;
            sess.startSlot = bestSlot;
            sess.roomId    = bestRoom;
            committed.push_back(sess);   // now visible to future sessions
            ++result.scheduled;
            result.totalPenalty += bestPenalty;
        } else {
            // Greedy stuck — leave session unassigned
            ++result.unscheduled;
        }
    }

    // ── Timing ────────────────────────────────────────────────────
    auto t1 = std::chrono::steady_clock::now();
    result.elapsedMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    result.feasible = (result.unscheduled == 0);

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// printTimetable  — full grid MON-FRI x slots 1-7
// ═══════════════════════════════════════════════════════════════════
void printTimetable(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap)
{
    // Group assigned sessions by day and start slot for display
    // We just print a flat sorted list (DAY | TIME | COURSE | ...)
    // which is easier to read than a dense grid for small datasets.

    // Collect assigned sessions
    std::vector<const Session*> asgn;
    for (const auto& s : sessions) {
        if (!s.day.empty() && s.startSlot != -1)
            asgn.push_back(&s);
    }

    // Sort: day order first, then slot
    const auto& days = orderedDays();
    auto dayIdx = [&](const std::string& d) {
        auto it = std::find(days.begin(), days.end(), d);
        return (int)(it - days.begin());
    };

    std::sort(asgn.begin(), asgn.end(), [&](const Session* a, const Session* b) {
        int da = dayIdx(a->day), db = dayIdx(b->day);
        if (da != db) return da < db;
        if (a->startSlot != b->startSlot) return a->startSlot < b->startSlot;
        return a->sessionId < b->sessionId;
    });

    // Header
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " GENERATED TIMETABLE (SMALL)\n";
    std::cout << "========================================\n";
    std::cout << std::left
              << std::setw(5)  << "DAY"
              << std::setw(8)  << "TIME"
              << std::setw(10) << "SESSION"
              << std::setw(34) << "COURSE"
              << std::setw(8)  << "FACULTY"
              << std::setw(6)  << "BATCH"
              << std::setw(5)  << "ROOM"
              << "\n";
    std::cout << std::string(76, '-') << "\n";

    for (const Session* sp : asgn) {
        const Session& s = *sp;
        auto cit = courseMap.find(s.courseId);
        std::string cname = "?", fac = "?", bat = "?";
        if (cit != courseMap.end()) {
            cname = cit->second.courseName;
            fac   = cit->second.facultyId;
            bat   = cit->second.batchId;
        }

        std::cout << std::left
                  << std::setw(5)  << s.day
                  << std::setw(8)  << slotLabel(s.startSlot)
                  << std::setw(10) << s.sessionId
                  << std::setw(34) << cname
                  << std::setw(8)  << fac
                  << std::setw(6)  << bat
                  << std::setw(5)  << s.roomId
                  << "\n";
    }

    // Unscheduled
    int miss = 0;
    for (const auto& s : sessions)
        if (s.day.empty() || s.startSlot == -1) ++miss;

    if (miss > 0) {
        std::cout << "\n  [!] " << miss << " session(s) could not be scheduled:\n";
        for (const auto& s : sessions) {
            if (s.day.empty() || s.startSlot == -1)
                std::cout << "      " << s.sessionId << " (" << s.courseId << ")\n";
        }
    }
    std::cout << "========================================\n";
}

// ═══════════════════════════════════════════════════════════════════
// printGreedyStats
// ═══════════════════════════════════════════════════════════════════
void printGreedyStats(const std::string& datasetSize,
                      const std::vector<Course>& courses,
                      const GreedyResult& result,
                      const ValidationReport& report)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " GREEDY SCHEDULER RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Dataset              : " << datasetSize          << "\n";
    std::cout << "Courses              : " << courses.size()       << "\n";
    std::cout << "Total sessions       : " << result.totalSessions << "\n";
    std::cout << "Sessions scheduled   : " << result.scheduled     << "\n";
    std::cout << "Sessions unscheduled : " << result.unscheduled   << "\n";
    std::cout << "Candidates evaluated : " << result.attemptsCount << "\n";
    std::cout << "Execution time (ms)  : " << std::fixed
              << std::setprecision(3) << result.elapsedMs        << "\n";

    if (result.unscheduled == 0) {
        std::cout << "Total soft penalty   : " << result.totalPenalty << "\n";
    } else {
        std::cout << "Total soft penalty   : N/A (incomplete timetable)\n";
    }

    // Validator breakdown
    std::cout << "\n--- Independent Validator ---\n";
    std::cout << "Faculty conflicts      : " << report.facultyConflicts      << "\n";
    std::cout << "Batch conflicts        : " << report.batchConflicts        << "\n";
    std::cout << "Room conflicts         : " << report.roomConflicts         << "\n";
    std::cout << "Capacity violations    : " << report.capacityViolations    << "\n";
    std::cout << "Room type violations   : " << report.roomTypeViolations    << "\n";
    std::cout << "Availability violations: " << report.availabilityViolations << "\n";
    std::cout << "Lab violations         : " << report.labViolations         << "\n";
    std::cout << "Missing sessions       : " << report.missingSessions       << "\n";
    std::cout << "\n";
    std::cout << "FEASIBLE: " << (report.feasible ? "YES" : "NO") << "\n";
    std::cout << "========================================\n\n";
}
