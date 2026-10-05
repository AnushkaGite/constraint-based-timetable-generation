#pragma once
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>

// ─────────────────────────────────────────────
// RoomOption: one room entry parsed from room_options
// ─────────────────────────────────────────────
struct RoomOption {
    std::string roomId;
    int         capacity;
    std::string roomType;   // THEORY or LAB
};

// ─────────────────────────────────────────────
// Course: one row from the CSV (after filtering by dataset_size)
// ─────────────────────────────────────────────
struct Course {
    std::string              courseId;
    std::string              courseName;
    std::string              courseType;       // THEORY or LAB
    std::string              facultyId;
    std::string              batchId;
    int                      batchStrength;
    int                      sessionsPerWeek;
    int                      duration;         // in slots
    std::vector<RoomOption>  roomOptions;
    std::set<std::pair<std::string,int>> facultyUnavailable; // {DAY, slot}
    std::set<std::pair<std::string,int>> batchUnavailable;
    int                      preferredStartSlot;
    int                      preferredEndSlot;
};

// ─────────────────────────────────────────────
// Session: one scheduled (or to-be-scheduled) class
// ─────────────────────────────────────────────
struct Session {
    std::string sessionId;
    std::string courseId;
    std::string day;       // unassigned initially
    int         startSlot; // unassigned initially (-1)
    std::string roomId;    // unassigned initially
};

// ─────────────────────────────────────────────
// ConflictGraph: adjacency list indexed by course index
// ─────────────────────────────────────────────
struct ConflictGraph {
    int numVertices{0};
    int numEdges{0};
    std::vector<std::unordered_set<int>> adj; // adj[i] = set of neighbours

    void build(const std::vector<Course>& courses);
    int  minDegree() const;
    int  maxDegree() const;
    double avgDegree() const;
};

// ─────────────────────────────────────────────
// Functions declared in timetable.cpp
// ─────────────────────────────────────────────

// Parse "DAY-slot|DAY-slot|..." into a set<{day,slot}>
// Returns empty set for "NONE"
std::set<std::pair<std::string,int>> parseAvailability(const std::string& s);

// Parse "R01:60:THEORY|L01:70:LAB|..." into a vector<RoomOption>
std::vector<RoomOption> parseRoomOptions(const std::string& s);

// Load courses from CSV, filtering by datasetSize ("SMALL", "MEDIUM", etc.)
// Returns false and prints error if file cannot be opened.
bool loadCSV(const std::string& filepath,
             const std::string& datasetSize,
             std::vector<Course>& courses);

// Generate sessions for every course
std::vector<Session> generateSessions(const std::vector<Course>& courses);

// Print summary to stdout
void printSummary(const std::string&          datasetSize,
                  const std::vector<Course>&  courses,
                  const std::vector<Session>& sessions,
                  const ConflictGraph&        graph);

// ─────────────────────────────────────────────────────────────────
// Phase 2A: Hard Constraint Checker
// ─────────────────────────────────────────────────────────────────

// Slots per day (1..SLOTS_PER_DAY inclusive)
constexpr int SLOTS_PER_DAY = 7;
// Lunch break: any LAB that spans slot LUNCH_BREAK to LUNCH_BREAK+1 is invalid.
// We define the break BETWEEN slot 4 and slot 5 (i.e. a 2-slot lab starting at
// slot 4 would occupy slots 4 and 5 — crossing the lunch boundary).
constexpr int LUNCH_BREAK_AFTER = 4; // lab cannot start here if duration=2

// Valid days
inline bool isValidDay(const std::string& d) {
    return d == "MON" || d == "TUE" || d == "WED" || d == "THU" || d == "FRI";
}

// Returns true if two time blocks overlap.
//   blockA: [startA, startA + durA - 1]
//   blockB: [startB, startB + durB - 1]
inline bool slotsOverlap(int startA, int durA, int startB, int durB) {
    int endA = startA + durA - 1;
    int endB = startB + durB - 1;
    return (startA <= endB) && (startB <= endA);
}

// ── Violation record ─────────────────────────────────────────────
enum class ViolationType {
    NONE = 0,
    FACULTY_CONFLICT,
    BATCH_CONFLICT,
    ROOM_CONFLICT,
    ROOM_CAPACITY,
    ROOM_TYPE,
    ROOM_NOT_IN_OPTIONS,
    FACULTY_UNAVAILABLE,
    BATCH_UNAVAILABLE,
    INVALID_DURATION,
    LAB_CROSSES_LUNCH,
    OUT_OF_RANGE,
    SAME_COURSE_OVERLAP
};

