/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once

// Implementation of the public header's templates and compile-time checks.
// Applications only need to include ivy.hpp.
#include "ivy.hpp"

#include <array>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110L
#error "The Ivy C++ wrapper requires C++23 and std::move_only_function support."
#endif

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "The Ivy C++ wrapper requires std::expected support."
#endif

namespace ivy {

namespace detail {
template<std::size_t N>
constexpr std::string_view regexp_view(const char (&text)[N]) noexcept {
    return {text, N && text[N - 1] == '\0' ? N - 1 : N};
}
constexpr std::string_view regexp_view(std::string_view text) noexcept { return text; }

// Non-constexpr diagnostic functions: invalid inputs fail constant evaluation.
void regexp_must_start_with_anchor();
void regexp_must_not_contain_nul();

consteval void require_anchor(std::string_view text) {
    if (!text.starts_with('^'))
        regexp_must_start_with_anchor();
    if (text.find('\0') != std::string_view::npos)
        regexp_must_not_contain_nul();
}

template<class... Args>
class AnchoredFormat {
public:
    template<class T> requires std::convertible_to<const T&, std::string_view>
    consteval AnchoredFormat(const T& text) noexcept : format_(text) {
        require_anchor(regexp_view(text));
    }
    constexpr std::format_string<Args...> get() const noexcept { return format_; }
private:
    std::format_string<Args...> format_;
};

// All fallible work, including callback boxing, stays inside this boundary.
template<class Result>
Result failure(std::error_code error) noexcept {
    if constexpr (std::same_as<Result, SendReport>) {
        SendReport report;
        report.error = error;
        return report;
    } else {
        return std::unexpected(error);
    }
}

template<class Result, class Function>
Result guard(std::error_code fallback, Function&& function) noexcept {
    try {
        return std::forward<Function>(function)();
    } catch (const std::bad_alloc&) {
        return failure<Result>(make_error_code(IVY_ENOMEM));
    } catch (const std::format_error&) {
        return failure<Result>(make_error_code(IVY_EINVAL));
    } catch (const std::length_error&) {
        return failure<Result>(make_error_code(IVY_EINVAL));
    } catch (...) {
        return failure<Result>(fallback);
    }
}
} // namespace detail

template<class T> requires std::convertible_to<const T&, std::string_view>
consteval AnchoredRegexp::AnchoredRegexp(const T& text) noexcept : text_(detail::regexp_view(text)) {
    detail::require_anchor(text_);
}

constexpr std::string_view AnchoredRegexp::get() const noexcept { return text_; }

constexpr Every every(std::chrono::milliseconds period) noexcept { return {period}; }
constexpr Every every(std::chrono::milliseconds period, int count) noexcept { return {period, count}; }
constexpr After after(std::chrono::milliseconds delay) noexcept { return {delay}; }

constexpr RuntimeRegexp runtime_regexp(std::string_view text) noexcept { return {text}; }

template<class Application, class Die>
    requires (std::constructible_from<Bus::ApplicationCallback, Application> &&
              std::constructible_from<Bus::DieCallback, Die>)
Bus::CreateResult Bus::create(std::string_view application_name,
    std::optional<std::string_view> ready, Application&& application_callback, Die&& die_callback) noexcept {
    return detail::guard<CreateResult>(make_error_code(Error::callback_failed), [&] {
        return create_impl(application_name, ready,
            ApplicationCallback(std::forward<Application>(application_callback)),
            DieCallback(std::forward<Die>(die_callback)));
    });
}

template<class... Args> requires (sizeof...(Args) > 0)
std::expected<void, std::error_code>
Subscription::change(AnchoredFormat<Args...> format, Args&&... args) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::formatter_failed), [&] {
            return change(runtime_regexp(std::format(format.get(), std::forward<Args>(args)...)));
        });
}

template<class... Args> requires (sizeof...(Args) > 0)
std::expected<void, std::error_code>
Subscription::change_unanchored(std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::formatter_failed), [&] {
            return change_unanchored(std::format(format, std::forward<Args>(args)...));
        });
}

template<class... Args> requires (sizeof...(Args) > 0)
Bus::SendResult Bus::send(std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<SendResult>(make_error_code(Error::formatter_failed), [&] {
        return send(std::format(format, std::forward<Args>(args)...));
    });
}

template<class... Args> requires (sizeof...(Args) > 0)
SendReport Bus::send_report(std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<SendReport>(make_error_code(Error::formatter_failed), [&] {
        return send_report(std::format(format, std::forward<Args>(args)...));
    });
}

template<class... Args> requires (sizeof...(Args) > 0)
std::expected<void, std::error_code>
Bus::send(IvyClientPtr peer, int id, std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::formatter_failed), [&] {
            return send(peer, id, std::format(format, std::forward<Args>(args)...));
        });
}

