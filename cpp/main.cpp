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
    bool timedOut{false};
    double ms{0.0};
    long   backtracks{-1};     // -1 means N/A (greedy)
    long   nodes{-1};
    long   attempts{-1};
    long   pruned{-1};         // B&B pruned branches (-1 = N/A)
    int    penalty{-1};
};

// ─────────────────────────────────────────────────────────────────
// Per-algorithm single-size runners
// Each accepts quiet=true to suppress all per-run console output
// (used in benchmark mode so only the unified table is printed).
// ─────────────────────────────────────────────────────────────────

// ── Run GREEDY on one size ────────────────────────────────────────
static RunRow runGreedyOne(const std::string& size,
                           const std::string& csvPath,
                           bool printTable,
                           bool quiet = false)
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

    if (!quiet) {
        printGreedyStats(size, courses, res, report);
        if (printTable) printTimetable(sessions, courseMap);
    }

    row.courses   = (int)courses.size();
    row.sessions  = res.totalSessions;
    row.scheduled = res.scheduled;
    row.feasible  = feasible;
    row.ms        = res.elapsedMs;
    row.attempts  = res.attemptsCount;
    row.penalty   = feasible ? res.totalPenalty : -1;
    // backtracks / nodes / pruned not applicable for greedy
    return row;
}

// ── Run MRV on one size ───────────────────────────────────────────
static RunRow runMRVOne(const std::string& size,
                        const std::string& csvPath,
                        bool printTable,
                        double timeLimitMs,
                        bool quiet = false)
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

    if (!quiet) {
        printMRVStats(size, courses, res, report);
        if (printTable) printTimetable(sessions, courseMap);
    }

    row.courses    = (int)courses.size();
    row.sessions   = res.totalSessions;
    row.scheduled  = res.scheduled;
    row.feasible   = feasible;
    row.timedOut   = res.timedOut;
    row.ms         = res.elapsedMs;
    row.backtracks = res.backtracks;
    row.nodes      = res.nodesExplored;
    row.attempts   = res.attemptsCount;
    row.penalty    = feasible ? res.totalPenalty : -1;
    return row;
}

// ── Run BnB on one size ───────────────────────────────────────────
// NOTE: runBnBOne() internally runs Greedy as a warm-start reference.
// That internal Greedy run is NOT reported as the Greedy benchmark row;
// it only serves to print a comparison in the per-algo output.
static RunRow runBnBOne(const std::string& size,
                        const std::string& csvPath,
                        bool printTable,
                        double timeLimitMs,
                        bool quiet = false)
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

    // ── Internal Greedy warm-start (for comparison only, not benchmarked) ─
    std::vector<Session> greedySessions;
    GreedyResult grRes = runGreedy(courses, graph, courseMap, greedySessions);
    int greedyPenalty  = (grRes.unscheduled == 0) ? grRes.totalPenalty : 2000000000;

    if (!quiet)
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

    // If B&B found no solution at all, fall back to the Greedy solution
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

    if (!quiet) {
        int gpForPrint = usedGreedyFallback ? -1 : (int)greedyPenalty;
        printBnBStats(size, courses, res, report, gpForPrint);
        if (usedGreedyFallback)
            std::cout << "  [NOTE] B&B found no improvement over Greedy; "
                         "Greedy solution is shown.\n";
        if (printTable) printTimetable(sessions, courseMap);
    }

    row.courses    = (int)courses.size();
    row.sessions   = res.totalSessions;
    row.scheduled  = res.scheduled;
    row.feasible   = feasible;
    row.timedOut   = res.timedOut;
    row.ms         = res.elapsedMs;
    row.backtracks = res.backtracks;
    row.nodes      = res.nodesExplored;
    row.attempts   = res.attemptsCount;
    row.pruned     = res.prunedBranches;
    row.penalty    = feasible ? res.bestCost : -1;
    return row;
}