inline const char* violationName(ViolationType v) {
    switch (v) {
        case ViolationType::FACULTY_CONFLICT:    return "FACULTY_CONFLICT";
        case ViolationType::BATCH_CONFLICT:      return "BATCH_CONFLICT";
        case ViolationType::ROOM_CONFLICT:       return "ROOM_CONFLICT";
        case ViolationType::ROOM_CAPACITY:       return "ROOM_CAPACITY";
        case ViolationType::ROOM_TYPE:           return "ROOM_TYPE";
        case ViolationType::ROOM_NOT_IN_OPTIONS: return "ROOM_NOT_IN_OPTIONS";
        case ViolationType::FACULTY_UNAVAILABLE: return "FACULTY_UNAVAILABLE";
        case ViolationType::BATCH_UNAVAILABLE:   return "BATCH_UNAVAILABLE";
        case ViolationType::INVALID_DURATION:    return "INVALID_DURATION";
        case ViolationType::LAB_CROSSES_LUNCH:   return "LAB_CROSSES_LUNCH";
        case ViolationType::OUT_OF_RANGE:        return "OUT_OF_RANGE";
        case ViolationType::SAME_COURSE_OVERLAP: return "SAME_COURSE_OVERLAP";
        default:                                 return "NONE";
    }
}

struct CheckResult {
    bool          valid{true};
    ViolationType violation{ViolationType::NONE};
    std::string   message;
};

// ── Per-assignment check ─────────────────────────────────────────
// Check whether assigning (day, startSlot, roomId) to `session` is
// valid given the course it belongs to and the already-assigned sessions.
//
// courseMap : courseId -> Course   (fast lookup)
// assigned  : all sessions that already have a valid assignment
//             (day != "" && startSlot != -1 && roomId != "")
//
// The room capacity / type / option lookup uses the course's roomOptions list.
// The actual chosen room's properties are looked up from that list.
CheckResult checkAssignment(
    const Session&                           session,
    const std::string&                       day,
    int                                      startSlot,
    const std::string&                       roomId,
    const std::unordered_map<std::string, Course>& courseMap,
    const std::vector<Session>&              assigned
);

// ─────────────────────────────────────────────────────────────────
// Phase 2A: Independent Timetable Validator
// ─────────────────────────────────────────────────────────────────

struct ValidationReport {
    int facultyConflicts{0};
    int batchConflicts{0};
    int roomConflicts{0};
    int capacityViolations{0};
    int roomTypeViolations{0};
    int availabilityViolations{0}; // faculty + batch unavailability
    int labViolations{0};          // duration wrong or lunch crossing
    int missingSessions{0};        // sessions without an assignment
    bool feasible{false};

    void print() const;
};

// Validate a complete timetable independently.
// sessions   : all sessions (assigned + unassigned)
// courseMap  : courseId -> Course
ValidationReport validateTimetable(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap
);

// ─────────────────────────────────────────────────────────────────
// Phase 2B: Sequential Greedy Scheduler
// ─────────────────────────────────────────────────────────────────

// Ordered days and valid lab start slots (globals used by greedy)
inline const std::vector<std::string>& orderedDays() {
    static std::vector<std::string> d = {"MON","TUE","WED","THU","FRI"};
    return d;
}

// Slot-to-time label for display (slots 1-7 → 08-09, 09-10, ... 14-15)
inline std::string slotLabel(int slot) {
    int h = 8 + slot - 1;   // slot 1 = 08:xx
    int h2 = h + 1;
    auto pad = [](int x){ return (x < 10 ? "0" : "") + std::to_string(x); };
    return pad(h) + "-" + pad(h2);
}

// Immediate soft-constraint penalty for one candidate assignment.
// Inputs: course whose session we are placing, the proposed (day,slot,room),
//         and all sessions already committed.
int softPenalty(
    const Course&               course,
    const std::string&          day,
    int                         startSlot,
    const std::string&          roomId,
    const std::vector<Session>& committed
);

// Statistics returned by the greedy scheduler
struct GreedyResult {
    int  totalSessions{0};
    int  scheduled{0};
    int  unscheduled{0};
    long attemptsCount{0};   // total (day,slot,room) candidates evaluated
    bool feasible{false};
    int  totalPenalty{0};
    double elapsedMs{0.0};
};