template<class... Args> requires (sizeof...(Args) > 0)
std::expected<void, std::error_code>
Bus::send_error(IvyClientPtr peer, int id, std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::formatter_failed), [&] {
            return send_error(peer, id, std::format(format, std::forward<Args>(args)...));
        });
}

template<class... Args> requires (sizeof...(Args) > 0)
std::expected<void, std::error_code>
validate_anchored_regexp(std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::formatter_failed), [&] {
            return validate_anchored_regexp(std::format(format, std::forward<Args>(args)...));
        });
}

template<class Callback> requires std::constructible_from<Bus::TransportCallback, Callback>
std::expected<void, std::error_code> Bus::set_transport_error_callback(Callback&& callback) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(Error::callback_failed), [&] {
            return set_transport_error_callback_impl(TransportCallback(std::forward<Callback>(callback)));
        });
}

// Select the regexp/tag overload before checking the callable's signature.
// Constraining each candidate with is_invocable/constructible_from would
// instantiate generic lambda bodies with unrelated event argument types.
template<class Callback>
Bus::BindResult Bus::bind(Callback&& callback, AnchoredRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp.get(), true);
    });
}

template<class Callback>
Bus::BindResult Bus::bind(Callback&& callback, RuntimeRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp.text, true);
    });
}

template<class Callback> requires std::constructible_from<Bus::MessageCallback, Callback>
Bus::BindResult Bus::bind_unanchored(Callback&& callback, std::string_view regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp, false);
    });
}

template<class Callback> requires std::constructible_from<Bus::DirectCallback, Callback>
Bus::DirectBindResult Bus::bind(Callback&& callback) noexcept {
    return detail::guard<DirectBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_direct_impl(DirectCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback, class... Args>
    requires (sizeof...(Args) > 0 && std::constructible_from<Bus::MessageCallback, Callback>)
Bus::BindResult Bus::bind(Callback&& callback, AnchoredFormat<Args...> format, Args&&... args) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::formatter_failed), [&] {
        return bind(std::forward<Callback>(callback),
            runtime_regexp(std::format(format.get(), std::forward<Args>(args)...)));
    });
}

template<class Callback, class... Args>
    requires (sizeof...(Args) > 0 && std::constructible_from<Bus::MessageCallback, Callback>)
Bus::BindResult Bus::bind_unanchored(Callback&& callback,
    std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::formatter_failed), [&] {
        return bind_unanchored(std::forward<Callback>(callback), std::format(format, std::forward<Args>(args)...));
    });
}

template<class Callback>
Bus::EventBindResult Bus::bind(Callback&& callback, PongTag) noexcept {
    return detail::guard<EventBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_pong_impl(PongCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback>
Bus::EventBindResult Bus::bind(Callback&& callback, RemoteBindingsTag) noexcept {
    return detail::guard<EventBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_remote_bindings_impl(RemoteBindingsCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback>
Bus::TimerBindResult Bus::bind(Callback&& callback, Every schedule) noexcept {
    return detail::guard<TimerBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_timer_impl(TimerCallback(std::forward<Callback>(callback)), schedule);
    });
}

template<class Callback>
Bus::TimerBindResult Bus::bind(Callback&& callback, After schedule) noexcept {
    return detail::guard<TimerBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_timer_impl(TimerCallback(std::forward<Callback>(callback)), Every{schedule.delay, 1}, true);
    });
}

template<std::ranges::input_range Filters>
    requires (!std::convertible_to<Filters, std::string_view> &&
              std::convertible_to<std::ranges::range_reference_t<Filters>, std::string_view>)
std::expected<void, std::error_code> Bus::set_filters(Filters&& classes) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(make_error_code(IVY_EINVAL), [&]() -> std::expected<void, std::error_code> {
        std::vector<std::string> words;
        if constexpr (std::ranges::sized_range<Filters>) {
            const auto count = std::ranges::size(classes);
            if (!std::in_range<int>(count)) return std::unexpected(make_error_code(IVY_EINVAL));
            words.reserve(count);
        }
        for (auto&& word : classes) {
            // Own each element before advancing a single-pass range or destroying
            // a temporary string returned by a transform view.
            if (!std::in_range<int>(words.size() + 1)) return std::unexpected(make_error_code(IVY_EINVAL));
            words.emplace_back(std::string_view(std::forward<decltype(word)>(word)));
        }
        return set_filters_impl(words);
    });
}

template<class... Filters> requires (std::convertible_to<Filters, std::string_view> && ...)
std::expected<void, std::error_code> Bus::set_filters(Filters&&... classes) noexcept {
    return detail::guard<std::expected<void, std::error_code>>(make_error_code(IVY_EINVAL), [&] {
        const std::array<std::string_view, sizeof...(Filters)> words{
            std::string_view(std::forward<Filters>(classes))...
        };
        return set_filters(std::span<const std::string_view>(words));
    });
}

} // namespace ivy
