execute_process(COMMAND "${PROGRAM}" ${ARGS} RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status STREQUAL "1" OR NOT diagnostic MATCHES "${EXPECTED}")
  message(FATAL_ERROR "Unexpected exit ${status}, stderr: ${diagnostic}")
endif()
