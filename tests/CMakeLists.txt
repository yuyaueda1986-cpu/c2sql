include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)

# Task 1.2: Public header type/macro compilation test
add_executable(test_header test_header.c)
# Note: test_header only verifies the public header; it does NOT link against
# the library so it can run even before all implementations are complete.
add_test(NAME header_types COMMAND test_header)

# Task 2: Driver interface and SQLite3 implementation tests
add_executable(test_driver test_driver.c)
target_link_libraries(test_driver PRIVATE c2sql)
add_test(NAME driver_tests COMMAND test_driver)

# Task 3: Mutex / Error Context / Logger utility tests
add_executable(test_utils test_utils.c)
target_link_libraries(test_utils PRIVATE c2sql)
add_test(NAME utils_tests COMMAND test_utils)

# Task 4: Schema registry tests
add_executable(test_schema_register test_schema_register.c)
target_link_libraries(test_schema_register PRIVATE c2sql)
add_test(NAME schema_register_tests COMMAND test_schema_register)

# Task 5: Query builder tests
add_executable(test_query_builder test_query_builder.c)
target_link_libraries(test_query_builder PRIVATE c2sql)
add_test(NAME query_builder_tests COMMAND test_query_builder)

# Task 6: Condition AST tests
add_executable(test_condition_ast test_condition_ast.c)
target_link_libraries(test_condition_ast PRIVATE c2sql)
add_test(NAME condition_ast_tests COMMAND test_condition_ast)

# Task 7: Statement cache tests
add_executable(test_stmt_cache test_stmt_cache.c)
target_link_libraries(test_stmt_cache PRIVATE c2sql)
add_test(NAME stmt_cache_tests COMMAND test_stmt_cache)

# Task 8: Type mapping / NULL bitmap tests
add_executable(test_type_mapping test_type_mapping.c)
target_link_libraries(test_type_mapping PRIVATE c2sql)
add_test(NAME type_mapping_tests COMMAND test_type_mapping)

# Task 9: Handle lifecycle / SetConfig
add_executable(test_handle_lifecycle test_handle_lifecycle.c)
target_link_libraries(test_handle_lifecycle PRIVATE c2sql)
add_test(NAME handle_lifecycle_tests COMMAND test_handle_lifecycle)

# Task 10: CRUD public API (RegisterStruct, Write/WriteMany, Read/ReadMany, Delete)
add_executable(test_crud test_crud.c)
target_link_libraries(test_crud PRIVATE c2sql)
add_test(NAME crud_tests COMMAND test_crud)

# Task 11: Transaction control (BeginTx/CommitTx/RollbackTx, SAVEPOINT nesting)
add_executable(test_transaction test_transaction.c)
target_link_libraries(test_transaction PRIVATE c2sql)
add_test(NAME transaction_tests COMMAND test_transaction)

# Task 12: Variable-length BLOB field auxiliary APIs
add_executable(test_blob_field test_blob_field.c)
target_link_libraries(test_blob_field PRIVATE c2sql)
add_test(NAME blob_field_tests COMMAND test_blob_field)

# Task 13: Schema match / auto-migration
add_executable(test_migration test_migration.c)
target_link_libraries(test_migration PRIVATE c2sql)
add_test(NAME migration_tests COMMAND test_migration)

# Task 15.3: Concurrency, multi-handle, perf smoke
add_executable(test_concurrency test_concurrency.c)
target_link_libraries(test_concurrency PRIVATE c2sql)
if(NOT WIN32)
    target_link_libraries(test_concurrency PRIVATE pthread)
endif()
add_test(NAME concurrency_tests COMMAND test_concurrency)
set_tests_properties(concurrency_tests PROPERTIES TIMEOUT 60)

# Phase 2: SqlRDBCount + generated capacity guard, via the generated "sessions"
# schema. Guarded by EXISTS so a checkout without generated artifacts still
# configures (run tools/build_batch.sh to regenerate them).
if(EXISTS ${CMAKE_SOURCE_DIR}/generated/sessions_schema.c)
    add_executable(test_capacity test_capacity.c
        ${CMAKE_SOURCE_DIR}/generated/sessions_schema.c)
    target_include_directories(test_capacity PRIVATE ${CMAKE_SOURCE_DIR}/generated)
    target_link_libraries(test_capacity PRIVATE c2sql)
    add_test(NAME capacity_tests COMMAND test_capacity)
endif()

# Phase 2: c2sql-gen toolchain self-tests + golden-file drift check.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME tools_pytests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_tools.py)
endif()
