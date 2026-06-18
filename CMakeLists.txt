cmake_minimum_required(VERSION 3.20)
project(c2sql VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Task 15.4: optional AddressSanitizer + UndefinedBehaviorSanitizer build.
# Enable with: cmake -B build-asan -DC2SQL_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan
option(C2SQL_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(C2SQL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# SQLite3 >= 3.35.0 required (UPSERT + STRICT table support)
find_package(SQLite3 REQUIRED)
if(SQLite3_VERSION VERSION_LESS "3.35.0")
    message(FATAL_ERROR
        "SQLite3 >= 3.35.0 is required (found ${SQLite3_VERSION}). "
        "Upgrade your libsqlite3-dev / sqlite-devel package.")
endif()

# Optional PostgreSQL (libpq) backend. Off by default so the standard build
# needs only SQLite3. Enable with -DC2SQL_WITH_POSTGRES=ON (requires libpq-dev).
option(C2SQL_WITH_POSTGRES "Build the PostgreSQL (libpq) driver" OFF)
if(C2SQL_WITH_POSTGRES)
    find_package(PostgreSQL REQUIRED)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples)

# Task 15.4: static-analysis convenience targets (clang-tidy + cppcheck).
# These are no-ops when the tool is not installed. Run with:
#   cmake --build build --target tidy
#   cmake --build build --target cppcheck
#
# The shared source list analyzes all translation units. The PostgreSQL driver
# needs libpq headers, so it is only analyzed when C2SQL_WITH_POSTGRES=ON
# (with -DHAVE_POSTGRES and the libpq include dir added to the tool invocation).
set(C2SQL_ANALYSIS_SOURCES
    ${CMAKE_SOURCE_DIR}/src/c2sql.c
    ${CMAKE_SOURCE_DIR}/src/condition_ast.c
    ${CMAKE_SOURCE_DIR}/src/sqlite_driver.c
    ${CMAKE_SOURCE_DIR}/src/mutex.c
    ${CMAKE_SOURCE_DIR}/src/error_ctx.c
    ${CMAKE_SOURCE_DIR}/src/logger.c
    ${CMAKE_SOURCE_DIR}/src/schema_registry.c
    ${CMAKE_SOURCE_DIR}/src/query_builder.c
    ${CMAKE_SOURCE_DIR}/src/stmt_cache.c
    ${CMAKE_SOURCE_DIR}/src/type_mapping.c
    ${CMAKE_SOURCE_DIR}/src/migration.c)

set(C2SQL_ANALYSIS_DEFS)
set(C2SQL_ANALYSIS_PG_INC)
if(C2SQL_WITH_POSTGRES)
    list(APPEND C2SQL_ANALYSIS_SOURCES ${CMAKE_SOURCE_DIR}/src/postgres_driver.c)
    set(C2SQL_ANALYSIS_DEFS -DHAVE_POSTGRES=1)
    foreach(d ${PostgreSQL_INCLUDE_DIRS})
        list(APPEND C2SQL_ANALYSIS_PG_INC -I${d})
    endforeach()
endif()

# clang-tidy reads checks/options from the repository .clang-tidy file
# (WarningsAsErrors: '*' makes any finding fail the target).
find_program(CLANG_TIDY_EXE clang-tidy)
if(CLANG_TIDY_EXE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE}
            --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            ${C2SQL_ANALYSIS_SOURCES}
            --
            -std=c11
            -I${CMAKE_SOURCE_DIR}/include
            -I${CMAKE_SOURCE_DIR}/src
            ${C2SQL_ANALYSIS_PG_INC}
            ${C2SQL_ANALYSIS_DEFS}
        COMMENT "Running clang-tidy (config: .clang-tidy)")
endif()

find_program(CPPCHECK_EXE cppcheck)
if(CPPCHECK_EXE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXE}
            --enable=warning,style,performance,portability
            --std=c11
            --error-exitcode=1
            --inline-suppr
            --suppress=missingIncludeSystem
            --suppress=unmatchedSuppression
            # cppcheck does not parse the system <sqlite3.h>, so define the
            # version macro to satisfy the in-source minimum-version guard.
            -DSQLITE_VERSION_NUMBER=3045000
            ${C2SQL_ANALYSIS_DEFS}
            -I ${CMAKE_SOURCE_DIR}/include
            -I ${CMAKE_SOURCE_DIR}/src
            ${C2SQL_ANALYSIS_PG_INC}
            ${C2SQL_ANALYSIS_SOURCES}
        COMMENT "Running cppcheck (severity >= warning)")
endif()