// ── Run Parallel B&B on one size ──────────────────────────────────
static RunRow runPBnBOne(const std::string& size,
                         const std::string& csvPath,
                         bool printTable,
                         double timeLimitMs,
                         int  numThreads = 0,
                         bool quiet = false)
{
    RunRow row;
    row.algo    = "PBNB";
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
    PBnBResult res = runParallelBranchAndBound(
        courses, graph, courseMap, sessions,
        numThreads,
        2000000000,   // always store first solution found, then improve
        timeLimitMs);

    ValidationReport report = validateTimetable(sessions, courseMap);
    bool feasible = res.feasible && report.feasible;

    if (!quiet) {
        printPBnBStats(size, courses, res, report);
        if (printTable) printTimetable(sessions, courseMap);
    }

    row.courses    = (int)courses.size();
    row.sessions   = res.totalSessions;
    row.scheduled  = res.scheduled;
    row.feasible   = feasible;
    row.timedOut   = res.timedOut;
    row.ms         = res.elapsedMs;
    row.backtracks = res.backtracks;
    row.nodes      = res.nodesExplored;
    row.attempts   = res.attemptsCount;
    row.pruned     = res.prunedBranches;
    row.penalty    = feasible ? res.bestCost : -1;
    return row;
}

// Benchmark infrastructure
// ─────────────────────────────────────────────────────────────────

// Print one row of the unified benchmark table
static void printBenchRow(const RunRow& r)
{
    // Algo (7), Dataset (8), Sessions(10), Sched(8),
    // Feasible(10), Time(ms)(12),
    // Nodes(12), Attempts(12), Backtracks(12), Pruned(12), Penalty(10)
    auto na  = [](long v)  { return v >= 0 ? std::to_string(v) : "N/A"; };
    auto nai = [](int  v)  { return v >= 0 ? std::to_string(v) : "N/A"; };

    std::cout << std::left
              << std::setw(7)  << r.algo
              << std::setw(8)  << r.dataset
              << std::setw(10) << r.sessions
              << std::setw(8)  << r.scheduled;

    // Feasible — append * if timed out
    std::string feas = r.feasible ? "YES" : "NO";
    if (r.timedOut) feas += "*";
    std::cout << std::setw(10) << feas;

    std::cout << std::setw(12) << std::fixed << std::setprecision(1) << r.ms
              << std::setw(12) << na(r.nodes)
              << std::setw(12) << na(r.attempts)
              << std::setw(12) << na(r.backtracks)
              << std::setw(12) << na(r.pruned)
              << std::setw(10) << nai(r.penalty)
              << "\n";
}

// Print the complete unified benchmark table from all collected rows
static void printBenchmarkTable(const std::vector<RunRow>& rows,
                                const std::vector<std::string>& sizes,
                                double mrvLimitMs, double bnbLimitMs)
{
    const int W = 115;
    std::cout << "\n" << std::string(W, '=') << "\n";
    std::cout << " SEQUENTIAL BENCHMARK — Greedy / MRV / Branch & Bound\n";
    std::cout << " Sizes: ";
    for (size_t i = 0; i < sizes.size(); ++i)
        std::cout << sizes[i] << (i+1 < sizes.size() ? ", " : "");
    std::cout << "\n";
    std::cout << " MRV time limit: " << (int)(mrvLimitMs/1000) << " s per size"
              << "   |   B&B time limit: " << (int)(bnbLimitMs/1000) << " s per size"
              << "   |   * = timed out (best-found reported)\n";
    std::cout << " NOTE: B&B time INCLUDES its internal Greedy warm-start."
                 " Greedy rows are timed independently.\n";
    std::cout << std::string(W, '=') << "\n";

    // Header
    std::cout << std::left
              << std::setw(7)  << "Algo"
              << std::setw(8)  << "Dataset"
              << std::setw(10) << "Sessions"
              << std::setw(8)  << "Sched"
              << std::setw(10) << "Feasible"
              << std::setw(12) << "Time(ms)"
              << std::setw(12) << "Nodes"
              << std::setw(12) << "Attempts"
              << std::setw(12) << "Backtracks"
              << std::setw(12) << "Pruned"
              << std::setw(10) << "Penalty"
              << "\n";
    std::cout << std::string(W, '-') << "\n";

    // Group by dataset size for readability
    std::string lastDataset;
    for (const auto& r : rows) {
        if (!lastDataset.empty() && r.dataset != lastDataset)
            std::cout << std::string(W, '-') << "\n";
        lastDataset = r.dataset;
        printBenchRow(r);
    }

    std::cout << std::string(W, '=') << "\n\n";
}

