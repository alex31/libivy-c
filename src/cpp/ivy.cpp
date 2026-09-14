/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy.hpp"

#include <algorithm>
#include <climits>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ivy {
namespace {

class IvyErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "ivy"; }

    std::string message(int status) const override {
        switch (status) {
        case IVY_OK:       return "success";
        case IVY_ESTOPPED: return "context is stopping or stopped";
        case IVY_ESTATE:   return "invalid context state";
        case IVY_EINVAL:   return "invalid argument or handle";
        case IVY_ENOMEM:   return "insufficient memory or buffer space";
        case IVY_EIO:     return "transport I/O failure";
        case IVY_EUNANCHORED: return "regexp must start with '^' and be anchored";
        case IVY_EFIFOFULL: return "outgoing FIFO cannot accept the complete frame";
        default:          return "unknown Ivy status";
        }
    }
};

void check_status(int status, const char* operation) {
    if (status != IVY_OK)
        throw std::system_error(make_error_code(static_cast<IvyStatus>(status)), operation);
}

std::expected<void, std::error_code> status_result(int status) noexcept {
    if (status != IVY_OK)
        return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
    return {};
}

std::string terminated_string(std::string_view value, const char* parameter) {
    if (value.find('\0') != std::string_view::npos)
        throw std::invalid_argument(std::string(parameter) + " contains a NUL byte");
    return std::string(value);
}

bool stopped(IvyContext* context) noexcept {
    const auto state = IvyContextGetState(context);
    return state == IVY_CTX_STOPPING || state == IVY_CTX_STOPPED || state == IVY_CTX_DESTROYED;
}

} // namespace

std::error_code make_error_code(IvyStatus status) noexcept {
    static IvyErrorCategory category;
    return {static_cast<int>(status), category};
}

struct Subscription::State {
    struct Handler {
        Bus::MessageCallback message;
        Bus::DirectCallback direct;
        explicit Handler(Bus::MessageCallback callback) : message(std::move(callback)) {}
        explicit Handler(Bus::DirectCallback callback) : direct(std::move(callback)) {}
    };

    std::weak_ptr<Bus::Impl> owner;
    std::shared_ptr<Handler> handler;
    MsgRcvPtr binding = nullptr;
    std::size_t active_changes = 0;
    bool direct = false;

    static void on_message(IvyClientPtr app, void* data, int argc, char** argv) noexcept;
    static void on_direct(IvyClientPtr app, void* data, int id, char* message) noexcept;
};

struct Bus::Impl {
    ApplicationCallback application_callback;
    DieCallback die_callback;
    IvyContext* context = nullptr;
    std::mutex exception_mutex;
    std::exception_ptr callback_exception;
    std::mutex subscriptions_mutex;
    // C may have copied a user_data pointer before unbind/replacement. These
    // small relay objects stay alive until the C context has been destroyed.
    std::vector<std::shared_ptr<Subscription::State>> subscriptions;
    Subscription::State* direct_subscription = nullptr;

    Impl(ApplicationCallback application, DieCallback die)
        : application_callback(std::move(application)), die_callback(std::move(die)) {}

    ~Impl() {
        // Releasing callback storage after a rejected destruction would leave
        // dangling C user_data pointers. Destruction from a callback is forbidden.
        if (context && IvyContextDestroy(context) != IVY_OK)
            std::terminate();
        // Tokens may survive the Bus, but must not retain user captures then.
        // No token can lock its weak owner once this destructor has begun.
        for (const auto& subscription : subscriptions)
            subscription->handler.reset();
    }

    void save_callback_exception() noexcept {
        {
            std::lock_guard lock(exception_mutex);
            if (!callback_exception)
                callback_exception = std::current_exception();
        }
        (void)IvyContextStop(context);
    }

    static void on_application(IvyClientPtr app, void* data,
                               IvyApplicationEvent event) noexcept {
        auto& self = *static_cast<Impl*>(data);
        try {
            self.application_callback(app, event);
        } catch (...) {
            self.save_callback_exception();
        }
    }

    static void on_die(IvyClientPtr app, void* data, int id) noexcept {
        auto& self = *static_cast<Impl*>(data);
        try {
            self.die_callback(app, id);
        } catch (...) {
            self.save_callback_exception();
        }
    }

};

void Subscription::State::on_message(IvyClientPtr app, void* data, int argc, char** argv) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || stopped(owner->context))
        return;
    std::shared_ptr<Handler> handler;
    {
        std::lock_guard lock(owner->subscriptions_mutex);
        handler = state.handler;
    }
    if (!handler)
        return;
    try {
        if (argc < 0 || (argc > 0 && !argv))
            throw std::invalid_argument("invalid Ivy capture array");
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i)
            arguments.emplace_back(argv[i] ? argv[i] : "");
        handler->message(app, arguments);
    } catch (...) {
        owner->save_callback_exception();
    }
}

void Subscription::State::on_direct(IvyClientPtr app, void* data, int id, char* message) noexcept {
    auto& state = *static_cast<State*>(data);
    const auto owner = state.owner.lock();
    if (!owner || stopped(owner->context))
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
    } catch (...) {
        owner->save_callback_exception();
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
        if (state_->direct) {
            if (owner->direct_subscription == state_.get()) {
                // This C setter never invokes user callbacks synchronously.
                status = IvyContextBindDirectMsg(owner->context, nullptr, nullptr);
                owner->direct_subscription = nullptr;
            }
        } else if (state_->active_changes == 0) {
            binding = std::exchange(state_->binding, nullptr);
        }
    }
    // Unbind may synchronously dispatch application events, so release the lock.
    if (binding)
        status = IvyContextUnbindMsg(owner->context, binding);
    // A stopped context will free its remaining C registrations on destruction.
    return status_result(status == IVY_ESTOPPED ? IVY_OK : status);
}

