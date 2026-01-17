#pragma once

#include "logger.hpp"

/// Logging macros with file and line information

#ifdef ELIO_DEBUG
    #define ELIO_LOG_DEBUG(fmt, ...) \
        ::elio::log::logger::instance().log( \
            ::elio::log::level::debug, \
            __FILE__, __LINE__, \
            fmt __VA_OPT__(,) __VA_ARGS__ \
        )
#else
    #define ELIO_LOG_DEBUG(fmt, ...) ((void)0)
#endif

#define ELIO_LOG_INFO(fmt, ...) \
    ::elio::log::logger::instance().log( \
        ::elio::log::level::info, \
        __FILE__, __LINE__, \
        fmt __VA_OPT__(,) __VA_ARGS__ \
    )

#define ELIO_LOG_WARNING(fmt, ...) \
    ::elio::log::logger::instance().log( \
        ::elio::log::level::warning, \
        __FILE__, __LINE__, \
        fmt __VA_OPT__(,) __VA_ARGS__ \
    )

#define ELIO_LOG_ERROR(fmt, ...) \
    ::elio::log::logger::instance().log( \
        ::elio::log::level::error, \
        __FILE__, __LINE__, \
        fmt __VA_OPT__(,) __VA_ARGS__ \
    )
