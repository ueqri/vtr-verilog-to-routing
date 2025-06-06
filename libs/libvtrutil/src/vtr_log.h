#pragma once
/**
 * @file
 * @brief This header defines useful logging macros for VTR projects.
 *
 * Message Type
 * ============
 *
 * Three types of log message types are defined:
 *     - VTR_LOG         : The standard 'info' type log message
 *     - VTR_LOG_WARN    : A warning log message. This represents an unusual condition that may indicate an issue but execution continues
 *     - VTR_LOG_ERROR   : An error log message. This represents a clear issue that should result in stopping the program execution.
 *                         Please note that using this log message will not actually terminate the program. So a VtrError should be thrown
 *                         after all the necessary VTR_LOG_ERROR messages are printed.
 * 
 * For example:
 *
 *      VTR_LOG("This produces a regular '%s' message\n", "info");
 *      VTR_LOG_WARN("This produces a '%s' message\n", "warning");
 *      VTR_LOG_ERROR("This produces an '%s' message\n", "error");
 *
 * Conditional Logging
 * ===================
 *
 * Each of the three message types also have a VTR_LOGV_* variant,
 * which will cause the message to be logged if a user-defined condition
 * is satisfied.
 *
 * For example:
 *
 *      VTR_LOGV(verbosity > 5, "This message will be logged only if verbosity is greater than %d\n", 5);
 *      VTR_LOGV_WARN(verbose, "This warning message will be logged if verbose is true\n");
 *      VTR_LOGV_ERROR(false, "This error message will never be logged\n");
 *
 * Custom Location Logging
 * =======================
 *
 * Each of the three message types also have a VTR_LOGF_* variant,
 * which will cause the message to be logged for a custom file and
 *
 * For example:
 *
 *      VTR_LOGF("my_file.txt", "This message will be logged from file 'my_file.txt' line %d\n", 42);
 *  
 * Debug Logging
 * =============
 *
 * For debug purposes it may be useful to have additional logging.
 * This is supported by VTR_LOG_DEBUG() and VTR_LOGV_DEBUG().
 *
 * To avoid run-time overhead, these are only enabled if VTR_ENABLE_DEBUG_LOGGING 
 * is defined (disabled by default).
 */

#include <unordered_set>
#include <string>

// Unconditional logging macros
#define VTR_LOG(...) VTR_LOGV(true, __VA_ARGS__)
#define VTR_LOG_WARN(...) VTR_LOGV_WARN(true, __VA_ARGS__)
#define VTR_LOG_ERROR(...) VTR_LOGV_ERROR(true, __VA_ARGS__)
#define VTR_LOG_NOP(...) VTR_LOGV_NOP(true, __VA_ARGS__)

// Conditional logging macros
#define VTR_LOGV(expr, ...) VTR_LOGVF(expr, __FILE__, __LINE__, __VA_ARGS__)
#define VTR_LOGV_WARN(expr, ...) VTR_LOGVF_WARN(expr, __FILE__, __LINE__, __VA_ARGS__)
#define VTR_LOGV_ERROR(expr, ...) VTR_LOGVF_ERROR(expr, __FILE__, __LINE__, __VA_ARGS__)
#define VTR_LOGV_NOP(expr, ...) VTR_LOGVF_NOP(expr, __FILE__, __LINE__, __VA_ARGS__)

// Custom file-line location logging macros
#define VTR_LOGF(file, line, ...) VTR_LOGVF(true, file, line, __VA_ARGS__)
#define VTR_LOGF_WARN(file, line, ...) VTR_LOGVF_WARN(true, file, line, __VA_ARGS__)
#define VTR_LOGF_ERROR(file, line, ...) VTR_LOGVF_ERROR(true, file, line, __VA_ARGS__)
#define VTR_LOGF_NOP(file, line, ...) VTR_LOGVF_NOP(true, file, line, __VA_ARGS__)

