cmake_minimum_required(VERSION 3.14)
project(mini_sqlite CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Library: all frontend + common + storage + engine sources ─────────────────
add_library(mini_sqlite_lib
    src/common/token.cpp
    src/common/eval.cpp
    src/frontend/tokenizer.cpp
    src/frontend/parser.cpp
    src/frontend/catalog.cpp
    src/frontend/sema.cpp
    src/frontend/planner.cpp
    src/storage/pager.cpp
    src/engine/db_engine.cpp
)
target_include_directories(mini_sqlite_lib PUBLIC src)

# ── Main REPL executable ──────────────────────────────────────────────────────
add_executable(mini_sqlite src/main.cpp)
target_link_libraries(mini_sqlite PRIVATE mini_sqlite_lib)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

function(add_test_exe name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE mini_sqlite_lib)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_test_exe(test_tokenizer tests/test_tokenizer.cpp)
add_test_exe(test_parser    tests/test_parser.cpp)
add_test_exe(test_catalog   tests/test_catalog.cpp)
add_test_exe(test_sema      tests/test_sema.cpp)
add_test_exe(test_planner   tests/test_planner.cpp)
add_test_exe(test_storage   tests/test_storage.cpp)
add_executable(test_locking tests/test_locking.cpp)
target_link_libraries(test_locking PRIVATE mini_sqlite_lib)

# integration_runtime_test: links mini_sqlite_lib (which now includes pager.cpp)
add_executable(integration_runtime_test integration_compile_test.cpp)

target_link_libraries(
    integration_runtime_test
    PRIVATE
    mini_sqlite_lib
)

