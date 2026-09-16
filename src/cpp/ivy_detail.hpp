/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once

// Implementation of the public header's templates and compile-time checks.
// Applications only need to include ivy.hpp.
#include "ivy.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <new>
#include <stdexcept>
#include <tuple>
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
// Invocation-local diagnostics: owner identity survives Bus moves, and a stack
// preserves outer callbacks' views during nested or concurrent dispatch.
struct ConversionContext {
    const void* owner;
    std::string_view error;
    ConversionContext* previous;
    static thread_local ConversionContext* current;
    ConversionContext(const void* owner, std::string_view error) noexcept;
    ~ConversionContext();
    ConversionContext(const ConversionContext&) = delete;
    ConversionContext& operator=(const ConversionContext&) = delete;
};

template<class T>
inline constexpr bool capture_type = std::same_as<T, long> || std::same_as<T, double> ||
    std::same_as<T, std::string_view> || std::same_as<T, bool>;

template<class... Args>
struct CaptureArguments {
    using Values = std::tuple<Args...>;
    static constexpr bool with_peer = false;
    static constexpr bool valid = (capture_type<Args> && ...);
};

template<class... Args>
struct CaptureArguments<IvyClientPtr, Args...> : CaptureArguments<Args...> {
    static constexpr bool with_peer = true;
    // Check the remaining arguments directly: a second peer is not a capture.
    static constexpr bool valid = (capture_type<Args> && ...);
};

template<class Signature>
struct CaptureSignature {
    static constexpr bool valid = false;
};

template<class R, class... Args>
struct CaptureSignature<R(ConvertStatus, Args...)> : CaptureArguments<Args...> {
    static constexpr bool valid = std::same_as<R, void> && CaptureArguments<Args...>::valid;
};

template<class R, class... Args>
struct CaptureSignature<R(Args...) noexcept> : CaptureSignature<R(Args...)> {};

template<class R, class... Args>
struct CaptureSignature<R(*)(Args...)> : CaptureSignature<R(Args...)> {};

template<class R, class... Args>
struct CaptureSignature<R(*)(Args...) noexcept> : CaptureSignature<R(Args...)> {};

// Callbacks are stored and invoked as lvalues, like MessageCallback.
#define IVY_CAPTURE_SIGNATURE(qualifiers) \
    template<class C, class R, class... Args> \
    struct CaptureSignature<R(C::*)(Args...) qualifiers> : CaptureSignature<R(Args...)> {};
IVY_CAPTURE_SIGNATURE()
IVY_CAPTURE_SIGNATURE(const)
IVY_CAPTURE_SIGNATURE(&)
IVY_CAPTURE_SIGNATURE(const &)
IVY_CAPTURE_SIGNATURE(noexcept)
IVY_CAPTURE_SIGNATURE(const noexcept)
IVY_CAPTURE_SIGNATURE(& noexcept)
IVY_CAPTURE_SIGNATURE(const & noexcept)
#undef IVY_CAPTURE_SIGNATURE

template<class Callback, class = void>
struct CaptureCallback : CaptureSignature<Callback> {};

template<class Callback>
struct CaptureCallback<Callback, std::void_t<decltype(&Callback::operator())>>
    : CaptureSignature<decltype(&Callback::operator())> {};

template<class T>
constexpr std::string_view capture_type_name() noexcept {
    if constexpr (std::same_as<T, long>) return "long";
    if constexpr (std::same_as<T, double>) return "double";
    if constexpr (std::same_as<T, bool>) return "bool";
    return "std::string_view";
}