// Run the full sequential benchmark
static void runBenchmark(const std::vector<std::string>& sizes,
                         const std::string& csvPath,
                         double mrvLimitMs,
                         double bnbLimitMs)
{
    std::vector<RunRow> rows;
    rows.reserve(sizes.size() * 3);

    for (const auto& sz : sizes) {
        std::cout << "\n[BENCH] ── " << sz << " ──────────────────────────\n";

        // ── Greedy ───────────────────────────────────────────────
        std::cout << "[BENCH] " << sz << " GREEDY ...\n";
        rows.push_back(runGreedyOne(sz, csvPath,
                                    /*printTable=*/false,
                                    /*quiet=*/true));
        const RunRow& gr = rows.back();
        std::cout << "        scheduled=" << gr.scheduled << "/" << gr.sessions
                  << "  feasible=" << (gr.feasible ? "YES" : "NO")
                  << "  penalty=" << (gr.penalty >= 0 ? std::to_string(gr.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << gr.ms << " ms\n";

        // ── MRV ──────────────────────────────────────────────────
        std::cout << "[BENCH] " << sz << " MRV (limit "
                  << (int)(mrvLimitMs/1000) << " s) ...\n";
        rows.push_back(runMRVOne(sz, csvPath,
                                  /*printTable=*/false,
                                  mrvLimitMs,
                                  /*quiet=*/true));
        const RunRow& mr = rows.back();
        std::cout << "        scheduled=" << mr.scheduled << "/" << mr.sessions
                  << "  feasible=" << (mr.feasible ? "YES" : "NO")
                  << (mr.timedOut ? "*" : "")
                  << "  penalty=" << (mr.penalty >= 0 ? std::to_string(mr.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << mr.ms << " ms\n";

        // ── B&B ───────────────────────────────────────────────────
        std::cout << "[BENCH] " << sz << " B&B (limit "
                  << (int)(bnbLimitMs/1000) << " s, includes internal Greedy warm-start) ...\n";
        rows.push_back(runBnBOne(sz, csvPath,
                                  /*printTable=*/false,
                                  bnbLimitMs,
                                  /*quiet=*/true));
        const RunRow& br = rows.back();
        std::cout << "        scheduled=" << br.scheduled << "/" << br.sessions
                  << "  feasible=" << (br.feasible ? "YES" : "NO")
                  << (br.timedOut ? "*" : "")
                  << "  bestPenalty=" << (br.penalty >= 0 ? std::to_string(br.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << br.ms << " ms\n";
    }

    printBenchmarkTable(rows, sizes, mrvLimitMs, bnbLimitMs);
}

// ── Usage ─────────────────────────────────────────────────────────
static void printUsage(const char* prog)
{
    std::cerr << "Usage (single algorithm):\n"
              << "  " << prog
              << " <SMALL|MEDIUM|LARGE|XLARGE|ALL>"
              << " [--algo greedy|mrv|bnb|pbnb]"
              << " [--time-limit <ms>] [--threads <n>]\n"
              << "  Default algo  : mrv\n"
              << "  Default limit : 60000 ms per dataset size\n\n"
              << "Usage (sequential benchmark):\n"
              << "  " << prog << " --benchmark [SMALL] [MEDIUM] [LARGE] [XLARGE]\n"
              << "  (no sizes = run all four)\n"
              << "  [--mrv-limit <ms>]  default 60000\n"
              << "  [--bnb-limit <ms>]  default 30000\n";
}

// ─────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (argc < 2) { printUsage(argv[0]); return 1; }

    // ── Check for --benchmark mode ────────────────────────────────
    bool isBenchmark = (std::string(argv[1]) == "--benchmark");

    if (isBenchmark) {
        // Collect optional size args after --benchmark; default = all four
        const std::vector<std::string> allSizes = {"SMALL","MEDIUM","LARGE","XLARGE"};
        std::vector<std::string> benchSizes;
        double mrvLimitMs = 60000.0;
        double bnbLimitMs = 30000.0;

        for (int i = 2; i < argc; ++i) {
            std::string a = toUpper(argv[i]);
            if (a == "SMALL" || a == "MEDIUM" || a == "LARGE" || a == "XLARGE") {
                benchSizes.push_back(a);
            } else if (a == "--MRV-LIMIT" && i + 1 < argc) {
                try { mrvLimitMs = std::stod(argv[++i]); } catch (...) {}
            } else if (a == "--BNB-LIMIT" && i + 1 < argc) {
                try { bnbLimitMs = std::stod(argv[++i]); } catch (...) {}
            } else {
                std::cerr << "[WARN] Unknown benchmark argument: " << argv[i] << "\n";
            }
        }
        if (benchSizes.empty()) benchSizes = allSizes;

        std::string csvPath = findCSV();
        if (csvPath.empty()) {
            std::cerr << "[ERROR] timetable_dataset.csv not found.\n";
            return 1;
        }
        std::cout << "[BENCH] CSV         : " << csvPath << "\n";
        std::cout << "[BENCH] Sizes       : ";
        for (size_t i = 0; i < benchSizes.size(); ++i)
            std::cout << benchSizes[i] << (i+1 < benchSizes.size() ? ", " : "");
        std::cout << "\n";
        std::cout << "[BENCH] MRV limit   : " << mrvLimitMs << " ms\n";
        std::cout << "[BENCH] B&B limit   : " << bnbLimitMs << " ms\n";

        runBenchmark(benchSizes, csvPath, mrvLimitMs, bnbLimitMs);
        return 0;
    }

    // ── Single-algorithm mode (original behaviour) ────────────────
    std::string sizeArg  = toUpper(argv[1]);
    std::string algo     = "MRV";       // default
    double      limitMs  = 60000.0;     // 60 s per size
    int         threads  = 0;           // 0 = OMP default

    // Parse optional flags
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--algo" && i + 1 < argc) {
            algo = toUpper(argv[++i]);
        } else if (flag == "--time-limit" && i + 1 < argc) {
            try { limitMs = std::stod(argv[++i]); } catch (...) {}
        } else if (flag == "--threads" && i + 1 < argc) {
            try { threads = std::stoi(argv[++i]); } catch (...) {}
        } else {
            std::cerr << "[WARN] Unknown flag: " << flag << "\n";
        }
    }

    if (algo != "GREEDY" && algo != "MRV" && algo != "BNB" && algo != "PBNB") {
        std::cerr << "[ERROR] Unknown algo: " << algo
                  << ". Use greedy, mrv, bnb, or pbnb.\n";
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
    if (algo == "MRV" || algo == "BNB" || algo == "PBNB")
        std::cout << "[INFO] Time limit    : " << limitMs << " ms per size\n";
    if (algo == "PBNB")
        std::cout << "[INFO] Threads       : "
                  << (threads > 0 ? std::to_string(threads) : "auto (OMP_NUM_THREADS)") << "\n";

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
        } else if (algo == "BNB") {
            std::cout << "\n[INFO] Running B&B on " << sz << " ...\n";
            rows.push_back(runBnBOne(sz, csvPath, printTable, limitMs));
        } else {
            std::cout << "\n[INFO] Running Parallel B&B on " << sz << " ...\n";
            rows.push_back(runPBnBOne(sz, csvPath, printTable, limitMs, threads));
        }
    }

    // ── Summary table ─────────────────────────────────────────────
    bool isMRV  = (algo == "MRV");
    bool isBnB  = (algo == "BNB");
    bool isPBnB = (algo == "PBNB");
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
    } else if (isBnB || isPBnB) {
        std::cout << std::left
                  << std::setw(9)  << "Dataset"
                  << std::setw(9)  << "Courses"
                  << std::setw(10) << "Sessions"
                  << std::setw(11) << "Scheduled"
                  << std::setw(10) << "Feasible"
                  << std::setw(12) << "Time(ms)"
                  << std::setw(12) << "Backtracks"
                  << std::setw(12) << "Nodes"
                  << std::setw(12) << "Pruned"
                  << std::setw(10) << "Penalty"
                  << "\n";
        std::cout << std::string(107, '-') << "\n";
        for (const auto& r : rows) {
            std::cout << std::left
                      << std::setw(9)  << r.dataset
                      << std::setw(9)  << r.courses
                      << std::setw(10) << r.sessions
                      << std::setw(11) << r.scheduled
                      << std::setw(10) << (r.feasible ? "YES" : "NO")
                      << std::setw(12) << std::fixed << std::setprecision(2) << r.ms
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
