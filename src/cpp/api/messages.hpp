/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file messages.hpp
 * @brief Message callbacks and regexp/direct subscriptions on ivy::Bus.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /// @brief Regexp callback: borrowed peer and capture views, valid only during the call.
    using MessageCallback =
        std::move_only_function<void(IvyClientPtr, std::span<const std::string_view>)>;

    /// @brief Direct callback: borrowed peer, message identifier and text valid during the call.
    using DirectCallback = std::move_only_function<void(IvyClientPtr, int, std::string_view)>;

    /// @brief Regexp subscription on success; keep it alive to keep receiving messages.
    using BindResult = std::expected<Subscription, std::error_code>;

    /// @brief Direct subscription on success; keep it alive to keep receiving messages.
    using DirectBindResult = std::expected<DirectSubscription, std::error_code>;

    /**
     * @brief Subscribe to messages matching a constant anchored regexp.
     * @tparam Callback Callable compatible with MessageCallback.
     * @param callback Receives peer and captured groups; the capture views last only for the call.
     * @param regexp Constant regexp beginning with ^; braces and percent signs are literal.
     * @return Owned subscription on success. IVY_EUNANCHORED if the expanded expression is not
     * anchored, IVY_EINVAL for invalid input/syntax, IVY_ENOMEM for allocation failure,
     * IVY_ESTATE for a moved-from bus or unavailable validator, IVY_ESTOPPED for a stopped bus,
     * or Error::callback_failed for callback construction failure.
     * Keep the subscription or its expected alive; an empty callback is invalid.
     * @see cpp_quickstart cpp_formatting
     */
    template<class Callback>
    [[nodiscard]] BindResult bind_raw(Callback&& callback, AnchoredRegexp regexp) noexcept;

    /**
     * @brief Subscribe using a dynamic anchored regexp.
     * @tparam Callback Callable compatible with MessageCallback.
     * @param callback Receives peer and captured groups; the capture views last only for the call.
     * @param regexp Use runtime_regexp(text); text is consumed during this call.
     * @return Owned subscription on success. The same validation, lifecycle and allocation
     * errors as the constant anchored overload.
     * Keep the subscription or its expected alive; an empty callback is invalid.
     * @see cpp_quickstart cpp_formatting
     */
    template<class Callback>
    [[nodiscard]] BindResult bind_raw(Callback&& callback, RuntimeRegexp regexp) noexcept;

    /**
     * @brief Subscribe with automatic conversion of captured groups.
     * @tparam Callback Callable with one explicit, non-overloaded signature returning void.
     * @param callback Takes ConvertStatus first, then one value per capture: only
     * long, double, std::string_view or bool. An optional IvyClientPtr immediately
     * after the status receives the borrowed sender.
     * Generic lambdas, overloaded call operators and reference parameters are rejected.
     * Move-only captures, mutable/noexcept lambdas and function pointers are supported.
     * @param regexp Constant regexp beginning with ^; braces and percent signs are literal.
     * @return Same owned subscription and registration errors as bind_raw().
     *
     * Captures are converted in order before invoking the callback. Integers are
     * decimal; doubles accept decimal/scientific notation and must be finite.
     * An optional leading + or - is allowed for numbers. No whitespace, trailing
     * text, empty numbers or out-of-range values are accepted for long/double.
     * For bool, a complete decimal integer (optional sign, any size) is false if
     * zero, true otherwise. Non-integers starting with f/F are false; t/T/v/V are
     * true. All other text, including empty captures, is a conversion error.
     * No whitespace is trimmed. String views are not copied
     * and are valid only during the callback; an empty string is valid.
     *
     * The number of captures must equal the number of value parameters, including
     * after Subscription::change(). The callback is called on every received
     * message: OK on success, COUNT_ERROR for a count mismatch, CONVERT_ERROR for
     * the first invalid capture. On either error, all value parameters are
     * default-initialized (0, 0.0, empty view, false); the sender is preserved.
     * Inspect conversion_error() during the callback for the exact diagnostic.
     * Conversion errors let the bus continue and do not set take_callback_error().
     * Exceptions thrown by the callback retain bind_raw()'s stop-and-record behavior.
     * Signature errors are diagnosed at compile time;
     * capture count and values are checked on receipt, not during registration.
     * Keep the subscription or its expected alive; an empty callable is invalid.
     * @code{.cpp}
     * auto tracks = bus.bind_convert(
     *     [&bus](ivy::ConvertStatus status, long id, double altitude, std::string_view name, bool active) {
     *         if (status != ivy::ConvertStatus::OK) {
     *             std::cerr << bus.conversion_error() << '\n';
     *             return;
     *         }
     *         // Values are already converted here.
     *     }, R"(^TRACK (\S+) (\S+) (\S+) (\S+)$)");
     * @endcode
     */
    template<class Callback>
    [[nodiscard]] BindResult bind_convert(Callback&& callback, AnchoredRegexp regexp) noexcept;

    /**
     * @brief Convert captures from a dynamic anchored regexp.
     * @tparam Callback Callable with the typed signature of bind_convert().
     * @param callback Same typed signature and conversion rules as bind_convert().
     * @param regexp Use runtime_regexp(text); text is consumed during this call.
     * @return Same subscription and errors as the constant anchored overload.
     */
    template<class Callback>
    [[nodiscard]] BindResult bind_convert(Callback&& callback, RuntimeRegexp regexp) noexcept;

    /**
     * @brief Format an anchored regexp and convert its captured groups.
     * @tparam Callback Callable with the typed signature of bind_convert().
     * @tparam Args Types of the formatting arguments.
     * @param callback Same typed signature and conversion rules as bind_convert().
     * @param format Constant format string beginning with ^; double literal braces.
     * @param args Values inserted without escaping regexp syntax.
     * @return Same subscription and errors as bind_convert(), plus formatting errors.
     * @see @ref cpp_formatting
     */
    template<class Callback, class... Args> requires (sizeof...(Args) > 0)
    [[nodiscard]] BindResult bind_convert(Callback&& callback,
                                         AnchoredFormat<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Describe the current bind_convert callback's conversion error.
     * @return Empty on OK or outside a bind_convert callback for this bus on the
     * calling thread. COUNT_ERROR describes expected/received capture counts;
     * CONVERT_ERROR identifies the first failing capture (1-based), its text,
     * expected type and failure reason.
     * The view is valid until that callback returns. Copy it to retain it.
     * Nested callbacks and callbacks on other threads preserve this invocation's
     * diagnostic. Repeated calls do not clear it. If diagnostic text allocation
     * fails, a fixed fallback message is returned and the callback still runs.
     */
    [[nodiscard]] std::string_view conversion_error() const noexcept;

    /**
     * @brief Subscribe with an explicit unanchored search.
     * @tparam Callback Callable compatible with MessageCallback.
     * @param callback Receives peer and captured groups; the capture views last only for the call.
     * @param regexp Text consumed during the call; no local PCRE2 anchoring validation.
     * @return Owned subscription on success. The same lifecycle/input/allocation/callback
     * errors as bind_raw(), without its anchoring check.
     * Keep the subscription or its expected alive; an empty callback is invalid.
     * @see cpp_quickstart cpp_formatting
     */
    template<class Callback> requires std::constructible_from<MessageCallback, Callback>
    [[nodiscard]] BindResult bind_raw_unanchored(Callback&& callback, std::string_view regexp) noexcept;

    /**
     * @brief Register the bus's single direct-message callback.
     * @tparam Callback Callable compatible with DirectCallback.
     * @param callback Receives peer, identifier and text valid only during the call.
     * @return Owned DirectSubscription on success; IVY_EINVAL for an empty callable,
     * IVY_ESTATE for a moved-from bus, IVY_ESTOPPED for a stopped bus, or an
     * allocation/callback-construction error.
     * Success replaces the previous direct callback and makes its token inactive.
     * Keep the result alive to keep receiving direct messages.
     * @see send(IvyClientPtr,int,std::string_view)
     */
    template<class Callback> requires std::constructible_from<DirectCallback, Callback>
    [[nodiscard]] DirectBindResult bind_direct(Callback&& callback) noexcept;

    /**
     * @brief Format and register an anchored regexp subscription.
     * @tparam Callback Callable compatible with MessageCallback.
     * @tparam Args Types of the formatting arguments.
     * @param callback Receives peer and capture views valid during the call.
     * @param format Constant format string beginning with ^.
     * @param args Values inserted without escaping regexp syntax.
     * @return Same subscription/result as the text overload, plus formatting errors.
     * Double literal regexp braces, e.g. `R"(^TRACK {} ([0-9]{{2}})$)"` with an integer ID.
     * @see cpp_formatting
     */
    template<class Callback, class... Args>
        requires (sizeof...(Args) > 0 && std::constructible_from<MessageCallback, Callback>)
    [[nodiscard]] BindResult bind_raw(Callback&& callback,
                                 AnchoredFormat<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Format and register an unanchored regexp subscription.
     * @tparam Callback Callable compatible with MessageCallback.
     * @tparam Args Types of the formatting arguments.
     * @param callback Receives peer and capture views valid during the call.
     * @param format Constant format string.
     * @param args Values inserted without escaping regexp syntax.
     * @return Same subscription/result as the text overload, plus formatting errors.
     * Double literal regexp braces, e.g. `R"(^TRACK {} ([0-9]{{2}})$)"` with an integer ID.
     * @see cpp_formatting
     */
    template<class Callback, class... Args>
        requires (sizeof...(Args) > 0 && std::constructible_from<MessageCallback, Callback>)
    [[nodiscard]] BindResult bind_raw_unanchored(Callback&& callback,
                                           std::format_string<Args...> format, Args&&... args) noexcept;
// IVY_CPP_API_END

#endif
