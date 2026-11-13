# Fuzz targets. Each target also builds as a corpus replay test that runs in
# every build, so the seed corpora double as regression inputs. With
# OMACALENDAR_BUILD_FUZZERS=ON (Clang), libFuzzer binaries are built too:
#   build/tests/fuzz/omacalendar-fuzz-icalendar tests/fuzz/corpus/icalendar
function(omacalendar_add_fuzzer name source)
  add_executable(omacalendar-fuzz-${name}-replay ${source} replay_main.cpp)
  target_link_libraries(omacalendar-fuzz-${name}-replay PRIVATE ${ARGN})
  add_test(NAME fuzz_corpus_${name}
    COMMAND omacalendar-fuzz-${name}-replay ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}
  )
  set_tests_properties(fuzz_corpus_${name} PROPERTIES LABELS fuzz TIMEOUT 60)
  if(OMACALENDAR_BUILD_FUZZERS)
    add_executable(omacalendar-fuzz-${name} ${source})
    target_link_libraries(omacalendar-fuzz-${name} PRIVATE ${ARGN})
    target_link_options(omacalendar-fuzz-${name} PRIVATE -fsanitize=fuzzer)
  endif()
endfunction()

omacalendar_add_fuzzer(icalendar fuzz_icalendar.cpp omacalendar_sync omacalendar_core)
omacalendar_add_fuzzer(caldav_multistatus fuzz_caldav_multistatus.cpp
  omacalendar_sync omacalendar_core)
omacalendar_add_fuzzer(google_event fuzz_google_event.cpp omacalendar_sync omacalendar_core)
omacalendar_add_fuzzer(ipc_request fuzz_ipc_request.cpp omacalendar_ipc)
omacalendar_add_fuzzer(quickadd fuzz_quickadd.cpp omacalendar_core)
