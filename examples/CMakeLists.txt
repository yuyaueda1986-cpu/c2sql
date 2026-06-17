# Task 14: example programs.
#
# Each example is a stand-alone executable linked against libc2sql. They are
# registered as ctest cases so that a regression that breaks the public-API
# usage patterns is caught the moment `ctest` runs.

set(C2SQL_EXAMPLES
    basic_crud
    transactions
    multi_row)

foreach(example IN LISTS C2SQL_EXAMPLES)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE c2sql)
    add_test(NAME example_${example} COMMAND ${example})
endforeach()

# Code-generation demo (Phase 1): uses tools/c2sql_gen.py output checked into
# generated/. Guarded by EXISTS so a fresh checkout without generated artifacts
# still configures; run tools/build_batch.sh to (re)generate them.
set(C2SQL_GENERATED_CMAKE ${CMAKE_SOURCE_DIR}/generated/persons.cmake)
if(EXISTS ${C2SQL_GENERATED_CMAKE})
    include(${C2SQL_GENERATED_CMAKE})  # defines the persons_schema target
    add_executable(generated_crud generated_crud.c)
    target_link_libraries(generated_crud PRIVATE persons_schema)
    add_test(NAME example_generated_crud COMMAND generated_crud)
    list(APPEND C2SQL_EXAMPLES generated_crud)
endif()

# A convenience target so `make examples` builds them all.
add_custom_target(examples DEPENDS ${C2SQL_EXAMPLES})
