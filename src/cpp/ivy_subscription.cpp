/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>

namespace ivy {


void Subscription::State::on_message(IvyClientPtr app, void* data, int argc, char** argv) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || detail::stopped(owner->context))
        return;
    std::shared_ptr<Handler> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        handler = state.handler;
    }
    if (!handler)
        return;
    try {
        if (argc < 0 || (argc > 0 && !argv)) {
            owner->save_callback_error(make_error_code(IVY_EINVAL));
            return;
        }
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i)
            arguments.emplace_back(argv[i] ? argv[i] : "");
        handler->message(app, arguments);
    } catch (const std::bad_alloc&) {
        owner->save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        owner->save_callback_error(make_error_code(Error::callback_failed));
    }
}

void Subscription::State::on_direct(IvyClientPtr app, void* data, int id, char* message) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || detail::stopped(owner->context))
        return;
    std::shared_ptr<Handler> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        handler = state.handler;
    }
    if (!handler)
        return;
    try {
        handler->direct(app, id, message ? std::string_view(message) : std::string_view{});
    } catch (const std::bad_alloc&) {
        owner->save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        owner->save_callback_error(make_error_code(Error::callback_failed));
    }
}

Subscription::Subscription() noexcept = default;
Subscription::Subscription(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
Subscription::~Subscription() { (void)unbind(); }
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        (void)unbind();
        state_ = std::move(other.state_);
    }
    return *this;
}

std::expected<void, std::error_code> Subscription::unbind() noexcept {
    if (!state_)
        return {};
    const auto owner = state_->owner.lock();
    if (!owner)
        return {};

    // Destroy user captures outside locks; a running callback retains its own
    // shared reference, including when it destroys its own Subscription.
    std::shared_ptr<State::Handler> retired;
    MsgRcvPtr binding = nullptr;
    int status = IVY_OK;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        retired = std::exchange(state_->handler, {});
        if (state_->kind != State::Kind::regexp) {
            auto& slot = state_->slot(*owner);
            if (slot == state_.get()) {
                // C setters do not invoke user callbacks synchronously.
                status = state_->set_native(owner->context, false);
                slot = nullptr;
            }
        } else if (state_->active_changes == 0) {
            binding = std::exchange(state_->binding, nullptr);
        }
    }
    // Unbind may synchronously dispatch application events, so release the lock.
    if (binding)
        status = IvyContextUnbindMsg(owner->context, binding);
    // A stopped context will free its remaining C registrations on destruction.
    return detail::status_result(status == IVY_ESTOPPED ? IVY_OK : status);
}

bool Subscription::is_bound() const noexcept {
    if (!state_)
        return false;
    const auto owner = state_->owner.lock();
    if (!owner || detail::stopped(owner->context))
        return false;
    std::lock_guard lock(owner->subscriptions_mutex);
    return state_->handler != nullptr;
}

std::expected<void, std::error_code> Subscription::change(AnchoredRegexp regexp) noexcept {
    return change_impl(regexp.get(), true);
}

std::expected<void, std::error_code> Subscription::change(RuntimeRegexp regexp) noexcept {
    return change_impl(regexp.text, true);
}

std::expected<void, std::error_code> Subscription::change_unanchored(std::string_view regexp) noexcept {
    return change_impl(regexp, false);
}

std::expected<void, std::error_code> Subscription::change_impl(std::string_view regexp, bool anchored) noexcept {
    const auto state = state_;
    if (!state)
        return detail::status_result(IVY_ESTATE);
    const auto owner = state->owner.lock();
    if (!owner)
        return detail::status_result(IVY_ESTATE);
    if (regexp.find('\0') != std::string_view::npos)
        return detail::status_result(IVY_EINVAL);
    if (detail::stopped(owner->context))
        return detail::status_result(IVY_ESTOPPED);
    try {
        const std::string pattern(regexp);
        if (anchored) {
            const int status = IvyValidateAnchoredRegexp(pattern.c_str());
            if (status != IVY_OK)
                return detail::status_result(status);
        }
        MsgRcvPtr binding;
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            if (state->kind != State::Kind::regexp || !state->handler || !state->binding)
                return detail::status_result(IVY_ESTATE);
            binding = state->binding;
            ++state->active_changes;
        }

        // C may invoke application callbacks here. Pin the handle rather than
        // holding a wrapper lock: callbacks can change/unbind this same token.
        const auto changed = IvyContextChangeMsg(owner->context, binding, "%s", pattern.c_str());
        int status = changed ? IVY_OK : IvyGetLastError();
        MsgRcvPtr pending_removal = nullptr;
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            --state->active_changes;
            if (!state->handler) {
                if (status == IVY_OK)
                    status = IVY_ESTATE;
                if (state->active_changes == 0)
                    pending_removal = std::exchange(state->binding, nullptr);
            }
        }
        if (pending_removal)
            (void)IvyContextUnbindMsg(owner->context, pending_removal);
        return detail::status_result(status);
    } catch (const std::bad_alloc&) {
        return detail::status_result(IVY_ENOMEM);
    } catch (const std::length_error&) {
        return detail::status_result(IVY_EINVAL);
    }
}

DirectSubscription::DirectSubscription() noexcept = default;
DirectSubscription::DirectSubscription(Subscription subscription) noexcept
    : subscription_(std::move(subscription)) {}
DirectSubscription::~DirectSubscription() = default;
DirectSubscription::DirectSubscription(DirectSubscription&&) noexcept = default;
DirectSubscription& DirectSubscription::operator=(DirectSubscription&&) noexcept = default;

std::expected<void, std::error_code> DirectSubscription::unbind() noexcept {
    return subscription_.unbind();
}

bool DirectSubscription::is_bound() const noexcept {
    return subscription_.is_bound();
}

Bus::BindResult Bus::bind_impl(MessageCallback callback, std::string_view regexp, bool anchored) noexcept {
    const auto owner = impl_;
    if (!owner)
        return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback || regexp.find('\0') != std::string_view::npos)
        return std::unexpected(make_error_code(IVY_EINVAL));
    try {
        const std::string pattern(regexp);
        if (anchored) {
            const int status = IvyValidateAnchoredRegexp(pattern.c_str());
            if (status != IVY_OK)
                return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
        }
        auto subscription = std::make_shared<Subscription::State>();
        subscription->owner = owner;
        subscription->handler = std::make_shared<Subscription::State::Handler>(std::move(callback));
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            owner->subscriptions.push_back(subscription);
        }
        const auto binding = IvyContextBindMsg(owner->context, Subscription::State::on_message,
                                               subscription.get(), "%s", pattern.c_str());
        if (!binding) {
            const auto error = IvyGetLastError();
            std::lock_guard lock(owner->subscriptions_mutex);
            std::erase(owner->subscriptions, subscription);
            return std::unexpected(make_error_code(error));
        }
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            subscription->binding = binding;
        }
        return Subscription(std::move(subscription));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error_code(IVY_ENOMEM));
    } catch (const std::length_error&) {
        return std::unexpected(make_error_code(IVY_EINVAL));
    }
}

} // namespace ivy
