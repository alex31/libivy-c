/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file regexp.hpp
 * @brief Regexp types, standalone validation and formatting conventions.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 *
 * @section cpp_formatting Text, regexps and formatting
 * Without format arguments, text is unchanged: percent signs and braces are
 * literal. With arguments, std::format syntax applies and the format string is
 * checked at compile time. Double literal braces (`{{` and `}}`) in formatted
 * regexps. Values inserted into regexps are not escaped.
 *
 * Constant regexps passed to bind_raw()/change() must begin with `^` and contain
 * no NUL; invalid constants fail compilation. Use ivy::runtime_regexp(text)
 * for dynamic anchored regexps, or bind_raw_unanchored()/change_unanchored() to
 * allow searching away from the start. Anchored forms validate the final
 * expression after formatting and Ivy interval expansion; `^FOO|BAR` fails
 * because its second alternative is unanchored. This requires PCRE2 support.
 *
 * @see examples/cpp/lifecycle.cpp
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
namespace ivy {

namespace detail {
template<class... Args> class AnchoredFormat;
} // namespace detail
/**
 * @brief Constant regexp checked for a leading ^ and absence of NUL at compile time.
 *
 * Usually supplied implicitly by a string literal in bind_raw() or change().
 */
class AnchoredRegexp {
public:
    /**
     * @brief Check a constant regexp without copying it.
     * @tparam T Constant string type convertible to std::string_view.
     * @param text Regexp text; its storage must outlive this view.
     * @see runtime_regexp()
     */
    template<class T> requires std::convertible_to<const T&, std::string_view>
    consteval AnchoredRegexp(const T& text) noexcept;

    /**
     * @brief Access the constant regexp text.
     * @return Borrowed view of the original text.
     */
    constexpr std::string_view get() const noexcept;
private:
    std::string_view text_;
};

/**
 * @brief Format string checked for a leading ^, absence of NUL and valid format syntax.
 * @tparam Args Types of values inserted into the regexp.
 * @see cpp_formatting
 */
template<class... Args>
using AnchoredFormat = detail::AnchoredFormat<std::type_identity_t<Args>...>;

/// @brief Explicit dynamic regexp that still requires start anchoring.
struct RuntimeRegexp {
    std::string_view text; ///< Borrowed regexp text; validation occurs during bind_raw/change.
};

/**
 * @brief Mark a dynamic regexp for bind_raw() or change().
 * @param text Regexp storage, valid until the bind_raw/change call returns.
 * @return Borrowed view tagged for runtime anchoring validation.
 * No validation or allocation occurs in this helper.
 */
[[nodiscard]] constexpr RuntimeRegexp runtime_regexp(std::string_view text) noexcept;

/**
 * @brief Validate a regexp's start anchoring without creating a subscription.
 * @param expression Regexp text, copied during the call; no NUL is allowed.
 * @return Empty success, IVY_EUNANCHORED for missing/effective unanchoring,
 * IVY_EINVAL for invalid input/syntax/interval expansion, IVY_ENOMEM on allocation
 * failure, or IVY_ESTATE if the C library lacks PCRE2 support.
 * Constants and dynamic expressions both return errors at runtime here. Validation
 * uses the same Ivy interval expansion as bind_raw(), without needing a Bus.
 */
[[nodiscard]] std::expected<void, std::error_code>
validate_anchored_regexp(std::string_view expression) noexcept;

/** @brief Format and validate an anchored regexp without subscribing.
 * @tparam Args Types of the format arguments.
 * @param format Constant std::format string; literal braces must be doubled.
 * @param args Values inserted without escaping regexp syntax.
 * @return Same validation errors as the text overload, plus formatting errors.
 */
template<class... Args> requires (sizeof...(Args) > 0)
[[nodiscard]] std::expected<void, std::error_code>
validate_anchored_regexp(std::format_string<Args...> format, Args&&... args) noexcept;

} // namespace ivy
// IVY_CPP_API_END

#endif
