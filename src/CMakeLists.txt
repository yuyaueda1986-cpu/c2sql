add_library(c2sql STATIC
    c2sql.c
    condition_ast.c
    sqlite_driver.c
    mutex.c
    error_ctx.c
    logger.c
    schema_registry.c
    query_builder.c
    stmt_cache.c
    type_mapping.c
    migration.c)

target_include_directories(c2sql
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(c2sql PRIVATE SQLite::SQLite3)

# pthread required for mutex on non-Windows
if(NOT WIN32)
    target_link_libraries(c2sql PRIVATE pthread)
endif()

# C-level SQLite3 version guard (catches cases where CMake check is bypassed)
target_compile_definitions(c2sql PRIVATE
    $<$<BOOL:${SQLite3_FOUND}>:HAVE_SQLITE3=1>)
