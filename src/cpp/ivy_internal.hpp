/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once

// Shared state for the compiled implementation. This header is not installed.
#include "ivy.hpp"

#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace ivy {

struct Subscription::State {
    enum class Kind { regexp, direct, pong, remote_bindings };
    struct Handler {
        Bus::MessageCallback message;
        Bus::DirectCallback direct;
        Bus::PongCallback pong;
        Bus::RemoteBindingsCallback remote_bindings;
        explicit Handler(Bus::MessageCallback callback) : message(std::move(callback)) {}
        explicit Handler(Bus::DirectCallback callback) : direct(std::move(callback)) {}
        explicit Handler(Bus::PongCallback callback) : pong(std::move(callback)) {}
        explicit Handler(Bus::RemoteBindingsCallback callback) : remote_bindings(std::move(callback)) {}
    };

    std::weak_ptr<Bus::Impl> owner;
    std::shared_ptr<Handler> handler;
    MsgRcvPtr binding = nullptr;
    std::size_t active_changes = 0;
    Kind kind = Kind::regexp;

    State*& slot(Bus::Impl& owner) noexcept;
    int set_native(IvyContext* context, bool enable) noexcept;
    static Bus::BindResult subscribe(const std::shared_ptr<Bus::Impl>& owner,
        Kind kind, std::shared_ptr<Handler> handler) noexcept;

    static void on_message(IvyClientPtr app, void* data, int argc, char** argv) noexcept;
    static void on_direct(IvyClientPtr app, void* data, int id, char* message) noexcept;
    static void on_pong(IvyClientPtr app, void* data, int delay) noexcept;
    static void on_remote_bindings(IvyClientPtr app, void* data, int id,
        const char* regexp, IvyBindEvent event) noexcept;
};

struct Bus::Impl {
    ApplicationCallback application_callback;
    DieCallback die_callback;
    IvyContext* context = nullptr;
    std::mutex callback_mutex;
    std::error_code callback_error;
    std::shared_ptr<TransportCallback> transport_callback;
    std::mutex subscriptions_mutex;
    // C may have copied a user_data pointer before unbind/replacement. These
    // small relay objects stay alive until the C context has been destroyed.
    std::vector<std::shared_ptr<Subscription::State>> subscriptions;
    Subscription::State* direct_subscription = nullptr;
    Subscription::State* pong_subscription = nullptr;
    Subscription::State* remote_bindings_subscription = nullptr;

    Impl(ApplicationCallback application, DieCallback die);
    ~Impl();

    void save_callback_error(std::error_code error) noexcept;
    static void on_application(IvyClientPtr app, void* data, IvyApplicationEvent event) noexcept;
    static void on_die(IvyClientPtr app, void* data, int id) noexcept;
    static void on_transport(IvyClientPtr app, void* data, IvyStatus status, int system_error) noexcept;
};

namespace detail {

inline std::expected<void, std::error_code> status_result(int status) noexcept {
    if (status != IVY_OK)
        return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
    return {};
}

inline bool stopped(IvyContext* context) noexcept {
    const auto state = IvyContextGetState(context);
    return state == IVY_CTX_STOPPING || state == IVY_CTX_STOPPED || state == IVY_CTX_DESTROYED;
}

} // namespace detail

} // namespace ivy
