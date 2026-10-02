cmake_minimum_required(VERSION 3.16)
project(TimetableGenerator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/parser.cpp
    src/conflict_graph.cpp
    src/constraint_checker.cpp
    src/penalty.cpp
    src/greedy.cpp
    src/mrv_greedy.cpp
    src/backtracking.cpp
    src/branch_and_bound.cpp
    src/reporter.cpp
)

add_executable(timetable ${SOURCES})
target_include_directories(timetable PRIVATE src)

# ── Compile flags ─────────────────────────────────────────────────────────────
target_compile_options(timetable PRIVATE -O2 -Wall -Wextra)

# ── OpenMP (required for parallel phases) ─────────────────────────────────────
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(timetable OpenMP::OpenMP_CXX)
    message(STATUS "OpenMP found: ${OpenMP_CXX_VERSION}")
else()
    message(WARNING "OpenMP NOT found — parallel phases will run single-threaded.")
    # Provide stub macros so the source still compiles
    target_compile_definitions(timetable PRIVATE OPENMP_MISSING)
endif()
