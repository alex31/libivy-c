/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <string>

namespace ivy {

std::expected<void, std::error_code>
Bus::set_filters(std::span<const std::string_view> classes) noexcept {
    if (!std::in_range<int>(classes.size())) return detail::status_result(IVY_EINVAL);
    return detail::guard<std::expected<void, std::error_code>>(make_error_code(IVY_EINVAL), [&] {
        std::vector<std::string> words;
        words.reserve(classes.size());
        for (auto word : classes) words.emplace_back(word);
        return set_filters_impl(words);
    });
}

std::expected<void, std::error_code>
Bus::set_filters_impl(std::span<const std::string> words) noexcept {
    const auto owner = impl_;
    if (!owner) return detail::status_result(IVY_ESTATE);
    if (detail::stopped(owner->context)) return detail::status_result(IVY_ESTOPPED);
    if (!std::in_range<int>(words.size())) return detail::status_result(IVY_EINVAL);
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(IVY_EINVAL), [&]() -> std::expected<void, std::error_code> {
            std::vector<const char*> pointers;
            pointers.reserve(words.size());
            for (const auto& word : words) {
                if (word.find('\0') != std::string::npos) return detail::status_result(IVY_EINVAL);
                pointers.push_back(word.c_str());
            }
            return detail::status_result(IvyContextSetFilter(owner->context,
                static_cast<int>(pointers.size()), pointers.data()));
        });
}

std::expected<void, std::error_code>
Bus::set_filters(std::initializer_list<std::string_view> classes) noexcept {
    return set_filters(std::span<const std::string_view>(classes.begin(), classes.size()));
}

std::expected<void, std::error_code> Bus::add_filter(std::string_view word) noexcept {
    const auto owner = impl_;
    if (!owner) return detail::status_result(IVY_ESTATE);
    if (detail::stopped(owner->context)) return detail::status_result(IVY_ESTOPPED);
    if (word.find('\0') != std::string_view::npos) return detail::status_result(IVY_EINVAL);
    return detail::guard<std::expected<void, std::error_code>>(make_error_code(IVY_EINVAL), [&] {
        const std::string text(word);
        return detail::status_result(IvyContextAddFilter(owner->context, text.c_str()));
    });
}

std::expected<void, std::error_code> Bus::remove_filter(std::string_view word) noexcept {
    const auto owner = impl_;
    if (!owner) return detail::status_result(IVY_ESTATE);
    if (detail::stopped(owner->context)) return detail::status_result(IVY_ESTOPPED);
    if (word.find('\0') != std::string_view::npos) return detail::status_result(IVY_EINVAL);
    return detail::guard<std::expected<void, std::error_code>>(make_error_code(IVY_EINVAL), [&] {
        const std::string text(word);
        return detail::status_result(IvyContextRemoveFilter(owner->context, text.c_str()));
    });
}

std::expected<void, std::error_code> Bus::clear_filters() noexcept {
    if (!impl_) return detail::status_result(IVY_ESTATE);
    return detail::status_result(IvyContextSetFilter(impl_->context, 0, nullptr));
}

} // namespace ivy
