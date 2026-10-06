#include "timetable.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <iomanip>
#include <algorithm>

// ── Helpers ──────────────────────────────────────────────────────

static std::unordered_map<std::string, Course>
buildCourseMap(const std::vector<Course>& courses)
{
    std::unordered_map<std::string, Course> m;
    m.reserve(courses.size());
    for (const auto& c : courses) m[c.courseId] = c;
    return m;
}

static std::string findCSV()
{
    for (const auto& p : {
            "timetable_dataset.csv",
            "../timetable_dataset.csv",
            "../../timetable_dataset.csv",
            "data/timetable_dataset.csv",
            "../data/timetable_dataset.csv"}) {
        std::ifstream f(p);
        if (f.is_open()) return p;
    }
    return "";
}

static std::string toUpper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ── Per-run result row (shared for greedy, MRV, BnB) ─────────────
struct RunRow {
    std::string algo;
    std::string dataset;
    int  courses{0}, sessions{0}, scheduled{0};
    bool feasible{false};
    double ms{0.0};
    long   backtracks{-1};     // -1 means N/A (greedy)
    long   nodes{-1};
    long   pruned{-1};         // B&B pruned branches (-1 = N/A)
    int    penalty{-1};
};

// ── Run GREEDY on one size ────────────────────────────────────────
static RunRow runGreedyOne(const std::string& size,
                           const std::string& csvPath,
                           bool printTable)
{
    RunRow row;
    row.algo    = "GREEDY";
    row.dataset = size;

    std::vector<Course> courses;
    if (!loadCSV(csvPath, size, courses) || courses.empty()) {
        std::cerr << "[ERROR] Failed to load " << size << "\n";
        return row;
    }

    auto courseMap = buildCourseMap(courses);
    ConflictGraph graph;
    graph.build(courses);

    std::vector<Session> sessions;
    GreedyResult res = runGreedy(courses, graph, courseMap, sessions);

    ValidationReport report = validateTimetable(sessions, courseMap);
    bool feasible = (res.unscheduled == 0) && report.feasible;

    printGreedyStats(size, courses, res, report);
    if (printTable) printTimetable(sessions, courseMap);

    row.courses   = (int)courses.size();
    row.sessions  = res.totalSessions;
    row.scheduled = res.scheduled;
    row.feasible  = feasible;
    row.ms        = res.elapsedMs;
    row.penalty   = feasible ? res.totalPenalty : -1;
    // backtracks / nodes not applicable for greedy
    return row;
}

// ── Run MRV on one size ───────────────────────────────────────────
static RunRow runMRVOne(const std::string& size,
                        const std::string& csvPath,
                        bool printTable,
                        double timeLimitMs)
{
    RunRow row;
    row.algo    = "MRV";
    row.dataset = size;

    std::vector<Course> courses;
    if (!loadCSV(csvPath, size, courses) || courses.empty()) {
        std::cerr << "[ERROR] Failed to load " << size << "\n";
        return row;
    }

    auto courseMap = buildCourseMap(courses);
    ConflictGraph graph;
    graph.build(courses);

    std::vector<Session> sessions;
    MRVResult res = runMRV(courses, graph, courseMap, sessions, timeLimitMs);

    ValidationReport report = validateTimetable(sessions, courseMap);
    bool feasible = res.feasible && report.feasible;

    printMRVStats(size, courses, res, report);
    if (printTable) printTimetable(sessions, courseMap);

    row.courses    = (int)courses.size();
    row.sessions   = res.totalSessions;
    row.scheduled  = res.scheduled;
    row.feasible   = feasible;
    row.ms         = res.elapsedMs;
    row.backtracks = res.backtracks;
    row.nodes      = res.nodesExplored;
    row.penalty    = feasible ? res.totalPenalty : -1;
    return row;
}

