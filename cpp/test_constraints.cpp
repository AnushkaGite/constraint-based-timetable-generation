#include "timetable.h"
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
// Test utilities
// ═══════════════════════════════════════════════════════════════════

static int g_pass = 0;
static int g_fail = 0;

// Expect that checkAssignment is VALID
static void expectValid(const std::string& label,
                        const Session& sess,
                        const std::string& day, int slot,
                        const std::string& room,
                        const std::unordered_map<std::string, Course>& cm,
                        const std::vector<Session>& assigned)
{
    CheckResult r = checkAssignment(sess, day, slot, room, cm, assigned);
    if (r.valid) {
        std::cout << "  [PASS] " << label << "\n";
        ++g_pass;
    } else {
        std::cout << "  [FAIL] " << label
                  << " => expected VALID but got "
                  << violationName(r.violation)
                  << ": " << r.message << "\n";
        ++g_fail;
    }
}

// Expect that checkAssignment is INVALID with the given ViolationType
static void expectInvalid(const std::string& label,
                           const Session& sess,
                           const std::string& day, int slot,
                           const std::string& room,
                           const std::unordered_map<std::string, Course>& cm,
                           const std::vector<Session>& assigned,
                           ViolationType expectedViolation)
{
    CheckResult r = checkAssignment(sess, day, slot, room, cm, assigned);
    if (!r.valid && r.violation == expectedViolation) {
        std::cout << "  [PASS] " << label
                  << " (correctly rejected: " << violationName(r.violation) << ")\n";
        ++g_pass;
    } else if (!r.valid) {
        std::cout << "  [FAIL] " << label
                  << " => expected " << violationName(expectedViolation)
                  << " but got " << violationName(r.violation)
                  << ": " << r.message << "\n";
        ++g_fail;
    } else {
        std::cout << "  [FAIL] " << label
                  << " => expected INVALID (" << violationName(expectedViolation)
                  << ") but result was VALID\n";
        ++g_fail;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Build a minimal in-memory dataset for testing
// ═══════════════════════════════════════════════════════════════════
static std::unordered_map<std::string, Course> buildTestCourseMap()
{
    std::unordered_map<std::string, Course> cm;

    // ── THEORY course: C1 ─────────────────────────────────────────
    {
        Course c;
        c.courseId        = "C1";
        c.courseName      = "Algorithms";
        c.courseType      = "THEORY";
        c.facultyId       = "F1";
        c.batchId         = "B1";
        c.batchStrength   = 40;
        c.sessionsPerWeek = 3;
        c.duration        = 1;
        c.roomOptions     = {{"R1", 50, "THEORY"}, {"R2", 30, "THEORY"}};
        c.facultyUnavailable = {{"MON", 3}};   // F1 unavailable MON-3
        c.batchUnavailable   = {{"WED", 5}};   // B1 unavailable WED-5
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    // ── THEORY course: C2  (same faculty as C1 = F1) ─────────────
    {
        Course c;
        c.courseId        = "C2";
        c.courseName      = "OS";
        c.courseType      = "THEORY";
        c.facultyId       = "F1";   // SAME faculty as C1
        c.batchId         = "B2";
        c.batchStrength   = 35;
        c.sessionsPerWeek = 3;
        c.duration        = 1;
        c.roomOptions     = {{"R1", 50, "THEORY"}};
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    // ── THEORY course: C3  (same batch as C1 = B1) ───────────────
    {
        Course c;
        c.courseId        = "C3";
        c.courseName      = "Networks";
        c.courseType      = "THEORY";
        c.facultyId       = "F2";
        c.batchId         = "B1";   // SAME batch as C1
        c.batchStrength   = 40;
        c.sessionsPerWeek = 3;
        c.duration        = 1;
        c.roomOptions     = {{"R1", 50, "THEORY"}};
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    // ── LAB course: C4 ────────────────────────────────────────────
    {
        Course c;
        c.courseId        = "C4";
        c.courseName      = "DB Lab";
        c.courseType      = "LAB";
        c.facultyId       = "F3";
        c.batchId         = "B3";
        c.batchStrength   = 25;
        c.sessionsPerWeek = 2;
        c.duration        = 2;
        c.roomOptions     = {{"L1", 30, "LAB"}};
        c.facultyUnavailable = {{"FRI", 7}};
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    // ── THEORY course: C5  (small room for capacity test) ─────────
    {
        Course c;
        c.courseId        = "C5";
        c.courseName      = "Maths";
        c.courseType      = "THEORY";
        c.facultyId       = "F4";
        c.batchId         = "B4";
        c.batchStrength   = 60;           // batch of 60
        c.sessionsPerWeek = 2;
        c.duration        = 1;
        c.roomOptions     = {{"R2", 30, "THEORY"}}; // room capacity only 30
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    // ── THEORY course: C6  (uses a LAB room for type test) ────────
    {
        Course c;
        c.courseId        = "C6";
        c.courseName      = "Physics";
        c.courseType      = "THEORY";
        c.facultyId       = "F5";
        c.batchId         = "B5";
        c.batchStrength   = 20;
        c.sessionsPerWeek = 2;
        c.duration        = 1;
        c.roomOptions     = {{"L1", 30, "LAB"}}; // wrong type in options
        c.preferredStartSlot = 1;
        c.preferredEndSlot   = 7;
        cm[c.courseId] = c;
    }

    return cm;
}

// ═══════════════════════════════════════════════════════════════════
// Run all constraint tests
// ═══════════════════════════════════════════════════════════════════
static void runConstraintTests()
{
    auto cm = buildTestCourseMap();

    // Helper lambdas to build sessions quickly
    auto makeSession = [](const std::string& id, const std::string& cid) {
        Session s; s.sessionId = id; s.courseId = cid;
        s.day = ""; s.startSlot = -1; s.roomId = "";
        return s;
    };
    auto assignSession = [](Session s, const std::string& day, int slot,
                             const std::string& room) {
        s.day = day; s.startSlot = slot; s.roomId = room;
        return s;
    };

    Session s1 = makeSession("C1_S1", "C1");
    Session s2 = makeSession("C2_S1", "C2");
    Session s3 = makeSession("C3_S1", "C3");
    Session s4 = makeSession("C4_S1", "C4");
    Session s5 = makeSession("C5_S1", "C5");
    Session s6 = makeSession("C6_S1", "C6");

    std::cout << "\n========================================\n";
    std::cout << " PHASE 2A — CONSTRAINT CHECKER TESTS\n";
    std::cout << "========================================\n\n";

    // ── Test 1: Valid assignment ──────────────────────────────────
    std::cout << "── Test 1: Valid assignment ─────────────────────────\n";
    {
        std::vector<Session> assigned;
        expectValid("T01 valid THEORY C1 on TUE-1 R1",
                    s1, "TUE", 1, "R1", cm, assigned);
    }

    // ── Test 2: Faculty conflict ──────────────────────────────────
    std::cout << "── Test 2: Faculty conflict ─────────────────────────\n";
    {
        // C2 also has F1 — assign C1_S1 to MON-1 first, then C2_S1 on same slot
        Session already = assignSession(s1, "MON", 1, "R1");
        std::vector<Session> assigned = {already};
        expectInvalid("T02 faculty conflict C2 vs C1 same slot MON-1",
                      s2, "MON", 1, "R1", cm, assigned,
                      ViolationType::FACULTY_CONFLICT);
    }

    // ── Test 3: Batch conflict ────────────────────────────────────
    std::cout << "── Test 3: Batch conflict ───────────────────────────\n";
    {
        // C3 has B1 — same as C1
        Session already = assignSession(s1, "TUE", 2, "R1");
        std::vector<Session> assigned = {already};
        expectInvalid("T03 batch conflict C3 vs C1 same slot TUE-2",
                      s3, "TUE", 2, "R1", cm, assigned,
                      ViolationType::BATCH_CONFLICT);
    }

    // ── Test 4: Room conflict ─────────────────────────────────────
    std::cout << "── Test 4: Room conflict ────────────────────────────\n";
    {
        // C1 and C2 use different faculties/batches but we force room R1
        // We need a different course pair that differs on faculty AND batch
        // C1 (F1, B1, R1) and C3 (F2, B1) — but B1 same triggers batch first.
        // Use C2 (F1, B2) and give it a different room slot scenario:
        // Actually let's manufacture: a second C1 session already in R1 at WED-1,
        // and try to place C2 in R1 at WED-1. 
        // C1=F1,B1  C2=F1,B2 — faculty conflict fires first.
        // We need a pair with different faculty, different batch, same room.
        // C4=F3,B3  C1=F1,B1 — no shared constraints.
        // But C4 uses L1 (LAB) not R1 (THEORY). So create a scenario where
        // an already-assigned non-conflicting session occupies R1, then C1 wants R1.
        //
        // To get a pure room conflict we need two sessions with:
        //   different faculty, different batch, same room.
        // C1 (F1,B1) vs C4 (F3,B3) — assign C1 to TUE-1 in R1 first.
        // Then try C1 again in TUE-1 R1: triggers SAME_COURSE_OVERLAP.
        // So we need a 3rd course. Let's temporarily add one inside this test.
        Course cx;
        cx.courseId = "CX"; cx.courseName = "Extra"; cx.courseType = "THEORY";
        cx.facultyId = "FX"; cx.batchId = "BX"; cx.batchStrength = 20;
        cx.sessionsPerWeek = 1; cx.duration = 1;
        cx.roomOptions = {{"R1", 50, "THEORY"}};
        cx.preferredStartSlot = 1; cx.preferredEndSlot = 7;
        auto cmX = cm;
        cmX["CX"] = cx;

        Session sx = makeSession("CX_S1", "CX");
        Session already = assignSession(s1, "WED", 3, "R1");
        std::vector<Session> assigned = {already};
        expectInvalid("T04 room conflict CX vs C1 same room WED-3",
                      sx, "WED", 3, "R1", cmX, assigned,
                      ViolationType::ROOM_CONFLICT);
    }

    // ── Test 5: Insufficient room capacity ────────────────────────
    std::cout << "── Test 5: Room capacity violation ──────────────────\n";
    {
        std::vector<Session> assigned;
        // C5 batch=60, R2 capacity=30
        expectInvalid("T05 capacity violation C5 in R2 (30 < 60)",
                      s5, "MON", 2, "R2", cm, assigned,
                      ViolationType::ROOM_CAPACITY);
    }

    // ── Test 6: Wrong room type ───────────────────────────────────
    std::cout << "── Test 6: Room type violation ──────────────────────\n";
    {
        std::vector<Session> assigned;
        // C6 is THEORY, L1 is LAB — room type mismatch
        expectInvalid("T06 room type violation C6 (THEORY) in L1 (LAB)",
                      s6, "MON", 2, "L1", cm, assigned,
                      ViolationType::ROOM_TYPE);
    }

    // ── Test 7: Faculty unavailable slot ─────────────────────────
    std::cout << "── Test 7: Faculty unavailable ──────────────────────\n";
    {
        std::vector<Session> assigned;
        // C1 faculty F1 unavailable MON-3
        expectInvalid("T07 faculty unavailable C1 on MON-3",
                      s1, "MON", 3, "R1", cm, assigned,
                      ViolationType::FACULTY_UNAVAILABLE);
    }

    // ── Test 8: Batch unavailable slot ───────────────────────────
    std::cout << "── Test 8: Batch unavailable ────────────────────────\n";
    {
        std::vector<Session> assigned;
        // C1 batch B1 unavailable WED-5
        expectInvalid("T08 batch unavailable C1 on WED-5",
                      s1, "WED", 5, "R1", cm, assigned,
                      ViolationType::BATCH_UNAVAILABLE);
    }

    // ── Test 9: Invalid lab placement (out of day range) ─────────
    std::cout << "── Test 9: LAB out of slot range ────────────────────\n";
    {
        std::vector<Session> assigned;
        // C4 duration=2, starting at slot 7 -> slot 8 doesn't exist (max=7)
        expectInvalid("T09 LAB C4 starting slot 7 goes out of range",
                      s4, "MON", 7, "L1", cm, assigned,
                      ViolationType::OUT_OF_RANGE);
    }

    // ── Test 10: Lab crossing lunch ──────────────────────────────
    std::cout << "── Test 10: LAB crosses lunch break ─────────────────\n";
    {
        std::vector<Session> assigned;
        // C4 duration=2, starting at LUNCH_BREAK_AFTER (slot 4) crosses lunch
        expectInvalid("T10 LAB C4 starts at slot 4 crosses lunch",
                      s4, "TUE", LUNCH_BREAK_AFTER, "L1", cm, assigned,
                      ViolationType::LAB_CROSSES_LUNCH);
    }

    // ── Summary ───────────────────────────────────────────────────
    std::cout << "\n========================================\n";
    std::cout << " CONSTRAINT TESTS SUMMARY\n";
    std::cout << "========================================\n";
    std::cout << "Passed: " << g_pass << "\n";
    std::cout << "Failed: " << g_fail << "\n";
    if (g_fail == 0) {
        std::cout << "\nALL CONSTRAINT TESTS PASSED\n";
    } else {
        std::cout << "\nSOME TESTS FAILED — see output above\n";
    }
    std::cout << "========================================\n\n";
}

// ═══════════════════════════════════════════════════════════════════
// Validator integration test: build a small timetable with known
// violations and verify the report counts them correctly.
// ═══════════════════════════════════════════════════════════════════
static void runValidatorTests()
{
    std::cout << "========================================\n";
    std::cout << " PHASE 2A — VALIDATOR INTEGRATION TEST \n";
    std::cout << "========================================\n\n";

    auto cm = buildTestCourseMap();

    // ── Sub-test A: perfect timetable (should be FEASIBLE) ───────
    {
        std::cout << "Sub-test A: All valid assignments -> expect FEASIBLE\n";
        std::vector<Session> sessions;

        // C1_S1: TUE-1 R1  (C1: F1,B1,THEORY,dur=1, R1:50:THEORY capacity OK)
        sessions.push_back({"C1_S1","C1","TUE",1,"R1"});
        // C2_S1: WED-1 R1  (C2: F1,B2,THEORY,dur=1 -- F1 but different day)
        sessions.push_back({"C2_S1","C2","WED",1,"R1"});
        // C3_S1: FRI-2 R1  (C3: F2,B1,THEORY,dur=1 -- B1 but different day to C1)
        sessions.push_back({"C3_S1","C3","FRI",2,"R1"});
        // C4_S1: MON-1 L1  (C4: F3,B3,LAB,dur=2, slot 1->2 OK)
        sessions.push_back({"C4_S1","C4","MON",1,"L1"});

        auto rep = validateTimetable(sessions, cm);
        rep.print();

        bool ok = rep.feasible;
        if (ok) { std::cout << "  [PASS] Sub-test A: timetable reported FEASIBLE\n\n"; ++g_pass; }
        else     { std::cout << "  [FAIL] Sub-test A: expected FEASIBLE but got violations\n\n"; ++g_fail; }
    }

    // ── Sub-test B: timetable with known violations ───────────────
    {
        std::cout << "Sub-test B: Injected violations -> expect NOT FEASIBLE\n";
        std::vector<Session> sessions;

        // Faculty conflict: C1 and C2 both F1, same day+slot
        sessions.push_back({"C1_S1","C1","MON",1,"R1"});
        sessions.push_back({"C2_S1","C2","MON",1,"R1"}); // R1 conflict + faculty conflict

        // Missing session (unassigned)
        sessions.push_back({"C4_S1","C4","",  -1,""});

        // Capacity violation: C5 batch=60 in R2 cap=30
        sessions.push_back({"C5_S1","C5","TUE",2,"R2"});

        auto rep = validateTimetable(sessions, cm);
        rep.print();

        bool ok = !rep.feasible
                  && rep.facultyConflicts >= 1
                  && rep.roomConflicts    >= 1
                  && rep.missingSessions  >= 1
                  && rep.capacityViolations >= 1;
        if (ok) { std::cout << "  [PASS] Sub-test B: violations correctly detected\n\n"; ++g_pass; }
        else     { std::cout << "  [FAIL] Sub-test B: violation counts unexpected\n\n"; ++g_fail; }
    }
}

// ═══════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════
int main() {
    runConstraintTests();
    runValidatorTests();

    std::cout << "========================================\n";
    std::cout << " OVERALL RESULT\n";
    std::cout << "========================================\n";
    std::cout << "Total passed: " << g_pass << "\n";
    std::cout << "Total failed: " << g_fail << "\n";
    if (g_fail == 0) {
        std::cout << "\nPHASE 2A FOUNDATION TEST PASSED\n";
    } else {
        std::cout << "\nSOME TESTS FAILED\n";
    }
    std::cout << "========================================\n";
    return (g_fail == 0) ? 0 : 1;
}