// Run the greedy scheduler.
//   courses    : loaded and filtered courses
//   graph      : conflict graph (provides degree ordering)
//   courseMap  : courseId -> Course  (for fast lookup)
//   sessions   : OUTPUT — filled with assigned sessions
//   returns    : GreedyResult statistics
GreedyResult runGreedy(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions
);

// Print the full timetable as a formatted table (MON-FRI, slot 1-7)
void printTimetable(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap
);

// Print the greedy statistics
void printGreedyStats(const std::string& datasetSize,
                      const std::vector<Course>& courses,
                      const GreedyResult& result,
                      const ValidationReport& report);

// ─────────────────────────────────────────────────────────────────
// Phase 3: MRV + Backtracking Scheduler
// ─────────────────────────────────────────────────────────────────

// One concrete (day, slot, room) candidate
struct Candidate {
    std::string day;
    int         slot{-1};
    std::string roomId;
};

// Statistics returned by the MRV scheduler
struct MRVResult {
    int    totalSessions{0};
    int    scheduled{0};
    int    unscheduled{0};
    long   nodesExplored{0};    // recursive calls made
    long   attemptsCount{0};    // individual candidate assignments tried
    long   backtracks{0};       // times an assignment was undone
    bool   feasible{false};
    bool   timedOut{false};
    int    totalPenalty{0};
    double elapsedMs{0.0};
};

// Run the MRV + backtracking scheduler.
//   courses      : loaded, filtered courses
//   graph        : conflict graph (used for MRV tie-breaking by degree)
//   courseMap    : courseId -> Course
//   sessions     : OUTPUT — filled with the final assignment (or partial if timed out)
//   timeLimitMs  : abort and return partial result after this many ms (0 = no limit)
MRVResult runMRV(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions,
    double                                         timeLimitMs = 60000.0
);

// Print MRV statistics (reuses printTimetable from greedy for the timetable grid)
void printMRVStats(const std::string&         datasetSize,
                   const std::vector<Course>& courses,
                   const MRVResult&           result,
                   const ValidationReport&    report);

// ─────────────────────────────────────────────────────────────────
// Phase 4: Branch & Bound Soft-Constraint Optimizer
// ─────────────────────────────────────────────────────────────────

// Compute the GLOBAL soft-cost of a COMPLETE timetable.
// Unlike the incremental softPenalty(), this function sees the full
// assignment and also includes P4 (batch intra-day gap penalty).
// Only call on timetables where every session is assigned.
int globalPenalty(
    const std::vector<Session>&                    sessions,
    const std::unordered_map<std::string, Course>& courseMap
);

// Statistics produced by the B&B search
struct BnBResult {
    int    totalSessions{0};
    int    scheduled{0};
    int    unscheduled{0};
    long   nodesExplored{0};    // recursive calls to bnbSolve
    long   attemptsCount{0};    // candidate assignments actually tried
    long   backtracks{0};       // assignments undone after recursion
    long   prunedBranches{0};   // candidates skipped by cost bound
    int    solutionsFound{0};   // complete feasible timetables encountered
    int    bestCost{-1};        // soft cost of the best solution (-1 = none)
    bool   feasible{false};
    bool   timedOut{false};
    double elapsedMs{0.0};
};

// Run Branch & Bound optimizer.
//   courses          : loaded, filtered courses
//   graph            : conflict graph (MRV tie-breaking)
//   courseMap        : courseId -> Course
//   sessions         : OUTPUT — sessions in the best solution found
//   initialUpperBound: bestCost starts here; pass Greedy penalty for warm start
//                      (pass a very large number to let B&B find first solution
//                       unbiased, e.g. 2'000'000'000)
//   timeLimitMs      : abort after this many ms (0 = no limit)
BnBResult runBranchAndBound(
    const std::vector<Course>&                     courses,
    const ConflictGraph&                           graph,
    const std::unordered_map<std::string, Course>& courseMap,
    std::vector<Session>&                          sessions,
    int                                            initialUpperBound = 2000000000,
    double                                         timeLimitMs = 60000.0
);

// Print B&B statistics + validator report
void printBnBStats(const std::string&         datasetSize,
                   const std::vector<Course>& courses,
                   const BnBResult&           result,
                   const ValidationReport&    report,
                   int                        greedyPenalty = -1);
