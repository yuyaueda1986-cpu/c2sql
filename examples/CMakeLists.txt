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

# A convenience target so `make examples` builds them all.
add_custom_target(examples DEPENDS ${C2SQL_EXAMPLES})
