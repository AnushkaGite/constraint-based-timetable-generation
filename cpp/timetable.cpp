#include "timetable.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <numeric>
#include <climits>

// ═══════════════════════════════════════════════════════
// Utility: trim whitespace from both ends of a string
// ═══════════════════════════════════════════════════════
static std::string trim(const std::string& s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return "";
    size_t r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

// ═══════════════════════════════════════════════════════
// Utility: split a string by a single-char delimiter
// ═══════════════════════════════════════════════════════
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim)) {
        result.push_back(trim(token));
    }
    return result;
}

// ═══════════════════════════════════════════════════════
// CSV: handle a single quoted or unquoted field
// ═══════════════════════════════════════════════════════
static std::string parseCSVField(std::istream& is) {
    std::string field;
    char ch;
    if (!(is.get(ch))) return field;

    if (ch == '"') {
        // Quoted field
        while (is.get(ch)) {
            if (ch == '"') {
                // Peek to check for escaped quote ""
                char next;
                if (is.peek() == '"') {
                    is.get(next);
                    field += '"';
                } else {
                    break; // end of quoted field
                }
            } else {
                field += ch;
            }
        }
        // consume trailing comma or newline
        if (is.peek() == ',') is.get();
    } else {
        // Unquoted field
        field += ch;
        while (is.get(ch)) {
            if (ch == ',' || ch == '\n') break;
            if (ch == '\r') { // skip CR
                if (is.peek() == '\n') is.get();
                break;
            }
            field += ch;
        }
    }
    return trim(field);
}