// Custom file-line-func location logging macros
#define VTR_LOGFF_WARN(file, line, func, ...) VTR_LOGVFF_WARN(true, file, line, func, __VA_ARGS__)

// Conditional logging and custom file-line location macros
#define VTR_LOGVF(expr, file, line, ...)    \
    do {                                    \
        if (expr) vtr::printf(__VA_ARGS__); \
    } while (false)

#define VTR_LOGVF_WARN(expr, file, line, ...)                                   \
    do {                                                                        \
        if (expr) print_or_suppress_warning(file, line, __func__, __VA_ARGS__); \
    } while (false)

#define VTR_LOGVF_ERROR(expr, file, line, ...)                \
    do {                                                      \
        if (expr) vtr::printf_error(file, line, __VA_ARGS__); \
    } while (false)

// Conditional logging and custom file-line-func location macros
#define VTR_LOGVFF_WARN(expr, file, line, func, ...)                        \
    do {                                                                    \
        if (expr) print_or_suppress_warning(file, line, func, __VA_ARGS__); \
    } while (false)

/*
 * No-op version of logging macro which avoids unused parameter warnings.
 *
 * Note that to avoid unused parameter warnings we call sizeof() and cast
 * the result to void. sizeof is evaluated at compile time so there is no
 * run-time overhead.
 *
 * Also note the use of std::make_tuple to ensure all arguments in VA_ARGS
 * are used.
 */
#define VTR_LOGVF_NOP(expr, file, line, ...)                     \
    do {                                                         \
        static_cast<void>(sizeof(expr));                         \
        static_cast<void>(sizeof(file));                         \
        static_cast<void>(sizeof(line));                         \
        static_cast<void>(sizeof(std::make_tuple(__VA_ARGS__))); \
    } while (false)

// Debug logging macros
#ifdef VTR_ENABLE_DEBUG_LOGGING //Enable
#define VTR_LOG_DEBUG(...) VTR_LOG(__VA_ARGS__)
#define VTR_LOGV_DEBUG(expr, ...) VTR_LOGV(expr, __VA_ARGS__)
#else //Disable
#define VTR_LOG_DEBUG(...) VTR_LOG_NOP(__VA_ARGS__)
#define VTR_LOGV_DEBUG(expr, ...) VTR_LOGV_NOP(expr, __VA_ARGS__)
#endif

namespace vtr {

typedef void (*PrintHandlerInfo)(const char* pszMessage, ...);
typedef void (*PrintHandlerWarning)(const char* pszFileName, unsigned int lineNum, const char* pszMessage, ...);
typedef void (*PrintHandlerError)(const char* pszFileName, unsigned int lineNum, const char* pszMessage, ...);
typedef void (*PrintHandlerDirect)(const char* pszMessage, ...);

extern PrintHandlerInfo printf; //Same as printf_info
extern PrintHandlerInfo printf_info;
extern PrintHandlerWarning printf_warning;
extern PrintHandlerError printf_error;
extern PrintHandlerDirect printf_direct;

void set_log_file(const char* filename);

} // namespace vtr

static std::unordered_set<std::string> warnings_to_suppress;
static std::string noisy_warn_log_file;

/**
 * @brief The following data structure and functions allow to suppress noisy warnings and direct them into an external file, if specified.
 */
void add_warnings_to_suppress(std::string function_name);

/**
 * @brief This function creates a new log file to hold the suppressed warnings. If the file already exists, it is cleared out first.
 */
void set_noisy_warn_log_file(std::string log_file_name);

/** 
 * @brief This function checks whether to print or to suppress warning
 *
 * This function checks whether the function from which the warning has been called
 *  is in the set of warnings_to_suppress. If so, the warning is printed on the
 * noisy_warn_log_file, otherwise it is printed on stdout (or the regular log file)
 */
void print_or_suppress_warning(const char* pszFileName, unsigned int lineNum, const char* pszFuncName, const char* pszMessage, ...);
