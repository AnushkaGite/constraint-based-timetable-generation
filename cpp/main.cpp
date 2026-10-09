#include "timetable.h"
#include <iostream>
#include <fstream>
#include <sstream>
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
    int  threads{1};           // 1 for sequential, P for PBNB
    int  courses{0}, sessions{0}, scheduled{0};
    bool feasible{false};
    bool timedOut{false};
    double ms{0.0};
    long   backtracks{-1};     // -1 means N/A (greedy)
    long   nodes{-1};
    long   attempts{-1};
    long   pruned{-1};         // B&B pruned branches (-1 = N/A)
    int    penalty{-1};

    // Parallel comparison metrics
    double seqMs{-1.0};        // Sequential B&B runtime
    double parMs{-1.0};        // Parallel B&B runtime
    double speedup{-1.0};      // seqMs / parMs
    double efficiency{-1.0};   // speedup / threads

    // Additive fields for JSON export / UI
    int    unscheduled{0};
    bool   usedGreedyFallback{false};
    double timeLimitMs{0.0};
    ValidationReport validation;
    std::vector<Session> sessionsData;
    std::unordered_map<std::string, Course> courseMapData;
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
    row.unscheduled   = res.unscheduled;
    row.timeLimitMs   = 0.0;
    row.validation    = report;
    row.sessionsData  = sessions;
    row.courseMapData = courseMap;
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
    row.unscheduled   = res.unscheduled;
    row.timeLimitMs   = timeLimitMs;
    row.validation    = report;
    row.sessionsData  = sessions;
    row.courseMapData = courseMap;
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
    row.threads    = 1;
    row.seqMs      = res.elapsedMs;
    row.unscheduled        = res.unscheduled;
    row.usedGreedyFallback = usedGreedyFallback;
    row.timeLimitMs        = timeLimitMs;
    row.validation         = report;
    row.sessionsData       = sessions;
    row.courseMapData      = courseMap;
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
    row.threads    = (res.threadsUsed > 0) ? res.threadsUsed : (numThreads > 0 ? numThreads : 1);
    row.parMs      = res.elapsedMs;
    row.unscheduled   = res.unscheduled;
    row.timeLimitMs   = timeLimitMs;
    row.validation    = report;
    row.sessionsData  = sessions;
    row.courseMapData = courseMap;
    return row;
}

// Benchmark infrastructure & CSV / JSON Export
// ─────────────────────────────────────────────────────────────────

