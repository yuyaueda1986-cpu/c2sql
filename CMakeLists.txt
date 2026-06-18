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

# Task 15.4: static-analysis convenience targets.
# These are no-ops when the tool is not installed (target invocation just fails
# with an unambiguous error). Run with: cmake --build build --target cppcheck
find_program(CPPCHECK_EXE cppcheck)
if(CPPCHECK_EXE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXE}
            --enable=warning,style,performance,portability
            --std=c11
            --error-exitcode=1
            --inline-suppr
            --suppress=missingIncludeSystem
            -I ${CMAKE_SOURCE_DIR}/include
            -I ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src
        COMMENT "Running cppcheck (severity >= warning)")
endif()

find_program(CLANG_TIDY_EXE clang-tidy)
if(CLANG_TIDY_EXE)
    file(GLOB C2SQL_TIDY_SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)
    # Suppression rationale:
    #   bugprone-easily-swappable-parameters: triggers on every multi-arg API,
    #     which is unavoidable for a C library with no overloading.
    #   bugprone-reserved-identifier: _POSIX_C_SOURCE is the POSIX-defined
    #     feature-test macro, the leading underscore is required by spec.
    #   clang-analyzer-security.insecureAPI.{DeprecatedOrUnsafeBufferHandling,
    #     strcpy}: the C11 Annex K *_s variants are not implemented by glibc.
    #     We rely on bounded snprintf/memcpy with explicit length args.
    set(C2SQL_TIDY_CHECKS
        clang-analyzer-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,-clang-analyzer-security.insecureAPI.strcpy,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE}
            -warnings-as-errors=${C2SQL_TIDY_CHECKS}
            -checks=${C2SQL_TIDY_CHECKS}
            ${C2SQL_TIDY_SOURCES}
            --
            -std=c11
            -I${CMAKE_SOURCE_DIR}/include
            -I${CMAKE_SOURCE_DIR}/src
        COMMENT "Running clang-tidy (analyzer + bugprone checks)")
endif()
