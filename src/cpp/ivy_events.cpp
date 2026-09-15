/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <new>

namespace ivy {

Subscription::State*& Subscription::State::slot(Bus::Impl& owner) noexcept {
    switch (kind) {
    case Kind::direct: return owner.direct_subscription;
    case Kind::regexp: std::terminate(); // Only single-callback registrations have slots.
    }
    std::terminate();
}

int Subscription::State::set_native(IvyContext* context, bool enable) noexcept {
    switch (kind) {
    case Kind::direct:
        return IvyContextBindDirectMsg(context, enable ? on_direct : nullptr, enable ? this : nullptr);
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

} // namespace ivy
