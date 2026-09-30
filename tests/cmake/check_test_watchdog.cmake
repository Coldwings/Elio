if(NOT DEFINED ELIO_WATCHDOG_PROBE)
    message(FATAL_ERROR "ELIO_WATCHDOG_PROBE is required")
endif()

function(check_watchdog mode seconds disabled expected_exit expect_marker)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "ELIO_TEST_WATCHDOG_SECS=${seconds}"
            "ELIO_TEST_WATCHDOG_DISABLE=${disabled}"
            "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1"
            "TSAN_OPTIONS=halt_on_error=1:report_signal_unsafe=1"
            "${ELIO_WATCHDOG_PROBE}" "${mode}"
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10)
    set(report "${output}${error}")
    if(NOT "${actual_exit}" MATCHES "^(${expected_exit})$")
        message(FATAL_ERROR "Watchdog ${mode}: expected exit ${expected_exit}, got ${actual_exit}\n${report}")
    endif()
    if(report MATCHES "(WARNING: ThreadSanitizer|ERROR: AddressSanitizer|SUMMARY: .*Sanitizer)")
        message(FATAL_ERROR "Watchdog ${mode} produced a sanitizer diagnostic:\n${report}")
    endif()
    if(expect_marker AND NOT report MATCHES "elio_tests watchdog: wall-clock timeout reached")
        message(FATAL_ERROR "Watchdog ${mode} did not emit its timeout marker:\n${report}")
    elseif(NOT expect_marker AND report MATCHES "elio_tests watchdog:")
        message(FATAL_ERROR "Disabled watchdog emitted a timeout marker:\n${report}")
    endif()
endfunction()

# Sanitizer signal interposition can reach the _exit fallback even with SIGABRT
# unmasked. Both the observed abort and fallback are intentional timeout exits.
set(alarm_exit 86)
if(ELIO_WATCHDOG_VARIANT STREQUAL "tsan")
    set(alarm_exit "86|124")
endif()
check_watchdog(wait 1 0 "${alarm_exit}" TRUE)
check_watchdog(fallback 1 0 124 TRUE)
check_watchdog(disabled 0 0 0 FALSE)
check_watchdog(disabled 1 1 0 FALSE)
