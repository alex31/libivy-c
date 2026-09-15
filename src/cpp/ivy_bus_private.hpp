/* C++ interface to Ivy. See ../version.h for the copyright notice. */
// Private declarations of ivy::Bus, assembled by ivy.hpp. This compiler header
// is installed alongside ivy_detail.hpp; application code only needs ivy.hpp.
#if !defined(IVY_CPP_API_HEADERS)
#include "ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Bus(std::shared_ptr<Impl> impl) noexcept;
    static CreateResult create_impl(std::string_view application_name,
        std::optional<std::string_view> ready, ApplicationCallback application, DieCallback die) noexcept;
    std::expected<void, std::error_code> set_filters_impl(std::span<const std::string> words) noexcept;
    EventBindResult bind_pong_impl(PongCallback callback) noexcept;
    EventBindResult bind_remote_bindings_impl(RemoteBindingsCallback callback) noexcept;
    TimerBindResult bind_timer_impl(TimerCallback callback, Every schedule, bool one_shot = false) noexcept;
    DirectBindResult bind_direct_impl(DirectCallback callback) noexcept;
    std::expected<void, std::error_code> set_transport_error_callback_impl(TransportCallback callback) noexcept;
    BindResult bind_impl(MessageCallback callback, std::string_view regexp, bool anchored) noexcept;
    friend class Subscription;
    friend class TimerSubscription;
// IVY_CPP_API_END

#endif
