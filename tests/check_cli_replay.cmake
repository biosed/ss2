if(NOT DEFINED PROGRAM OR NOT DEFINED GAME_DIR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "PROGRAM, GAME_DIR and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(rep_file "${WORK_DIR}/russian1.ss2rep")
set(bad_file "${WORK_DIR}/russian1_corrupt.ss2rep")

set(extra_args)
if(DEFINED VALIDATION_ARG AND NOT "${VALIDATION_ARG}" STREQUAL "")
  list(APPEND extra_args "${VALIDATION_ARG}")
endif()

execute_process(
  COMMAND "${PROGRAM}" --game-dir "${GAME_DIR}" --campaign russian:1
          --record "${rep_file}" --frames 6 --no-movies ${extra_args}
  RESULT_VARIABLE rec_rc
  OUTPUT_VARIABLE rec_out
  ERROR_VARIABLE rec_err)
if(NOT rec_rc EQUAL 0)
  message(FATAL_ERROR "--record exited with ${rec_rc}:\n${rec_out}\n${rec_err}")
endif()
if(NOT EXISTS "${rep_file}")
  message(FATAL_ERROR "--record did not create ${rep_file}")
endif()
if(NOT "${rec_out}" MATCHES "Recorded [0-9]+ commands through tick [0-9]+")
  message(FATAL_ERROR "--record did not print summary:\n${rec_out}")
endif()

# Replay without --campaign/--mission: mission path, difficulty and auto_engage
# come from the .ss2rep metadata header.
execute_process(
  COMMAND "${PROGRAM}" --game-dir "${GAME_DIR}"
          --replay "${rep_file}" --frames 2 --no-movies ${extra_args}
  RESULT_VARIABLE rep_rc
  OUTPUT_VARIABLE rep_out
  ERROR_VARIABLE rep_err)
if(NOT rep_rc EQUAL 0)
  message(FATAL_ERROR "--replay exited with ${rep_rc}:\n${rep_out}\n${rep_err}")
endif()
if(NOT "${rep_out}" MATCHES "Replay verified: [0-9]+ commands, tick [0-9]+, stateHash 0x[0-9a-f]+")
  message(FATAL_ERROR "--replay did not print verification line:\n${rep_out}")
endif()

# Headless batch --replay without --frames or a Vulkan window/display driver.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=dummy
          "${PROGRAM}" --game-dir "${GAME_DIR}"
          --replay "${rep_file}" --no-movies
  RESULT_VARIABLE hless_rc
  OUTPUT_VARIABLE hless_out
  ERROR_VARIABLE hless_err)
if(NOT hless_rc EQUAL 0)
  message(FATAL_ERROR "Headless --replay exited with ${hless_rc}:\n${hless_out}\n${hless_err}")
endif()
if(NOT "${hless_out}" MATCHES "Replay verified: [0-9]+ commands, tick [0-9]+, stateHash 0x[0-9a-f]+")
  message(FATAL_ERROR "Headless --replay did not print verification line:\n${hless_out}")
endif()

# Flip the final hex digit of # end_state_hash: and require --replay to reject it.
file(READ "${rep_file}" rep_text)
if("${rep_text}" MATCHES "# end_state_hash: 0x[0-9a-f]+0\n")
  string(REGEX REPLACE "(# end_state_hash: 0x[0-9a-f]+)0\n" "\\11\n" bad_text "${rep_text}")
else()
  string(REGEX REPLACE "(# end_state_hash: 0x[0-9a-f]+)[0-9a-f]\n" "\\10\n" bad_text "${rep_text}")
endif()
file(WRITE "${bad_file}" "${bad_text}")

execute_process(
  COMMAND "${PROGRAM}" --game-dir "${GAME_DIR}"
          --replay "${bad_file}" --frames 1 --no-movies
  RESULT_VARIABLE bad_rc
  OUTPUT_VARIABLE bad_out
  ERROR_VARIABLE bad_err)
if(bad_rc EQUAL 0)
  message(FATAL_ERROR "--replay unexpectedly accepted a corrupted end_state_hash:\n${bad_out}\n${bad_err}")
endif()
set(combined_bad "${bad_out}\n${bad_err}")
if(NOT "${combined_bad}" MATCHES "Replay stateHash mismatch")
  message(FATAL_ERROR "Expected 'Replay stateHash mismatch', got:\n${combined_bad}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