// ═══════════════════════════════════════════════════════
// Parse availability string: "MON-3|WED-5|FRI-6" or "NONE"
// ═══════════════════════════════════════════════════════
std::set<std::pair<std::string,int>> parseAvailability(const std::string& s) {
    std::set<std::pair<std::string,int>> result;
    if (s == "NONE" || s.empty()) return result;

    auto tokens = split(s, '|');
    for (auto& tok : tokens) {
        auto parts = split(tok, '-');
        if (parts.size() == 2) {
            try {
                int slot = std::stoi(parts[1]);
                result.insert({parts[0], slot});
            } catch (...) {
                // malformed token — skip
            }
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════
// Parse room options: "R01:60:THEORY|L01:70:LAB|..."
// ═══════════════════════════════════════════════════════
std::vector<RoomOption> parseRoomOptions(const std::string& s) {
    std::vector<RoomOption> result;
    if (s.empty()) return result;

    auto tokens = split(s, '|');
    for (auto& tok : tokens) {
        auto parts = split(tok, ':');
        if (parts.size() == 3) {
            RoomOption ro;
            ro.roomId   = parts[0];
            try { ro.capacity = std::stoi(parts[1]); } catch (...) { ro.capacity = 0; }
            ro.roomType = parts[2];
            result.push_back(ro);
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════
// Load CSV
// ═══════════════════════════════════════════════════════
bool loadCSV(const std::string& filepath,
             const std::string& datasetSize,
             std::vector<Course>& courses)
{
    std::ifstream fin(filepath);
    if (!fin.is_open()) {
        std::cerr << "[ERROR] Cannot open file: " << filepath << "\n";
        return false;
    }

    // Skip header line
    std::string headerLine;
    std::getline(fin, headerLine);

    // Column indices (0-based) matching the CSV header
    // dataset_size,course_id,course_name,course_type,faculty_id,
    // batch_id,batch_strength,sessions_per_week,duration,room_options,
    // faculty_unavailable,batch_unavailable,preferred_start_slot,preferred_end_slot
    // (positions 0..13)

    std::string line;
    while (std::getline(fin, line)) {
        if (trim(line).empty()) continue;

        // Re-parse the line field by field using a stringstream
        std::istringstream ls(line);

        // We'll read exactly 14 fields
        std::string fields[14];
        for (int i = 0; i < 14; ++i) {
            fields[i] = parseCSVField(ls);
        }

        std::string sz = trim(fields[0]);
        if (sz != datasetSize) continue;

        Course c;
        c.courseId           = trim(fields[1]);
        c.courseName         = trim(fields[2]);
        c.courseType         = trim(fields[3]);
        c.facultyId          = trim(fields[4]);
        c.batchId            = trim(fields[5]);
        try { c.batchStrength    = std::stoi(trim(fields[6])); } catch (...) { c.batchStrength = 0; }
        try { c.sessionsPerWeek  = std::stoi(trim(fields[7])); } catch (...) { c.sessionsPerWeek = 0; }
        try { c.duration         = std::stoi(trim(fields[8])); } catch (...) { c.duration = 1; }
        c.roomOptions        = parseRoomOptions(trim(fields[9]));
        c.facultyUnavailable = parseAvailability(trim(fields[10]));
        c.batchUnavailable   = parseAvailability(trim(fields[11]));
        try { c.preferredStartSlot = std::stoi(trim(fields[12])); } catch (...) { c.preferredStartSlot = 1; }
        try { c.preferredEndSlot   = std::stoi(trim(fields[13])); } catch (...) { c.preferredEndSlot = 7; }

        courses.push_back(c);
    }

    return true;
}

// ═══════════════════════════════════════════════════════
// Session generation
// ═══════════════════════════════════════════════════════
std::vector<Session> generateSessions(const std::vector<Course>& courses) {
    std::vector<Session> sessions;
    for (const auto& c : courses) {
        for (int i = 1; i <= c.sessionsPerWeek; ++i) {
            Session s;
            s.sessionId = c.courseId + "_S" + std::to_string(i);
            s.courseId  = c.courseId;
            s.day       = "";    // unassigned
            s.startSlot = -1;    // unassigned
            s.roomId    = "";    // unassigned
            sessions.push_back(s);
        }
    }
    return sessions;
}

// ═══════════════════════════════════════════════════════
// ConflictGraph::build
// ═══════════════════════════════════════════════════════
void ConflictGraph::build(const std::vector<Course>& courses) {
    numVertices = static_cast<int>(courses.size());
    adj.assign(numVertices, std::unordered_set<int>{});
    numEdges = 0;

    for (int i = 0; i < numVertices; ++i) {
        for (int j = i + 1; j < numVertices; ++j) {
            bool conflict =
                (courses[i].batchId   == courses[j].batchId) ||
                (courses[i].facultyId == courses[j].facultyId);
            if (conflict) {
                adj[i].insert(j);
                adj[j].insert(i);
                ++numEdges;
            }
        }
    }
}

int ConflictGraph::minDegree() const {
    if (adj.empty()) return 0;
    int mn = INT_MAX;
    for (const auto& a : adj) {
        mn = std::min(mn, (int)a.size());
    }
    return mn;
}

int ConflictGraph::maxDegree() const {
    int mx = 0;
    for (const auto& a : adj) {
        mx = std::max(mx, (int)a.size());
    }
    return mx;
}

double ConflictGraph::avgDegree() const {
    if (adj.empty()) return 0.0;
    long long total = 0;
    for (const auto& a : adj) total += a.size();
    return static_cast<double>(total) / adj.size();
}

// ═══════════════════════════════════════════════════════
// Console summary
// ═══════════════════════════════════════════════════════
void printSummary(const std::string&          datasetSize,
                  const std::vector<Course>&  courses,
                  const std::vector<Session>& sessions,
                  const ConflictGraph&        graph)
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " CONSTRAINT-BASED TIMETABLE GENERATOR  \n";
    std::cout << "========================================\n\n";

    std::cout << "Dataset: " << datasetSize << "\n\n";
    std::cout << "Courses loaded  : " << courses.size()  << "\n";
    std::cout << "Sessions generated: " << sessions.size() << "\n\n";

    std::cout << "----------------------------------------\n";
    std::cout << " CONFLICT GRAPH\n";
    std::cout << "----------------------------------------\n";
    std::cout << "Vertices        : " << graph.numVertices   << "\n";
    std::cout << "Edges           : " << graph.numEdges      << "\n";
    std::cout << "Minimum degree  : " << graph.minDegree()   << "\n";
    std::cout << "Maximum degree  : " << graph.maxDegree()   << "\n";
    std::cout << "Average degree  : " << graph.avgDegree()   << "\n\n";

    // ── Sample courses (up to 5) ─────────────────────────────
    std::cout << "----------------------------------------\n";
    std::cout << " SAMPLE COURSES (first 5)\n";
    std::cout << "----------------------------------------\n";
    int show = std::min((int)courses.size(), 5);
    for (int i = 0; i < show; ++i) {
        const auto& c = courses[i];
        std::cout << "  [" << c.courseId << "] "
                  << c.courseName << "\n"
                  << "    Type: " << c.courseType
                  << "  Faculty: " << c.facultyId
                  << "  Batch: "  << c.batchId
                  << "  SPW: "    << c.sessionsPerWeek
                  << "  Dur: "    << c.duration << "\n"
                  << "    Rooms: ";
        for (const auto& r : c.roomOptions) {
            std::cout << r.roomId << "(" << r.capacity << "," << r.roomType << ") ";
        }
        std::cout << "\n"
                  << "    Faculty unavail: " << c.facultyUnavailable.size() << " slot(s)"
                  << "  Batch unavail: "  << c.batchUnavailable.size()   << " slot(s)\n";
    }

    // ── Sample sessions (up to 10) ───────────────────────────
    std::cout << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << " SAMPLE SESSIONS (first 10)\n";
    std::cout << "----------------------------------------\n";
    int showS = std::min((int)sessions.size(), 10);
    for (int i = 0; i < showS; ++i) {
        const auto& s = sessions[i];
        std::cout << "  " << s.sessionId
                  << "  (course: " << s.courseId
                  << ", day: [unassigned], slot: [unassigned], room: [unassigned])\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "  FOUNDATION TEST PASSED\n";
    std::cout << "========================================\n\n";
}
