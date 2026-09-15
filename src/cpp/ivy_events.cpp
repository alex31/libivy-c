/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <new>

namespace ivy {

Subscription::State*& Subscription::State::slot(Bus::Impl& owner) noexcept {
    switch (kind) {
    case Kind::direct: return owner.direct_subscription;
    case Kind::pong: return owner.pong_subscription;
    case Kind::remote_bindings: return owner.remote_bindings_subscription;
    case Kind::regexp: std::terminate(); // Only single-callback registrations have slots.
    }
    std::terminate();
}

int Subscription::State::set_native(IvyContext* context, bool enable) noexcept {
    switch (kind) {
    case Kind::direct:
        return IvyContextBindDirectMsg(context, enable ? on_direct : nullptr, enable ? this : nullptr);
    case Kind::pong:
        return IvyContextSetPongCallback(context, enable ? on_pong : nullptr, enable ? this : nullptr);
    case Kind::remote_bindings:
        return IvyContextSetBindCallback(context, enable ? on_remote_bindings : nullptr, enable ? this : nullptr);
    case Kind::regexp: return IVY_EINVAL;
    }
    return IVY_EINVAL;
}

Bus::BindResult Subscription::State::subscribe(const std::shared_ptr<Bus::Impl>& owner,
    Kind kind, std::shared_ptr<Handler> handler) noexcept {
    return detail::guard<Bus::BindResult>(make_error_code(IVY_EINVAL), [&]() -> Bus::BindResult {
        auto subscription = std::make_shared<State>();
        subscription->owner = owner;
        subscription->handler = std::move(handler);
        subscription->kind = kind;
        std::shared_ptr<Handler> retired;
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            owner->subscriptions.push_back(subscription);
            const int status = subscription->set_native(owner->context, true);
            if (status != IVY_OK) {
                owner->subscriptions.pop_back();
                return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
            }
            auto& slot = subscription->slot(*owner);
            if (slot)
                retired = std::exchange(slot->handler, {});
            slot = subscription.get();
        }
        // User captures may reenter the wrapper during destruction.
        return Subscription(std::move(subscription));
    });
}

void Subscription::State::on_pong(IvyClientPtr app, void* data, int delay) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || detail::stopped(owner->context)) return;
    std::shared_ptr<Handler> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        handler = state.handler;
    }
    if (!handler) return;
    try {
        handler->pong(app, delay);
    } catch (const std::bad_alloc&) {
        owner->save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        owner->save_callback_error(make_error_code(Error::callback_failed));
    }
}

void Subscription::State::on_remote_bindings(IvyClientPtr app, void* data, int id,
    const char* regexp, IvyBindEvent event) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || detail::stopped(owner->context)) return;
    std::shared_ptr<Handler> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        handler = state.handler;
    }
    if (!handler) return;
    try {
        handler->remote_bindings(app, id, regexp ? std::string_view(regexp) : std::string_view{}, event);
    } catch (const std::bad_alloc&) {
        owner->save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        owner->save_callback_error(make_error_code(Error::callback_failed));
    }
}

EventSubscription::EventSubscription() noexcept = default;
EventSubscription::EventSubscription(Subscription subscription) noexcept
    : subscription_(std::move(subscription)) {}
EventSubscription::~EventSubscription() = default;
EventSubscription::EventSubscription(EventSubscription&&) noexcept = default;
EventSubscription& EventSubscription::operator=(EventSubscription&&) noexcept = default;

std::expected<void, std::error_code> EventSubscription::unbind() noexcept {
    return subscription_.unbind();
}

bool EventSubscription::is_bound() const noexcept { return subscription_.is_bound(); }

Bus::DirectBindResult Bus::bind_direct_impl(DirectCallback callback) noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback) return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<DirectBindResult>(make_error_code(IVY_EINVAL), [&]() -> DirectBindResult {
        auto result = Subscription::State::subscribe(owner, Subscription::State::Kind::direct,
            std::make_shared<Subscription::State::Handler>(std::move(callback)));
        if (!result) return std::unexpected(result.error());
        return DirectSubscription(std::move(*result));
    });
}

Bus::EventBindResult Bus::bind_pong_impl(PongCallback callback) noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback) return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<EventBindResult>(make_error_code(IVY_EINVAL), [&]() -> EventBindResult {
        auto result = Subscription::State::subscribe(owner, Subscription::State::Kind::pong,
            std::make_shared<Subscription::State::Handler>(std::move(callback)));
        if (!result) return std::unexpected(result.error());
        return EventSubscription(std::move(*result));
    });
}

Bus::EventBindResult Bus::bind_remote_bindings_impl(RemoteBindingsCallback callback) noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback) return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<EventBindResult>(make_error_code(IVY_EINVAL), [&]() -> EventBindResult {
        auto result = Subscription::State::subscribe(owner, Subscription::State::Kind::remote_bindings,
            std::make_shared<Subscription::State::Handler>(std::move(callback)));
        if (!result) return std::unexpected(result.error());
        return EventSubscription(std::move(*result));
    });
}

} // namespace ivy
