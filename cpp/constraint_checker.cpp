#include "timetable.h"
#include <iostream>
#include <sstream>

// ═══════════════════════════════════════════════════════════════════
// Internal helper: find the RoomOption for a given roomId in a course
// Returns nullptr if not found.
// ═══════════════════════════════════════════════════════════════════
static const RoomOption* findRoomOption(const Course& course,
                                        const std::string& roomId)
{
    for (const auto& ro : course.roomOptions) {
        if (ro.roomId == roomId) return &ro;
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════
// checkAssignment
// ═══════════════════════════════════════════════════════════════════
CheckResult checkAssignment(
    const Session&                                 session,
    const std::string&                             day,
    int                                            startSlot,
    const std::string&                             roomId,
    const std::unordered_map<std::string, Course>& courseMap,
    const std::vector<Session>&                    assigned)
{
    auto fail = [](ViolationType vt, const std::string& msg) -> CheckResult {
        return {false, vt, msg};
    };

    // ── Lookup the course ────────────────────────────────────────
    auto it = courseMap.find(session.courseId);
    if (it == courseMap.end()) {
        return fail(ViolationType::NONE, "Course not found: " + session.courseId);
    }
    const Course& course = it->second;
    int dur = course.duration; // 1 for THEORY, 2 for LAB

    // ── 11. Valid day and slot range ──────────────────────────────
    if (!isValidDay(day)) {
        return fail(ViolationType::OUT_OF_RANGE,
                    "Invalid day: " + day);
    }
    if (startSlot < 1 || (startSlot + dur - 1) > SLOTS_PER_DAY) {
        std::ostringstream oss;
        oss << "Slot " << startSlot << " with duration " << dur
            << " goes out of range (1-" << SLOTS_PER_DAY << ") on " << day;
        return fail(ViolationType::OUT_OF_RANGE, oss.str());
    }

    // ── 9. Duration check ────────────────────────────────────────
    // The CSV duration field already encodes 1 for THEORY, 2 for LAB.
    // Here we also verify the course type matches expected duration.
    if (course.courseType == "THEORY" && dur != 1) {
        return fail(ViolationType::INVALID_DURATION,
                    "THEORY course should have duration 1");
    }
    if (course.courseType == "LAB" && dur != 2) {
        return fail(ViolationType::INVALID_DURATION,
                    "LAB course should have duration 2");
    }

    // ── 10. Lab crossing lunch ────────────────────────────────────
    // A 2-slot lab starting at LUNCH_BREAK_AFTER would occupy
    // slots LUNCH_BREAK_AFTER and LUNCH_BREAK_AFTER+1, crossing the break.
    if (course.courseType == "LAB" && startSlot == LUNCH_BREAK_AFTER) {
        std::ostringstream oss;
        oss << "LAB starting at slot " << startSlot
            << " crosses the lunch break (after slot " << LUNCH_BREAK_AFTER << ")";
        return fail(ViolationType::LAB_CROSSES_LUNCH, oss.str());
    }

    // ── 6. Room listed in course options ─────────────────────────
    const RoomOption* ro = findRoomOption(course, roomId);
    if (!ro) {
        return fail(ViolationType::ROOM_NOT_IN_OPTIONS,
                    "Room " + roomId + " not in course options for " + course.courseId);
    }

    // ── 5. Room type ─────────────────────────────────────────────
    if (ro->roomType != course.courseType) {
        return fail(ViolationType::ROOM_TYPE,
                    "Room type " + ro->roomType + " != course type " + course.courseType);
    }

    // ── 4. Room capacity ─────────────────────────────────────────
    if (ro->capacity < course.batchStrength) {
        std::ostringstream oss;
        oss << "Room " << roomId << " capacity " << ro->capacity
            << " < batch strength " << course.batchStrength;
        return fail(ViolationType::ROOM_CAPACITY, oss.str());
    }

    // ── 7. Faculty availability ───────────────────────────────────
    for (int s = startSlot; s < startSlot + dur; ++s) {
        if (course.facultyUnavailable.count({day, s})) {
            std::ostringstream oss;
            oss << "Faculty " << course.facultyId
                << " unavailable on " << day << "-" << s;
            return fail(ViolationType::FACULTY_UNAVAILABLE, oss.str());
        }
    }

    // ── 8. Batch availability ─────────────────────────────────────
    for (int s = startSlot; s < startSlot + dur; ++s) {
        if (course.batchUnavailable.count({day, s})) {
            std::ostringstream oss;
            oss << "Batch " << course.batchId
                << " unavailable on " << day << "-" << s;
            return fail(ViolationType::BATCH_UNAVAILABLE, oss.str());
        }
    }

    // ── Conflict checks against already-assigned sessions ─────────
    for (const auto& other : assigned) {
        // Skip sessions with no assignment or the same session
        if (other.sessionId == session.sessionId) continue;
        if (other.day != day) continue;           // different day → no conflict
        if (other.startSlot == -1) continue;       // unassigned other

        auto oit = courseMap.find(other.courseId);
        if (oit == courseMap.end()) continue;
        const Course& otherCourse = oit->second;
        int otherDur = otherCourse.duration;

        if (!slotsOverlap(startSlot, dur, other.startSlot, otherDur)) continue;

        // ── 1. Faculty conflict ───────────────────────────────────
        if (course.facultyId == otherCourse.facultyId) {
            return fail(ViolationType::FACULTY_CONFLICT,
                        "Faculty " + course.facultyId + " double-booked on " +
                        day + " with " + other.sessionId);
        }

        // ── 2. Batch conflict ─────────────────────────────────────
        if (course.batchId == otherCourse.batchId) {
            return fail(ViolationType::BATCH_CONFLICT,
                        "Batch " + course.batchId + " double-booked on " +
                        day + " with " + other.sessionId);
        }

        // ── 3. Room conflict ──────────────────────────────────────
        if (other.roomId == roomId) {
            return fail(ViolationType::ROOM_CONFLICT,
                        "Room " + roomId + " double-booked on " + day +
                        " with " + other.sessionId);
        }

        // ── 12. Same-course overlap ───────────────────────────────
        if (other.courseId == session.courseId) {
            return fail(ViolationType::SAME_COURSE_OVERLAP,
                        "Two sessions of course " + session.courseId +
                        " overlap on " + day);
        }
    }

    return {true, ViolationType::NONE, "OK"};
}

// ═══════════════════════════════════════════════════════════════════
// ValidationReport::print
// ═══════════════════════════════════════════════════════════════════
void ValidationReport::print() const {
    std::cout << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << " TIMETABLE VALIDATION REPORT\n";
    std::cout << "----------------------------------------\n";
    std::cout << "Faculty conflicts      : " << facultyConflicts     << "\n";
    std::cout << "Batch conflicts        : " << batchConflicts       << "\n";
    std::cout << "Room conflicts         : " << roomConflicts        << "\n";
    std::cout << "Capacity violations    : " << capacityViolations   << "\n";
    std::cout << "Room type violations   : " << roomTypeViolations   << "\n";
    std::cout << "Availability violations: " << availabilityViolations << "\n";
    std::cout << "Lab violations         : " << labViolations        << "\n";
    std::cout << "Missing sessions       : " << missingSessions      << "\n";
    std::cout << "\n";
    std::cout << "FEASIBLE: " << (feasible ? "YES" : "NO") << "\n";
    std::cout << "----------------------------------------\n";
}

// ═══════════════════════════════════════════════════════════════════
// validateTimetable — fully independent scan
// ═══════════════════════════════════════════════════════════════════
ValidationReport validateTimetable(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap)
{
    ValidationReport rep;

    // Separate assigned sessions
    std::vector<const Session*> assigned;
    for (const auto& s : sessions) {
        if (s.day.empty() || s.startSlot == -1 || s.roomId.empty()) {
            ++rep.missingSessions;
        } else {
            assigned.push_back(&s);
        }
    }

    int n = static_cast<int>(assigned.size());

    // ── Pairwise conflict checks ──────────────────────────────────
    for (int i = 0; i < n; ++i) {
        const Session& a = *assigned[i];
        auto ait = courseMap.find(a.courseId);
        if (ait == courseMap.end()) continue;
        const Course& ac = ait->second;

        for (int j = i + 1; j < n; ++j) {
            const Session& b = *assigned[j];
            if (a.day != b.day) continue;

            auto bit = courseMap.find(b.courseId);
            if (bit == courseMap.end()) continue;
            const Course& bc = bit->second;

            if (!slotsOverlap(a.startSlot, ac.duration, b.startSlot, bc.duration))
                continue;

            // Faculty conflict
            if (ac.facultyId == bc.facultyId)
                ++rep.facultyConflicts;

            // Batch conflict
            if (ac.batchId == bc.batchId)
                ++rep.batchConflicts;

            // Room conflict
            if (a.roomId == b.roomId)
                ++rep.roomConflicts;
        }
    }

    // ── Per-session checks ────────────────────────────────────────
    for (const Session* sp : assigned) {
        const Session& s = *sp;
        auto it = courseMap.find(s.courseId);
        if (it == courseMap.end()) continue;
        const Course& c = it->second;
        int dur = c.duration;

        // Capacity / room type / room option
        const RoomOption* ro = nullptr;
        for (const auto& r : c.roomOptions) {
            if (r.roomId == s.roomId) { ro = &r; break; }
        }
        if (ro) {
            if (ro->capacity < c.batchStrength)
                ++rep.capacityViolations;
            if (ro->roomType != c.courseType)
                ++rep.roomTypeViolations;
        } else {
            // Room not in options — count as a room type violation
            ++rep.roomTypeViolations;
        }

        // Availability
        for (int sl = s.startSlot; sl < s.startSlot + dur; ++sl) {
            if (c.facultyUnavailable.count({s.day, sl}) ||
                c.batchUnavailable.count({s.day, sl})) {
                ++rep.availabilityViolations;
                break; // count once per session
            }
        }

        // Lab duration / lunch crossing
        if (c.courseType == "LAB") {
            if (dur != 2) ++rep.labViolations;
            else if (s.startSlot == LUNCH_BREAK_AFTER) ++rep.labViolations;
        }
        if (c.courseType == "THEORY" && dur != 1) ++rep.labViolations;

        // Out-of-range (we still count it somewhere useful)
        if (!isValidDay(s.day) ||
            s.startSlot < 1 ||
            (s.startSlot + dur - 1) > SLOTS_PER_DAY) {
            ++rep.labViolations; // generic catch-all for range errors
        }
    }

    rep.feasible = (rep.facultyConflicts   == 0 &&
                    rep.batchConflicts     == 0 &&
                    rep.roomConflicts      == 0 &&
                    rep.capacityViolations == 0 &&
                    rep.roomTypeViolations == 0 &&
                    rep.availabilityViolations == 0 &&
                    rep.labViolations      == 0 &&
                    rep.missingSessions    == 0);

    return rep;
}
