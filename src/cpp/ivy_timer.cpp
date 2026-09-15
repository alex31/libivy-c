/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <algorithm>
#include <new>

namespace ivy {

std::expected<void, std::error_code>
TimerSubscription::State::schedule(std::chrono::milliseconds period) noexcept {
    const auto context_owner = owner.lock();
    if (!context_owner) return detail::status_result(IVY_ESTATE);
    if (period.count() < 0 || (!one_shot && period.count() == 0) || !std::in_range<long>(period.count()))
        return detail::status_result(IVY_EINVAL);
    if (detail::stopped(context_owner->context)) return detail::status_result(IVY_ESTOPPED);
    return detail::guard<std::expected<void, std::error_code>>(
        make_error_code(IVY_EINVAL), [&]() -> std::expected<void, std::error_code> {
            auto relay = std::make_unique<Registration>(this);
            auto* registration = relay.get();
            {
                std::lock_guard lock(context_owner->subscriptions_mutex);
                if (!handler) return detail::status_result(IVY_ESTATE);
                registrations.push_back(std::move(relay));
            }
            // C may post to the loop and wait. Never hold a wrapper lock here.
            // The pending relay also covers expiry before this call returns.
            const auto timer = IvyContextTimerRepeatAfter(context_owner->context,
                TIMER_LOOP, static_cast<long>(period.count()), on_timer, registration);
            const auto error = timer ? IVY_OK : IvyGetLastError();
            {
                std::lock_guard lock(context_owner->subscriptions_mutex);
                if (!timer) {
                    std::erase_if(registrations, [registration](const auto& entry) {
                        return entry.get() == registration;
                    });
                    return detail::status_result(error);
                }
                registration->pending = false;
                if (!handler) return detail::status_result(IVY_ESTATE);
                if (detail::stopped(context_owner->context))
                    return detail::status_result(IVY_ESTOPPED);
                current = registration;
            }
            return {};
        });
}

void TimerSubscription::State::on_timer(TimerId timer, void* data, unsigned long lateness) noexcept {
    auto& registration = *static_cast<Registration*>(data);
    auto& state = *registration.state;
    const auto owner = state.owner.lock();
    if (!owner) return;
    std::shared_ptr<Bus::TimerCallback> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        if (registration.pending) return;
        if (state.current == &registration && !detail::stopped(owner->context))
            handler = state.handler;
        if (handler && state.remaining && --*state.remaining == 0) {
            // Count actual selected invocations, not native expirations while a
            // registration is pending. Keep this last callback alive locally.
            state.handler.reset();
            state.current = nullptr;
        }
    }
    if (!handler) {
        TimerRemove(timer); // Only the native event-loop thread touches timer handles.
        return;
    }
    try {
        if (!std::in_range<std::chrono::milliseconds::rep>(lateness)) {
            owner->save_callback_error(make_error_code(IVY_EINVAL));
        } else {
            (*handler)(std::chrono::milliseconds(lateness));
        }
    } catch (const std::bad_alloc&) {
        owner->save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        owner->save_callback_error(make_error_code(Error::callback_failed));
    }
    bool retired;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        retired = !state.handler || state.current != &registration || detail::stopped(owner->context);
    }
    if (retired) TimerRemove(timer); // Includes self-unbind or a period change in the callback.
}

TimerSubscription::TimerSubscription() noexcept = default;
TimerSubscription::TimerSubscription(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
TimerSubscription::~TimerSubscription() { (void)unbind(); }
TimerSubscription::TimerSubscription(TimerSubscription&&) noexcept = default;
TimerSubscription& TimerSubscription::operator=(TimerSubscription&& other) noexcept {
    if (this != &other) {
        (void)unbind();
        state_ = std::move(other.state_);
    }
    return *this;
}

std::expected<void, std::error_code> TimerSubscription::unbind() noexcept {
    if (!state_) return {};
    const auto owner = state_->owner.lock();
    if (!owner) return {};
    std::shared_ptr<Bus::TimerCallback> retired;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        retired = std::exchange(state_->handler, {});
        state_->current = nullptr;
    }
    return {};
}

bool TimerSubscription::is_bound() const noexcept {
    if (!state_) return false;
    const auto owner = state_->owner.lock();
    if (!owner || detail::stopped(owner->context)) return false;
    std::lock_guard lock(owner->subscriptions_mutex);
    return state_->handler && state_->current;
}

std::expected<void, std::error_code>
TimerSubscription::set_period(std::chrono::milliseconds period) noexcept {
    if (!state_) return detail::status_result(IVY_ESTATE);
    return state_->schedule(period);
}

Bus::TimerBindResult Bus::bind_timer_impl(TimerCallback callback, Every schedule, bool one_shot) noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback || (schedule.count && *schedule.count <= 0))
        return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<TimerBindResult>(make_error_code(IVY_EINVAL), [&]() -> TimerBindResult {
        auto timer = std::make_shared<TimerSubscription::State>();
        timer->owner = owner;
        timer->remaining = schedule.count;
        timer->one_shot = one_shot;
        timer->handler = std::make_shared<TimerCallback>(std::move(callback));
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            owner->timers.push_back(timer);
        }
        const auto result = timer->schedule(schedule.period);
        if (!result) {
            // C may have created a relay just before a concurrent stop. Retain
            // its storage until context destruction, but release user captures.
            std::shared_ptr<TimerCallback> retired;
            {
                std::lock_guard lock(owner->subscriptions_mutex);
                retired = std::exchange(timer->handler, {});
                if (timer->registrations.empty()) std::erase(owner->timers, timer);
            }
            return std::unexpected(result.error());
        }
        return TimerSubscription(std::move(timer));
    });
}

} // namespace ivy
