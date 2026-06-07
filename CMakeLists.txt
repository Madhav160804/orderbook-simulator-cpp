cmake_minimum_required(VERSION 3.14)
project(OrderBookSim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# ── Dependencies ──────────────────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# absl::flat_hash_map — open-addressing + SIMD metadata probe, used in Orderbook.h
FetchContent_Declare(
        absl
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG        20240116.2
        GIT_SHALLOW    TRUE
)
set(ABSL_PROPAGATE_CXX_STD ON)
FetchContent_MakeAvailable(absl)

find_package(CURL REQUIRED)

# Google Benchmark
FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark testing" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Disable benchmark install" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# ── Executables ───────────────────────────────────────────────────────────

# Functionality tests
add_executable(OrderBookTests tests.cpp)
target_compile_options(OrderBookTests PRIVATE -O3 -march=native -DNDEBUG)
target_link_libraries(OrderBookTests
        absl::flat_hash_map
)

# Google Benchmarks
add_executable(OrderBookBenchmarks benchmarks.cpp)
target_compile_options(OrderBookBenchmarks PRIVATE -O3 -march=native -DNDEBUG)
target_link_libraries(OrderBookBenchmarks
        benchmark::benchmark
        absl::flat_hash_map
)

# Live market data feed (Binance WebSocket)
add_executable(LiveMarketData LiveMarketData.cpp)
target_link_libraries(LiveMarketData
        CURL::libcurl
        nlohmann_json::nlohmann_json
        absl::flat_hash_map
)

# ── Tests ─────────────────────────────────────────────────────────────────

enable_testing()
add_test(NAME OrderBookTests COMMAND OrderBookTests)

# ── Build info ────────────────────────────────────────────────────────────

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# ── clang-format target ───────────────────────────────────────────────────

find_program(CLANG_FORMAT "clang-format")
if (CLANG_FORMAT)
    file(GLOB_RECURSE ALL_SOURCE_FILES *.cpp *.h)
    add_custom_target(
            format
            COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCE_FILES}
            COMMENT "Running clang-format"
    )
endif()