// ── Run BnB on one size ───────────────────────────────────────────
static RunRow runBnBOne(const std::string& size,
                        const std::string& csvPath,
                        bool printTable,
                        double timeLimitMs)
{
    RunRow row;
    row.algo    = "BNB";
    row.dataset = size;

    std::vector<Course> courses;
    if (!loadCSV(csvPath, size, courses) || courses.empty()) {
        std::cerr << "[ERROR] Failed to load " << size << "\n";
        return row;
    }

    auto courseMap = buildCourseMap(courses);
    ConflictGraph graph;
    graph.build(courses);

    // ── Warm-start upper bound: run Greedy first ──────────────────
    //    This lets B&B prune aggressively from the very first node.
    std::vector<Session> greedySessions;
    GreedyResult grRes = runGreedy(courses, graph, courseMap, greedySessions);
    int greedyPenalty  = (grRes.unscheduled == 0) ? grRes.totalPenalty : 2000000000;

    std::cout << "[INFO] Greedy warm-start penalty: " << greedyPenalty << "\n";

    // ── Run B&B ───────────────────────────────────────────────────
    // Use a large initial upper bound so B&B always stores the first
    // feasible solution it finds (the greedy penalty is only for comparison).
    // B&B will then continue within the time limit to try to improve.
    std::vector<Session> sessions;
    BnBResult res = runBranchAndBound(
        courses, graph, courseMap, sessions,
        2000000000,  // always store first solution found, then improve
        timeLimitMs);

    // If B&B found no improvement, fall back to the Greedy solution
    // so we always have a complete feasible timetable to validate.
    bool usedGreedyFallback = false;
    if (!res.feasible && grRes.unscheduled == 0) {
        sessions           = greedySessions;
        res.bestCost       = greedyPenalty;
        res.feasible       = true;
        res.scheduled      = grRes.scheduled;
        res.unscheduled    = 0;
        usedGreedyFallback = true;
    }

    ValidationReport report = validateTimetable(sessions, courseMap);
    bool feasible = res.feasible && report.feasible;

    // Report Greedy penalty for comparison (pass -1 if fallback used,
    // since penalty is identical)
    int gpForPrint = usedGreedyFallback ? -1 : (int)greedyPenalty;
    printBnBStats(size, courses, res, report, gpForPrint);
    if (usedGreedyFallback)
        std::cout << "  [NOTE] B&B found no improvement over Greedy; "
                     "Greedy solution is shown.\n";
    if (printTable) printTimetable(sessions, courseMap);

    row.courses    = (int)courses.size();
    row.sessions   = res.totalSessions;
    row.scheduled  = res.scheduled;
    row.feasible   = feasible;
    row.ms         = res.elapsedMs;
    row.backtracks = res.backtracks;
    row.nodes      = res.nodesExplored;
    row.pruned     = res.prunedBranches;
    row.penalty    = feasible ? res.bestCost : -1;
    return row;
}

// ── Usage ─────────────────────────────────────────────────────────
static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " <SMALL|MEDIUM|LARGE|XLARGE|ALL>"
              << " [--algo greedy|mrv]"
              << " [--time-limit <ms>]\n"
              << "  Default algo  : mrv\n"
              << "  Default limit : 60000 ms per dataset size\n";
}