template<class T>
std::string_view convert_capture(std::string_view text, T& value) noexcept {
    static_assert(capture_type<T>);
    if constexpr (std::same_as<T, std::string_view>) {
        value = text;
        return {};
    } else if constexpr (std::same_as<T, bool>) {
        auto digits = text;
        if (digits.starts_with('+') || digits.starts_with('-')) digits.remove_prefix(1);
        bool integer = !digits.empty(), nonzero = false;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') { integer = false; break; }
            nonzero = nonzero || digit != '0';
        }
        // Only zero/nonzero matters: arbitrarily large integers need no narrowing.
        if (integer) { value = nonzero; return {}; }
        if (!text.empty()) {
            switch (text.front()) {
            case 'f': case 'F': value = false; return {};
            case 't': case 'T': case 'v': case 'V': value = true; return {};
            }
        }
        return "expected an integer or a value starting with f/F, t/T or v/V";
    } else {
        if (text.empty()) return "empty numeric capture";
        if (text.starts_with('+')) {
            text.remove_prefix(1);
            if (text.starts_with('-')) return "invalid numeric syntax";
        }
        if (text.empty()) return "invalid numeric syntax";
        const auto end = text.data() + text.size();
        const auto result = std::from_chars(text.data(), end, value);
        if (result.ec == std::errc::invalid_argument) return "invalid numeric syntax";
        if (result.ec == std::errc::result_out_of_range) return "numeric value out of range";
        if (result.ptr != end) return "trailing characters in numeric capture";
        if constexpr (std::same_as<T, double>) {
            if (!std::isfinite(value)) return "expected a finite number";
        }
        return {};
    }
}

template<class Signature, class Callback, std::size_t... I>
void invoke_converted(Callback& callback, const void* owner, IvyClientPtr peer,
                      std::span<const std::string_view> arguments, std::index_sequence<I...>) {
    typename Signature::Values values{};
    auto status = ConvertStatus::OK;
    std::size_t failed_index = 0;
    std::string_view failed_type, reason;
    if (arguments.size() != sizeof...(I)) {
        status = ConvertStatus::COUNT_ERROR;
    } else if constexpr (sizeof...(I) > 0) {
        const auto convert = [&]<std::size_t Index>() {
            using T = std::tuple_element_t<Index, typename Signature::Values>;
            reason = convert_capture(arguments[Index], std::get<Index>(values));
            if (reason.empty()) return true;
            failed_index = Index;
            failed_type = capture_type_name<T>();
            status = ConvertStatus::CONVERT_ERROR;
            return false;
        };
        (void)(convert.template operator()<I>() && ...);
    }
    std::string diagnostic;
    std::string_view error;
    if (status != ConvertStatus::OK) {
        // Discard partial results so an error never exposes half-converted data.
        values = {};
        error = "conversion diagnostic unavailable: could not allocate error text";
        try {
            if (status == ConvertStatus::COUNT_ERROR)
                diagnostic = std::format("capture count mismatch: expected {}, received {}",
                    sizeof...(I), arguments.size());
            else
                diagnostic = std::format("capture {}: cannot convert \"{}\" to {}: {}",
                    failed_index + 1, arguments[failed_index], failed_type, reason);
            error = diagnostic;
        } catch (const std::bad_alloc&) {
        } catch (const std::length_error&) {
        }
    }
    const ConversionContext context(owner, error);
    if constexpr (Signature::with_peer)
        std::invoke(callback, status, peer, std::get<I>(values)...);
    else
        std::invoke(callback, status, std::get<I>(values)...);
}

template<class Callback>
Bus::MessageCallback converted_callback(Callback&& callback, const void* owner) {
    using Stored = std::decay_t<Callback>;
    using Signature = CaptureCallback<Stored>;
    static_assert(Signature::valid,
        "bind_convert requires a non-generic, unambiguous void callback taking ConvertStatus first, "
        "an optional IvyClientPtr, then only long, double, std::string_view or bool by value");
    if constexpr (Signature::valid) {
        // Preserve bind_raw's rejection of null function pointers and empty std::function /
        // std::move_only_function objects before hiding them inside a nonempty lambda.
        if constexpr (!std::is_function_v<std::remove_reference_t<Callback>> &&
                      requires { static_cast<bool>(callback); }) {
            if (!static_cast<bool>(callback)) return {};
        }
        return [function = Stored(std::forward<Callback>(callback)), owner]
            (IvyClientPtr peer, std::span<const std::string_view> arguments) mutable {
                invoke_converted<Signature>(function, owner, peer, arguments,
                    std::make_index_sequence<std::tuple_size_v<typename Signature::Values>>{});
            };
    }
}

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

