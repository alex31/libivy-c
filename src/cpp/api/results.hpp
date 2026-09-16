/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file results.hpp
 * @brief Error codes, send reports and result conventions.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 *
 * @section cpp_results Checking results
 * Fallible operations return `std::expected<..., std::error_code>`, except
 * send_report(), which returns ivy::SendReport. Check `if (!result)` before
 * dereferencing an expected; inspect `result.error()` only on failure. For a
 * report, inspect its `error`, `system_error`, `matched`, `accepted` and `failed`.
 * Compare errors with ivy::make_error_code(IVY_EINVAL), for example.
 *
 * Runtime operations are noexcept and translate failures into result values.
 * Allocation failures use IVY_ENOMEM; invalid input, formatting or length uses
 * IVY_EINVAL. Other user callback/formatter failures use ivy::Error. Callbacks
 * that fail request a bus stop; collect the first error with
 * ivy::Bus::take_callback_error() after servicing and joining the loop.
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
namespace ivy {

/** @brief Result of converting one message for a bind_convert callback. */
enum class ConvertStatus {
    OK,            ///< Every capture was converted successfully.
    COUNT_ERROR,   ///< The capture count differs from the callback's value parameter count.
    CONVERT_ERROR  ///< A capture cannot be converted to its expected type.
};

/**
 * @brief C++-specific errors in the "ivy-cpp" category.
 *
 * Allocation/format/length errors retain the corresponding IvyStatus codes.
 */
enum class Error {
    callback_failed = 1, ///< User callback construction or invocation failed.
    formatter_failed = 2 ///< A custom formatter failed for another reason than allocation/format/length.
};

/**
 * @brief Convert a C++ wrapper error to an error code.
 * @param error C++ callback or formatter failure.
 * @return Error code in the "ivy-cpp" category.
 */
[[nodiscard]] std::error_code make_error_code(Error error) noexcept;

/**
 * @brief Convert a C API status to an error code.
 * @param status Ivy status, including IVY_OK.
 * @return Error code in the "ivy" category; IVY_OK tests false.
 */
[[nodiscard]] std::error_code make_error_code(IvyStatus status) noexcept;

/**
 * @brief Detailed result of one broadcast through Bus::send_report().
 *
 * A peer with several matching subscriptions counts several times.
 * `matched == accepted + failed`; errors before matching leave all counts zero.
 * An error may accompany nonzero accepted; those frames are not rolled back.
 */
struct SendReport {
    std::size_t matched = 0; ///< Number of matching remote subscriptions.
    std::size_t accepted = 0; ///< Complete frames written or queued locally; not delivery acknowledgements.
    std::size_t failed = 0; ///< Matching frames that could not be accepted locally.
    std::error_code error; ///< First failure, or a false-testing code on success.
    std::error_code system_error; ///< OS error associated with the first failure, or zero if none is available.
};

} // namespace ivy
// IVY_CPP_API_END

#endif