bool Subscription::is_bound() const noexcept {
    if (!state_)
        return false;
    const auto owner = state_->owner.lock();
    if (!owner || stopped(owner->context))
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
        return status_result(IVY_ESTATE);
    const auto owner = state->owner.lock();
    if (!owner)
        return status_result(IVY_ESTATE);
    if (regexp.find('\0') != std::string_view::npos)
        return status_result(IVY_EINVAL);
    if (stopped(owner->context))
        return status_result(IVY_ESTOPPED);
    try {
        const std::string pattern(regexp);
        if (anchored) {
            const int status = IvyValidateAnchoredRegexp(pattern.c_str());
            if (status != IVY_OK)
                return status_result(status);
        }
        MsgRcvPtr binding;
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            if (state->direct || !state->handler || !state->binding)
                return status_result(IVY_ESTATE);
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
        return status_result(status);
    } catch (const std::bad_alloc&) {
        return status_result(IVY_ENOMEM);
    } catch (const std::length_error&) {
        return status_result(IVY_EINVAL);
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

Bus::Bus(std::string_view application_name, std::optional<std::string_view> ready,
         ApplicationCallback application_callback, DieCallback die_callback)
    : impl_(std::make_shared<Impl>(std::move(application_callback), std::move(die_callback))) {
    const auto name = terminated_string(application_name, "application name");
    const auto ready_message = ready
        ? std::optional<std::string>(terminated_string(*ready, "ready message"))
        : std::nullopt;

    impl_->context = IvyContextCreate(
        name.c_str(), ready_message ? ready_message->c_str() : nullptr,
        impl_->application_callback ? Impl::on_application : nullptr, impl_.get(),
        impl_->die_callback ? Impl::on_die : nullptr, impl_.get());
    if (!impl_->context)
        throw std::system_error(make_error_code(IvyGetLastError()), "IvyContextCreate");
}

Bus::~Bus() = default;
Bus::Bus(Bus&&) noexcept = default;
Bus& Bus::operator=(Bus&&) noexcept = default;

std::expected<void, std::error_code> Bus::start() noexcept {
    if (!impl_)
        return status_result(IVY_ESTATE);
    return status_result(IvyContextStart(impl_->context, nullptr));
}

std::expected<void, std::error_code> Bus::start(std::string_view bus) noexcept {
    if (!impl_)
        return status_result(IVY_ESTATE);
    if (bus.find('\0') != std::string_view::npos)
        return status_result(IVY_EINVAL);
    try {
        const std::string address(bus);
        return status_result(IvyContextStart(impl_->context, address.c_str()));
    } catch (const std::bad_alloc&) {
        return status_result(IVY_ENOMEM);
    } catch (const std::length_error&) {
        return status_result(IVY_EINVAL);
    }
}

void Bus::stop() {
    if (impl_)
        check_status(IvyContextStop(impl_->context), "IvyContextStop");
}

Bus::BindResult Bus::bind(MessageCallback callback, AnchoredRegexp regexp) noexcept {
    return bind_impl(std::move(callback), regexp.get(), true);
}

Bus::BindResult Bus::bind(MessageCallback callback, RuntimeRegexp regexp) noexcept {
    return bind_impl(std::move(callback), regexp.text, true);
}

Bus::BindResult Bus::bind_unanchored(MessageCallback callback, std::string_view regexp) noexcept {
    return bind_impl(std::move(callback), regexp, false);
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

Bus::DirectBindResult Bus::bind(DirectCallback callback) noexcept {
    const auto owner = impl_;
    if (!owner)
        return std::unexpected(make_error_code(IVY_ESTATE));
    if (!callback)
        return std::unexpected(make_error_code(IVY_EINVAL));
    try {
        auto subscription = std::make_shared<Subscription::State>();
        subscription->owner = owner;
        subscription->handler = std::make_shared<Subscription::State::Handler>(std::move(callback));
        subscription->direct = true;
        std::shared_ptr<Subscription::State::Handler> retired;
        {
            std::lock_guard lock(owner->subscriptions_mutex);
            owner->subscriptions.push_back(subscription);
            const int status = IvyContextBindDirectMsg(owner->context, Subscription::State::on_direct,
                                                       subscription.get());
            if (status != IVY_OK) {
                owner->subscriptions.pop_back();
                return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
            }
            if (owner->direct_subscription)
                retired = std::exchange(owner->direct_subscription->handler, {});
            owner->direct_subscription = subscription.get();
        }
        return DirectSubscription(Subscription(std::move(subscription)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error_code(IVY_ENOMEM));
    }
}

IvyContextState Bus::state() const noexcept {
    return impl_ ? IvyContextGetState(impl_->context) : IVY_CTX_DESTROYED;
}

IvyContext* Bus::native_handle() const noexcept {
    return impl_ ? impl_->context : nullptr;
}

void Bus::rethrow_callback_exception() {
    if (!impl_)
        return;
    std::exception_ptr exception;
    {
        std::lock_guard lock(impl_->exception_mutex);
        exception = std::exchange(impl_->callback_exception, {});
    }
    if (exception)
        std::rethrow_exception(exception);
}

} // namespace ivy