template<class Callback>
Bus::BindResult Bus::bind_raw(Callback&& callback, AnchoredRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp.get(), true);
    });
}

template<class Callback>
Bus::BindResult Bus::bind_raw(Callback&& callback, RuntimeRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp.text, true);
    });
}

template<class Callback>
Bus::BindResult Bus::bind_convert(Callback&& callback, AnchoredRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(detail::converted_callback(std::forward<Callback>(callback), impl_.get()), regexp.get(), true);
    });
}

template<class Callback>
Bus::BindResult Bus::bind_convert(Callback&& callback, RuntimeRegexp regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(detail::converted_callback(std::forward<Callback>(callback), impl_.get()), regexp.text, true);
    });
}

template<class Callback, class... Args> requires (sizeof...(Args) > 0)
Bus::BindResult Bus::bind_convert(Callback&& callback,
    AnchoredFormat<Args...> format, Args&&... args) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::formatter_failed), [&] {
        return bind_convert(std::forward<Callback>(callback),
            runtime_regexp(std::format(format.get(), std::forward<Args>(args)...)));
    });
}

template<class Callback> requires std::constructible_from<Bus::MessageCallback, Callback>
Bus::BindResult Bus::bind_raw_unanchored(Callback&& callback, std::string_view regexp) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_impl(MessageCallback(std::forward<Callback>(callback)), regexp, false);
    });
}

template<class Callback> requires std::constructible_from<Bus::DirectCallback, Callback>
Bus::DirectBindResult Bus::bind_direct(Callback&& callback) noexcept {
    return detail::guard<DirectBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_direct_impl(DirectCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback, class... Args>
    requires (sizeof...(Args) > 0 && std::constructible_from<Bus::MessageCallback, Callback>)
Bus::BindResult Bus::bind_raw(Callback&& callback, AnchoredFormat<Args...> format, Args&&... args) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::formatter_failed), [&] {
        return bind_raw(std::forward<Callback>(callback),
            runtime_regexp(std::format(format.get(), std::forward<Args>(args)...)));
    });
}

template<class Callback, class... Args>
    requires (sizeof...(Args) > 0 && std::constructible_from<Bus::MessageCallback, Callback>)
Bus::BindResult Bus::bind_raw_unanchored(Callback&& callback,
    std::format_string<Args...> format, Args&&... args) noexcept {
    return detail::guard<BindResult>(make_error_code(Error::formatter_failed), [&] {
        return bind_raw_unanchored(std::forward<Callback>(callback), std::format(format, std::forward<Args>(args)...));
    });
}

// Select the event tag before checking the callable's signature. Constraining
// every candidate would instantiate generic lambdas with unrelated event types.
template<class Callback>
Bus::EventBindResult Bus::bind_event(Callback&& callback, PongTag) noexcept {
    return detail::guard<EventBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_pong_impl(PongCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback>
Bus::EventBindResult Bus::bind_event(Callback&& callback, RemoteBindingsTag) noexcept {
    return detail::guard<EventBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_remote_bindings_impl(RemoteBindingsCallback(std::forward<Callback>(callback)));
    });
}

template<class Callback>
Bus::TimerBindResult Bus::bind_event(Callback&& callback, Every schedule) noexcept {
    return detail::guard<TimerBindResult>(make_error_code(Error::callback_failed), [&] {
        return bind_timer_impl(TimerCallback(std::forward<Callback>(callback)), schedule);
    });
}

template<class Callback>
Bus::TimerBindResult Bus::bind_event(Callback&& callback, After schedule) noexcept {
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
