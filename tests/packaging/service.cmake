function(check_service SERVICE_FILE EXECUTABLE)
    if(NOT EXISTS "${SERVICE_FILE}")
        message(FATAL_ERROR "Missing installed service: ${SERVICE_FILE}")
    endif()
    string(REPLACE "\\" "\\\\" EXPECTED_EXECUTABLE "${EXECUTABLE}")
    string(REPLACE "\"" "\\\"" EXPECTED_EXECUTABLE "${EXPECTED_EXECUTABLE}")
    string(REPLACE "%" "%%" EXPECTED_EXECUTABLE "${EXPECTED_EXECUTABLE}")
    string(REPLACE "\t" "\\t" EXPECTED_EXECUTABLE "${EXPECTED_EXECUTABLE}")
    string(REPLACE "\n" "\\n" EXPECTED_EXECUTABLE "${EXPECTED_EXECUTABLE}")
    string(REPLACE "\r" "\\r" EXPECTED_EXECUTABLE "${EXPECTED_EXECUTABLE}")
    file(READ "${SERVICE_FILE}" SERVICE_CONTENTS)
    string(
        FIND "${SERVICE_CONTENTS}"
        "\nExecStart=\"${EXPECTED_EXECUTABLE}\" transfer watch\n"
        EXEC_START_POSITION
    )
    if(EXEC_START_POSITION EQUAL -1)
        message(FATAL_ERROR "Service does not launch ${EXECUTABLE}:\n${SERVICE_CONTENTS}")
    endif()
endfunction()
