# One test binary per component (PROJECT.md 5). add_test registers each with
# ctest so `ctest --output-on-failure` runs the whole suite.
function(cachedb_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE cachedb_core doctest)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

cachedb_add_test(test_command)
cachedb_add_test(test_connection)
cachedb_add_test(test_resp)
cachedb_add_test(test_store)
cachedb_add_test(test_wal)
