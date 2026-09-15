/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file filters.hpp
 * @brief Per-bus filters: replacement, addition, removal, parameter packs and ranges.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 *
 * @section cpp_filters Per-bus message-class filters
 * set_filters() replaces only this bus's list. It accepts individual words,
 * initializer lists or input ranges of strings/string_views. Inputs are copied
 * during the call, including temporary strings produced by a view.
 * @code{.cpp}
 * // With a bus, <iostream> and <vector>, inside a function returning int:
 * if (auto configured = bus.set_filters("TRACK", "STATUS"); !configured) {
 *     std::cerr << configured.error().message() << '\n';
 *     return 1;
 * }
 * std::vector<std::string> classes{"PING", "PONG"};
 * if (auto changed = bus.set_filters(classes); !changed) {
 *     std::cerr << changed.error().message() << '\n';
 *     return 1;
 * }
 * @endcode
 * An empty call/list clears the filters. Each replacement is atomic; failure
 * leaves the previous list intact. Filters apply to future remote subscription
 * advertisements, not to messages received by local regexp callbacks. They do
 * not reprocess subscriptions already known by the bus.
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /**
     * @brief Atomically replace the remote-regexp filters of this bus only.
     * @param classes Message-class words, copied during this call. Empty disables filtering.
     * Words contain ASCII letters, digits, underscore or hyphen and must be nonempty.
     * @return Empty success, IVY_EINVAL for invalid words/count, IVY_ENOMEM on allocation
     * failure, IVY_ESTATE on a moved-from bus or IVY_ESTOPPED after stop.
     * Failure preserves the previous policy. Filters apply to future remote subscription
     * advertisements; they do not reprocess existing subscriptions. Unanchored or general
     * regexps without an extractable class are accepted. Configure before start() to cover
     * all initial advertisements. Rejections generate IvyFilterBind notifications.
     */
    [[nodiscard]] std::expected<void, std::error_code>
    set_filters(std::span<const std::string_view> classes) noexcept;

    /** @brief Replace this bus's filters using a list, e.g. {"TRACK", "STATUS"}.
     * @param classes Message-class words copied during the call.
     * @return Same result as the span overload; an empty list clears the filters.
     */
    [[nodiscard]] std::expected<void, std::error_code>
    set_filters(std::initializer_list<std::string_view> classes) noexcept;

    /** @brief Replace this bus's filters from a collection or a C++23 range.
     * @tparam Filters Input range whose elements convert to std::string_view.
     * @param classes Class words, copied as the range is consumed once. Accepts
     * vectors of string/string_view, arrays, lists and views, including temporary strings.
     * @return Same result as the span overload. Iteration/conversion failures report
     * IVY_EINVAL, allocation failures IVY_ENOMEM; no partial list is installed.
     */
    template<std::ranges::input_range Filters>
        requires (!std::convertible_to<Filters, std::string_view> &&
                  std::convertible_to<std::ranges::range_reference_t<Filters>, std::string_view>)
    [[nodiscard]] std::expected<void, std::error_code> set_filters(Filters&& classes) noexcept;

    /** @brief Replace this bus's filters with any number of class words.
     * @tparam Filters Types convertible to std::string_view.
     * @param classes Class words, e.g. set_filters("TRACK", "STATUS"); copied during the call.
     * @return Same result as the span overload; set_filters() disables filtering.
     */
    template<class... Filters> requires (std::convertible_to<Filters, std::string_view> && ...)
    [[nodiscard]] std::expected<void, std::error_code> set_filters(Filters&&... classes) noexcept;

    /** @brief Add one class to this bus's filters; already present is success.
     * @param word Nonempty class word, copied during the call.
     * @return Same input/lifecycle/allocation errors as set_filters().
     */
    [[nodiscard]] std::expected<void, std::error_code> add_filter(std::string_view word) noexcept;

    /** @brief Remove one class from this bus's filters; absent is success.
     * @param word Nonempty class word consumed during the call.
     * @return Same input/lifecycle/allocation errors as set_filters().
     * Removing the final word disables filtering for this bus.
     */
    [[nodiscard]] std::expected<void, std::error_code> remove_filter(std::string_view word) noexcept;

    /** @brief Disable remote-regexp filtering for this bus by clearing its list.
     * @return Empty success, IVY_ESTATE on a moved-from bus or IVY_ESTOPPED after stop.
     */
    [[nodiscard]] std::expected<void, std::error_code> clear_filters() noexcept;
// IVY_CPP_API_END

#endif