static std::string escapeJSON(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if ((unsigned char)c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

static void writeRunJSON(const std::string& filePath, const RunRow& r) {
    std::ofstream out(filePath);
    if (!out.is_open()) {
        std::cerr << "[ERROR] Could not open " << filePath << " for writing JSON.\n";
        return;
    }

    auto toLowerStr = [](std::string s) {
        for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    auto nullOrLong = [](long v) -> std::string {
        return v >= 0 ? std::to_string(v) : "null";
    };
    auto nullOrInt = [](int v) -> std::string {
        return v >= 0 ? std::to_string(v) : "null";
    };

    out << "{\n";
    out << "  \"schema\": 1,\n";

    // meta
    out << "  \"meta\": {\n";
    out << "    \"dataset\": \"" << escapeJSON(r.dataset) << "\",\n";
    out << "    \"algorithm\": \"" << escapeJSON(toLowerStr(r.algo)) << "\",\n";
    out << "    \"threads\": " << r.threads << ",\n";
    out << "    \"timeLimitMs\": " << std::fixed << std::setprecision(1) << r.timeLimitMs << "\n";
    out << "  },\n";

    // metrics
    out << "  \"metrics\": {\n";
    out << "    \"courses\": " << r.courses << ",\n";
    out << "    \"sessions\": " << r.sessions << ",\n";
    out << "    \"scheduled\": " << r.scheduled << ",\n";
    out << "    \"unscheduled\": " << r.unscheduled << ",\n";
    out << "    \"feasible\": " << (r.feasible ? "true" : "false") << ",\n";
    out << "    \"timedOut\": " << (r.timedOut ? "true" : "false") << ",\n";
    out << "    \"runtimeMs\": " << std::fixed << std::setprecision(2) << r.ms << ",\n";
    out << "    \"nodes\": " << nullOrLong(r.nodes) << ",\n";
    out << "    \"attempts\": " << nullOrLong(r.attempts) << ",\n";
    out << "    \"backtracks\": " << nullOrLong(r.backtracks) << ",\n";
    out << "    \"pruned\": " << nullOrLong(r.pruned) << ",\n";
    out << "    \"penalty\": " << nullOrInt(r.penalty) << ",\n";
    out << "    \"usedGreedyFallback\": " << (r.usedGreedyFallback ? "true" : "false") << "\n";
    out << "  },\n";

    // validation
    out << "  \"validation\": {\n";
    out << "    \"facultyConflicts\": " << r.validation.facultyConflicts << ",\n";
    out << "    \"batchConflicts\": " << r.validation.batchConflicts << ",\n";
    out << "    \"roomConflicts\": " << r.validation.roomConflicts << ",\n";
    out << "    \"capacityViolations\": " << r.validation.capacityViolations << ",\n";
    out << "    \"roomTypeViolations\": " << r.validation.roomTypeViolations << ",\n";
    out << "    \"availabilityViolations\": " << r.validation.availabilityViolations << ",\n";
    out << "    \"labViolations\": " << r.validation.labViolations << ",\n";
    out << "    \"missingSessions\": " << r.validation.missingSessions << ",\n";
    out << "    \"feasible\": " << (r.validation.feasible ? "true" : "false") << "\n";
    out << "  },\n";

    // sessions
    out << "  \"sessions\": [\n";
    for (size_t i = 0; i < r.sessionsData.size(); ++i) {
        const auto& s = r.sessionsData[i];
        auto cit = r.courseMapData.find(s.courseId);

        std::string cname = "", ctype = "", fac = "", batch = "";
        int bstrength = 0, dur = 1;
        if (cit != r.courseMapData.end()) {
            cname     = cit->second.courseName;
            ctype     = cit->second.courseType;
            fac       = cit->second.facultyId;
            batch     = cit->second.batchId;
            bstrength = cit->second.batchStrength;
            dur       = cit->second.duration;
        }

        bool assigned = (!s.day.empty() && s.startSlot != -1 && !s.roomId.empty());

        int roomCap = -1;
        if (assigned && cit != r.courseMapData.end()) {
            for (const auto& ro : cit->second.roomOptions) {
                if (ro.roomId == s.roomId) {
                    roomCap = ro.capacity;
                    break;
                }
            }
        }

        out << "    {\n";
        out << "      \"sessionId\": \"" << escapeJSON(s.sessionId) << "\",\n";
        out << "      \"courseId\": \"" << escapeJSON(s.courseId) << "\",\n";
        out << "      \"courseName\": \"" << escapeJSON(cname) << "\",\n";
        out << "      \"courseType\": \"" << escapeJSON(ctype) << "\",\n";
        out << "      \"facultyId\": \"" << escapeJSON(fac) << "\",\n";
        out << "      \"batchId\": \"" << escapeJSON(batch) << "\",\n";
        out << "      \"batchStrength\": " << bstrength << ",\n";
        out << "      \"duration\": " << dur << ",\n";
        out << "      \"assigned\": " << (assigned ? "true" : "false") << ",\n";

        if (assigned) {
            out << "      \"day\": \"" << escapeJSON(s.day) << "\",\n";
            out << "      \"startSlot\": " << s.startSlot << ",\n";
            int h1 = 8 + s.startSlot - 1;
            int h2 = h1 + dur;
            char sbuf[32];
            std::snprintf(sbuf, sizeof(sbuf), "%02d:00 - %02d:00", h1, h2);
            out << "      \"slotLabel\": \"" << sbuf << "\",\n";
            out << "      \"roomId\": \"" << escapeJSON(s.roomId) << "\",\n";
            if (roomCap >= 0)
                out << "      \"roomCapacity\": " << roomCap << "\n";
            else
                out << "      \"roomCapacity\": null\n";
        } else {
            out << "      \"day\": null,\n";
            out << "      \"startSlot\": null,\n";
            out << "      \"slotLabel\": null,\n";
            out << "      \"roomId\": null,\n";
            out << "      \"roomCapacity\": null\n";
        }

        out << "    }" << (i + 1 < r.sessionsData.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    // constants
    out << "  \"constants\": {\n";
    out << "    \"days\": [\"MON\", \"TUE\", \"WED\", \"THU\", \"FRI\"],\n";
    out << "    \"slotsPerDay\": " << SLOTS_PER_DAY << ",\n";
    out << "    \"lunchAfterSlot\": " << LUNCH_BREAK_AFTER << ",\n";
    out << "    \"slotLabels\": [\n";
    for (int sl = 1; sl <= SLOTS_PER_DAY; ++sl) {
        int h1 = 8 + sl - 1;
        int h2 = h1 + 1;
        char lbuf[32];
        std::snprintf(lbuf, sizeof(lbuf), "%02d:00 - %02d:00", h1, h2);
        out << "      \"" << lbuf << "\"" << (sl < SLOTS_PER_DAY ? "," : "") << "\n";
    }
    out << "    ]\n";
    out << "  }\n";
    out << "}\n";

    std::cout << "[INFO] Run exported to JSON: " << filePath << "\n";
}

// Write benchmark results to CSV (appends if file exists, writes header if new)
static void writeBenchmarkCSV(const std::string& csvFilePath, const std::vector<RunRow>& rows)
{
    bool fileExists = false;
    {
        std::ifstream f(csvFilePath);
        if (f.good()) fileExists = true;
    }

    std::ofstream out(csvFilePath, std::ios::app);
    if (!out.is_open()) {
        std::cerr << "[WARN] Could not open " << csvFilePath << " for writing.\n";
        return;
    }

    if (!fileExists) {
        out << "Dataset,Algorithm,Threads,Courses,Sessions,Scheduled,Feasible,TimedOut,"
            << "Runtime_ms,Nodes,Attempts,Backtracks,Pruned,Penalty,"
            << "Sequential_Runtime_ms,Parallel_Runtime_ms,Speedup,Efficiency\n";
    }

    auto naLong = [](long v) -> std::string { return v >= 0 ? std::to_string(v) : "N/A"; };
    auto naInt  = [](int v)  -> std::string { return v >= 0 ? std::to_string(v) : "N/A"; };
    auto naDbl  = [](double v) -> std::string {
        if (v < 0.0) return "N/A";
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << v;
        return ss.str();
    };

    for (const auto& r : rows) {
        out << r.dataset << ","
            << r.algo << ","
            << r.threads << ","
            << r.courses << ","
            << r.sessions << ","
            << r.scheduled << ","
            << (r.feasible ? "YES" : "NO") << ","
            << (r.timedOut ? "YES" : "NO") << ","
            << std::fixed << std::setprecision(2) << r.ms << ","
            << naLong(r.nodes) << ","
            << naLong(r.attempts) << ","
            << naLong(r.backtracks) << ","
            << naLong(r.pruned) << ","
            << naInt(r.penalty) << ","
            << naDbl(r.seqMs) << ","
            << naDbl(r.parMs) << ","
            << naDbl(r.speedup) << ","
            << naDbl(r.efficiency) << "\n";
    }
    std::cout << "[INFO] Benchmark results saved to: " << csvFilePath << "\n";
}

// Print one row of the unified benchmark table
static void printBenchRow(const RunRow& r)
{
    auto na  = [](long v)  { return v >= 0 ? std::to_string(v) : "N/A"; };
    auto nai = [](int  v)  { return v >= 0 ? std::to_string(v) : "N/A"; };

    std::cout << std::left
              << std::setw(9)  << r.dataset
              << std::setw(10) << r.algo
              << std::setw(9)  << (r.algo == "PBNB" ? std::to_string(r.threads) : "1")
              << std::setw(9)  << r.courses
              << std::setw(10) << r.sessions
              << std::setw(11) << (std::to_string(r.scheduled) + "/" + std::to_string(r.sessions))
              << std::setw(10) << (r.feasible ? "YES" : "NO")
              << std::setw(10) << (r.timedOut ? "YES" : "NO")
              << std::setw(13) << std::fixed << std::setprecision(1) << r.ms
              << std::setw(11) << na(r.nodes)
              << std::setw(11) << na(r.attempts)
              << std::setw(12) << na(r.backtracks)
              << std::setw(12) << na(r.pruned)
              << std::setw(9)  << nai(r.penalty)
              << "\n";
}

// Print Parallel Scaling & Speedup Table
static void printParallelScalingTable(const std::vector<RunRow>& rows)
{
    bool hasParallel = false;
    for (const auto& r : rows) {
        if (r.algo == "PBNB" && r.speedup >= 0.0) {
            hasParallel = true;
            break;
        }
    }
    if (!hasParallel) return;

    const int W = 115;
    std::cout << "\n" << std::string(W, '=') << "\n";
    std::cout << " PARALLEL B&B SPEEDUP & EFFICIENCY COMPARISON\n";
    std::cout << " Formulae: Speedup = Sequential_Runtime_ms / Parallel_Runtime_ms | Efficiency = Speedup / Threads\n";
    std::cout << std::string(W, '=') << "\n";
    std::cout << std::left
              << std::setw(9)  << "Dataset"
              << std::setw(9)  << "Threads"
              << std::setw(18) << "Seq_Time(ms)"
              << std::setw(18) << "Par_Time(ms)"
              << std::setw(12) << "Speedup"
              << std::setw(14) << "Efficiency"
              << std::setw(10) << "Feasible"
              << std::setw(10) << "Penalty"
              << "\n";
    std::cout << std::string(W, '-') << "\n";

    for (const auto& r : rows) {
        if (r.algo != "PBNB" || r.speedup < 0.0) continue;
        std::ostringstream spStr, effStr;
        spStr << std::fixed << std::setprecision(2) << r.speedup << "x";
        effStr << std::fixed << std::setprecision(1) << (r.efficiency * 100.0) << "%";

        std::cout << std::left
                  << std::setw(9)  << r.dataset
                  << std::setw(9)  << r.threads
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.seqMs
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.parMs
                  << std::setw(12) << spStr.str()
                  << std::setw(14) << effStr.str()
                  << std::setw(10) << (r.feasible ? "YES" : "NO")
                  << std::setw(10) << (r.penalty >= 0 ? std::to_string(r.penalty) : "N/A")
                  << "\n";
    }
    std::cout << std::string(W, '=') << "\n\n";
}

// Print complete unified benchmark table from all collected rows
static void printBenchmarkTable(const std::vector<RunRow>& rows,
                                const std::vector<std::string>& sizes,
                                double mrvLimitMs, double bnbLimitMs, double pbnbLimitMs)
{
    const int W = 138;
    std::cout << "\n" << std::string(W, '=') << "\n";
    std::cout << " UNIFIED BENCHMARK — Greedy / MRV / Sequential B&B / Parallel B&B\n";
    std::cout << " Sizes: ";
    for (size_t i = 0; i < sizes.size(); ++i)
        std::cout << sizes[i] << (i+1 < sizes.size() ? ", " : "");
    std::cout << "\n";
    std::cout << " MRV limit: " << (int)(mrvLimitMs/1000) << " s"
              << " | Seq B&B limit: " << (int)(bnbLimitMs/1000) << " s"
              << " | Parallel B&B limit: " << (int)(pbnbLimitMs/1000) << " s"
              << " | Note: TimedOut indicates search reached time limit\n";
    std::cout << std::string(W, '=') << "\n";

    // Header
    std::cout << std::left
              << std::setw(9)  << "Dataset"
              << std::setw(10) << "Algorithm"
              << std::setw(9)  << "Threads"
              << std::setw(9)  << "Courses"
              << std::setw(10) << "Sessions"
              << std::setw(11) << "Scheduled"
              << std::setw(10) << "Feasible"
              << std::setw(10) << "TimedOut"
              << std::setw(13) << "Runtime_ms"
              << std::setw(11) << "Nodes"
              << std::setw(11) << "Attempts"
              << std::setw(12) << "Backtracks"
              << std::setw(12) << "Pruned"
              << std::setw(9)  << "Penalty"
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

    std::cout << std::string(W, '=') << "\n";

    printParallelScalingTable(rows);
}

// Run the full unified benchmark
static void runBenchmark(const std::vector<std::string>& sizes,
                         const std::string& csvPath,
                         double mrvLimitMs,
                         double bnbLimitMs,
                         double pbnbLimitMs,
                         int threads,
                         const std::string& exportCsvPath)
{
    std::vector<RunRow> rows;
    rows.reserve(sizes.size() * 4);

    for (const auto& sz : sizes) {
        std::cout << "\n[BENCH] ── " << sz << " ──────────────────────────\n";

        // ── 1. Greedy ─────────────────────────────────────────────
        std::cout << "[BENCH] " << sz << " GREEDY ...\n";
        rows.push_back(runGreedyOne(sz, csvPath, /*printTable=*/false, /*quiet=*/true));
        const RunRow& gr = rows.back();
        std::cout << "        scheduled=" << gr.scheduled << "/" << gr.sessions
                  << "  feasible=" << (gr.feasible ? "YES" : "NO")
                  << "  penalty=" << (gr.penalty >= 0 ? std::to_string(gr.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << gr.ms << " ms\n";

        // ── 2. MRV ────────────────────────────────────────────────
        std::cout << "[BENCH] " << sz << " MRV (limit " << (int)(mrvLimitMs/1000) << " s) ...\n";
        rows.push_back(runMRVOne(sz, csvPath, /*printTable=*/false, mrvLimitMs, /*quiet=*/true));
        const RunRow& mr = rows.back();
        std::cout << "        scheduled=" << mr.scheduled << "/" << mr.sessions
                  << "  feasible=" << (mr.feasible ? "YES" : "NO")
                  << (mr.timedOut ? " (timed out)" : "")
                  << "  penalty=" << (mr.penalty >= 0 ? std::to_string(mr.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << mr.ms << " ms\n";

        // ── 3. Sequential B&B ─────────────────────────────────────
        std::cout << "[BENCH] " << sz << " Sequential B&B (limit " << (int)(bnbLimitMs/1000) << " s) ...\n";
        RunRow br = runBnBOne(sz, csvPath, /*printTable=*/false, bnbLimitMs, /*quiet=*/true);
        rows.push_back(br);
        std::cout << "        scheduled=" << br.scheduled << "/" << br.sessions
                  << "  feasible=" << (br.feasible ? "YES" : "NO")
                  << (br.timedOut ? " (timed out)" : "")
                  << "  bestPenalty=" << (br.penalty >= 0 ? std::to_string(br.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << br.ms << " ms\n";

        // ── 4. Parallel B&B (OpenMP) ──────────────────────────────
        std::cout << "[BENCH] " << sz << " Parallel B&B (limit " << (int)(pbnbLimitMs/1000)
                  << " s, threads=" << (threads > 0 ? std::to_string(threads) : "auto") << ") ...\n";
        RunRow pbr = runPBnBOne(sz, csvPath, /*printTable=*/false, pbnbLimitMs, threads, /*quiet=*/true);

        if (br.ms > 0.0 && pbr.ms > 0.0) {
            pbr.seqMs = br.ms;
            pbr.parMs = pbr.ms;
            pbr.speedup = br.ms / pbr.ms;
            int effThreads = pbr.threads > 0 ? pbr.threads : (threads > 0 ? threads : 1);
            pbr.efficiency = pbr.speedup / effThreads;
        }
        rows.push_back(pbr);
        std::cout << "        scheduled=" << pbr.scheduled << "/" << pbr.sessions
                  << "  feasible=" << (pbr.feasible ? "YES" : "NO")
                  << (pbr.timedOut ? " (timed out)" : "")
                  << "  bestPenalty=" << (pbr.penalty >= 0 ? std::to_string(pbr.penalty) : "N/A")
                  << "  time=" << std::fixed << std::setprecision(1) << pbr.ms << " ms";
        if (pbr.speedup >= 0.0) {
            std::cout << "  speedup=" << std::fixed << std::setprecision(2) << pbr.speedup << "x"
                      << "  efficiency=" << std::fixed << std::setprecision(1) << (pbr.efficiency * 100.0) << "%";
        }
        std::cout << "\n";
    }

    printBenchmarkTable(rows, sizes, mrvLimitMs, bnbLimitMs, pbnbLimitMs);
    writeBenchmarkCSV(exportCsvPath, rows);
}

// ── Usage ─────────────────────────────────────────────────────────
static void printUsage(const char* prog)
{
    std::cerr << "Usage (single algorithm):\n"
              << "  " << prog
              << " <SMALL|MEDIUM|LARGE|XLARGE|ALL>"
              << " [--algo greedy|mrv|bnb|pbnb]"
              << " [--time-limit <ms>] [--threads <n>] [--csv <file>] [--export-json <file>]\n"
              << "  Default algo  : mrv\n"
              << "  Default limit : 60000 ms per dataset size\n\n"
              << "Usage (unified benchmark):\n"
              << "  " << prog << " --benchmark [SMALL] [MEDIUM] [LARGE] [XLARGE]\n"
              << "  (no sizes = run all four sizes)\n"
              << "  [--threads <n>]     default 4\n"
              << "  [--time-limit <ms>] set limit for all algorithms\n"
              << "  [--mrv-limit <ms>]  default 60000\n"
              << "  [--bnb-limit <ms>]  default 30000\n"
              << "  [--pbnb-limit <ms>] default 30000\n"
              << "  [--csv <file>]      default benchmark_results.csv\n";
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
        double mrvLimitMs  = 60000.0;
        double bnbLimitMs  = 30000.0;
        double pbnbLimitMs = 30000.0;
        int    threads     = 4; // default benchmark threads
        std::string exportCsv = "benchmark_results.csv";

        for (int i = 2; i < argc; ++i) {
            std::string a = toUpper(argv[i]);
            if (a == "SMALL" || a == "MEDIUM" || a == "LARGE" || a == "XLARGE") {
                benchSizes.push_back(a);
            } else if (a == "--THREADS" && i + 1 < argc) {
                try { threads = std::stoi(argv[++i]); } catch (...) {}
            } else if (a == "--TIME-LIMIT" && i + 1 < argc) {
                try {
                    double lim = std::stod(argv[++i]);
                    mrvLimitMs  = lim;
                    bnbLimitMs  = lim;
                    pbnbLimitMs = lim;
                } catch (...) {}
            } else if (a == "--MRV-LIMIT" && i + 1 < argc) {
                try { mrvLimitMs = std::stod(argv[++i]); } catch (...) {}
            } else if (a == "--BNB-LIMIT" && i + 1 < argc) {
                try { bnbLimitMs = std::stod(argv[++i]); } catch (...) {}
            } else if (a == "--PBNB-LIMIT" && i + 1 < argc) {
                try { pbnbLimitMs = std::stod(argv[++i]); } catch (...) {}
            } else if (a == "--CSV" && i + 1 < argc) {
                exportCsv = argv[++i];
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
        std::cout << "[BENCH] Seq B&B lim : " << bnbLimitMs << " ms\n";
        std::cout << "[BENCH] Par B&B lim : " << pbnbLimitMs << " ms\n";
        std::cout << "[BENCH] Threads     : " << (threads > 0 ? std::to_string(threads) : "auto") << "\n";
        std::cout << "[BENCH] Export CSV  : " << exportCsv << "\n";

        runBenchmark(benchSizes, csvPath, mrvLimitMs, bnbLimitMs, pbnbLimitMs, threads, exportCsv);
        return 0;
    }

    // ── Single-algorithm mode (original behaviour) ────────────────
    std::string sizeArg       = toUpper(argv[1]);
    std::string algo          = "MRV";       // default
    double      limitMs       = 60000.0;     // 60 s per size
    int         threads       = 0;           // 0 = OMP default
    std::string csvExportPath  = "";
    std::string jsonExportPath = "";

    // Parse optional flags
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--algo" && i + 1 < argc) {
            algo = toUpper(argv[++i]);
        } else if (flag == "--time-limit" && i + 1 < argc) {
            try { limitMs = std::stod(argv[++i]); } catch (...) {}
        } else if (flag == "--threads" && i + 1 < argc) {
            try { threads = std::stoi(argv[++i]); } catch (...) {}
        } else if (flag == "--csv" && i + 1 < argc) {
            csvExportPath = argv[++i];
        } else if (flag == "--export-json" && i + 1 < argc) {
            jsonExportPath = argv[++i];
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

    if (!csvExportPath.empty()) {
        writeBenchmarkCSV(csvExportPath, rows);
    }
    if (!jsonExportPath.empty() && !rows.empty()) {
        writeRunJSON(jsonExportPath, rows.back());
    }

    return 0;
}
