cmake_minimum_required(VERSION 3.16)
project(TimetableSolver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP REQUIRED)

add_executable(timetable_solver
    main.cpp
    timetable.cpp
    constraint_checker.cpp
    greedy.cpp
    mrv.cpp
    bnb.cpp
    bnb_parallel.cpp
)

target_include_directories(timetable_solver PRIVATE .)

target_link_libraries(timetable_solver PRIVATE OpenMP::OpenMP_CXX)



target_compile_options(timetable_solver PRIVATE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/W4 /O2,-Wall -Wextra -O2>
)