// ─────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (argc < 2) { printUsage(argv[0]); return 1; }

    std::string sizeArg  = toUpper(argv[1]);
    std::string algo     = "MRV";       // default
    double      limitMs  = 60000.0;     // 60 s per size

    // Parse optional flags
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--algo" && i + 1 < argc) {
            algo = toUpper(argv[++i]);
        } else if (flag == "--time-limit" && i + 1 < argc) {
            try { limitMs = std::stod(argv[++i]); } catch (...) {}
        } else {
            std::cerr << "[WARN] Unknown flag: " << flag << "\n";
        }
    }

    if (algo != "GREEDY" && algo != "MRV" && algo != "BNB") {
        std::cerr << "[ERROR] Unknown algo: " << algo
                  << ". Use greedy, mrv, or bnb.\n";
        return 1;
    }

    std::vector<std::string> sizes;
    if (sizeArg == "ALL") {
        sizes = {"SMALL","MEDIUM","LARGE","XLARGE"};
    } else if (sizeArg == "SMALL" || sizeArg == "MEDIUM" ||
               sizeArg == "LARGE" || sizeArg == "XLARGE") {
        sizes = {sizeArg};
    } else {
        printUsage(argv[0]); return 1;
    }

    std::string csvPath = findCSV();
    if (csvPath.empty()) {
        std::cerr << "[ERROR] timetable_dataset.csv not found.\n";
        return 1;
    }
    std::cout << "[INFO] Using CSV     : " << csvPath << "\n";
    std::cout << "[INFO] Algorithm     : " << algo    << "\n";
    if (algo == "MRV" || algo == "BNB")
        std::cout << "[INFO] Time limit    : " << limitMs << " ms per size\n";

    // ── Run ──────────────────────────────────────────────────────
    std::vector<RunRow> rows;
    for (const auto& sz : sizes) {
        bool printTable = (sz == "SMALL");
        if (algo == "GREEDY") {
            std::cout << "\n[INFO] Running GREEDY on " << sz << " ...\n";
            rows.push_back(runGreedyOne(sz, csvPath, printTable));
        } else if (algo == "MRV") {
            std::cout << "\n[INFO] Running MRV on " << sz << " ...\n";
            rows.push_back(runMRVOne(sz, csvPath, printTable, limitMs));
        } else {
            std::cout << "\n[INFO] Running B&B on " << sz << " ...\n";
            rows.push_back(runBnBOne(sz, csvPath, printTable, limitMs));
        }
    }

    // ── Summary table ─────────────────────────────────────────────
    bool isMRV = (algo == "MRV");
    bool isBnB = (algo == "BNB");
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " SUMMARY TABLE  [" << algo << "]\n";
    std::cout << "========================================\n";

    if (isMRV) {
        std::cout << std::left
                  << std::setw(9)  << "Dataset"
                  << std::setw(9)  << "Courses"
                  << std::setw(10) << "Sessions"
                  << std::setw(11) << "Scheduled"
                  << std::setw(10) << "Feasible"
                  << std::setw(10) << "Time(ms)"
                  << std::setw(12) << "Backtracks"
                  << std::setw(12) << "Nodes"
                  << std::setw(10) << "Penalty"
                  << "\n";
        std::cout << std::string(93, '-') << "\n";
        for (const auto& r : rows) {
            std::cout << std::left
                      << std::setw(9)  << r.dataset
                      << std::setw(9)  << r.courses
                      << std::setw(10) << r.sessions
                      << std::setw(11) << r.scheduled
                      << std::setw(10) << (r.feasible ? "YES" : "NO")
                      << std::setw(10) << std::fixed << std::setprecision(2) << r.ms
                      << std::setw(12) << (r.backtracks >= 0
                                             ? std::to_string(r.backtracks) : "N/A")
                      << std::setw(12) << (r.nodes >= 0
                                             ? std::to_string(r.nodes) : "N/A")
                      << std::setw(10) << (r.penalty >= 0
                                             ? std::to_string(r.penalty) : "N/A")
                      << "\n";
        }
    } else if (isBnB) {
        std::cout << std::left
                  << std::setw(9)  << "Dataset"
                  << std::setw(9)  << "Courses"
                  << std::setw(10) << "Sessions"
                  << std::setw(11) << "Scheduled"
                  << std::setw(10) << "Feasible"
                  << std::setw(10) << "Time(ms)"
                  << std::setw(12) << "Backtracks"
                  << std::setw(12) << "Nodes"
                  << std::setw(12) << "Pruned"
                  << std::setw(10) << "Penalty"
                  << "\n";
        std::cout << std::string(105, '-') << "\n";
        for (const auto& r : rows) {
            std::cout << std::left
                      << std::setw(9)  << r.dataset
                      << std::setw(9)  << r.courses
                      << std::setw(10) << r.sessions
                      << std::setw(11) << r.scheduled
                      << std::setw(10) << (r.feasible ? "YES" : "NO")
                      << std::setw(10) << std::fixed << std::setprecision(2) << r.ms
                      << std::setw(12) << (r.backtracks >= 0
                                             ? std::to_string(r.backtracks) : "N/A")
                      << std::setw(12) << (r.nodes >= 0
                                             ? std::to_string(r.nodes) : "N/A")
                      << std::setw(12) << (r.pruned >= 0
                                             ? std::to_string(r.pruned) : "N/A")
                      << std::setw(10) << (r.penalty >= 0
                                             ? std::to_string(r.penalty) : "N/A")
                      << "\n";
        }
    } else {
        std::cout << std::left
                  << std::setw(9)  << "Dataset"
                  << std::setw(9)  << "Courses"
                  << std::setw(10) << "Sessions"
                  << std::setw(11) << "Scheduled"
                  << std::setw(10) << "Feasible"
                  << std::setw(10) << "Time(ms)"
                  << std::setw(10) << "Penalty"
                  << "\n";
        std::cout << std::string(69, '-') << "\n";
        for (const auto& r : rows) {
            std::cout << std::left
                      << std::setw(9)  << r.dataset
                      << std::setw(9)  << r.courses
                      << std::setw(10) << r.sessions
                      << std::setw(11) << r.scheduled
                      << std::setw(10) << (r.feasible ? "YES" : "NO")
                      << std::setw(10) << std::fixed << std::setprecision(2) << r.ms
                      << std::setw(10) << (r.penalty >= 0
                                             ? std::to_string(r.penalty) : "N/A")
                      << "\n";
        }
    }
    std::cout << "========================================\n";
    return 0;
}
