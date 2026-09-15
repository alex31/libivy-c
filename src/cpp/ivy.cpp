/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <new>
#include <stdexcept>
#include <string>

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

class CppErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "ivy-cpp"; }
    std::string message(int error) const override {
        switch (static_cast<Error>(error)) {
        case Error::callback_failed: return "callback construction or invocation failed";
        case Error::formatter_failed: return "custom formatter failed";
        default: return "unknown Ivy C++ error";
        }
    }
};

} // namespace

std::error_code make_error_code(IvyStatus status) noexcept {
    static IvyErrorCategory category;
    return {static_cast<int>(status), category};
}

std::error_code make_error_code(Error error) noexcept {
    static CppErrorCategory category;
    return {static_cast<int>(error), category};
}

Bus::Impl::Impl(ApplicationCallback application, DieCallback die)
    : application_callback(std::move(application)), die_callback(std::move(die)) {}

Bus::Impl::~Impl() {
    // Releasing callback storage after a rejected destruction would leave
    // dangling C user_data pointers. Destruction from a callback is forbidden.
    if (context && IvyContextDestroy(context) != IVY_OK)
        std::terminate();
    // Tokens may survive the Bus, but must not retain user captures then.
    // No token can lock its weak owner once this destructor has begun.
    for (const auto& subscription : subscriptions)
        subscription->handler.reset();
    for (const auto& timer : timers)
        timer->handler.reset();
}

void Bus::Impl::save_callback_error(std::error_code error) noexcept {
    {
        std::lock_guard lock(callback_mutex);
        if (!callback_error)
            callback_error = error;
    }
    (void)IvyContextStop(context);
}

void Bus::Impl::on_application(IvyClientPtr app, void* data,
                               IvyApplicationEvent event) noexcept {
    auto& self = *static_cast<Impl*>(data);
    try {
        self.application_callback(app, event);
    } catch (const std::bad_alloc&) {
        self.save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        self.save_callback_error(make_error_code(Error::callback_failed));
    }
}

void Bus::Impl::on_die(IvyClientPtr app, void* data, int id) noexcept {
    auto& self = *static_cast<Impl*>(data);
    try {
        self.die_callback(app, id);
    } catch (const std::bad_alloc&) {
        self.save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        self.save_callback_error(make_error_code(Error::callback_failed));
    }
}

void Bus::Impl::on_transport(IvyClientPtr app, void* data, IvyStatus status, int system_error) noexcept {
    auto& self = *static_cast<Impl*>(data);
    std::shared_ptr<TransportCallback> callback;
    {
        std::lock_guard lock(self.callback_mutex);
        callback = self.transport_callback;
    }
    if (!callback) return;
    try {
        (*callback)(app, make_error_code(status), system_error);
    } catch (const std::bad_alloc&) {
        self.save_callback_error(make_error_code(IVY_ENOMEM));
    } catch (...) {
        self.save_callback_error(make_error_code(Error::callback_failed));
    }
}

Bus::Bus(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Bus::CreateResult Bus::create_impl(std::string_view application_name,
    std::optional<std::string_view> ready, ApplicationCallback application, DieCallback die) noexcept {
    if (application_name.find('\0') != std::string_view::npos ||
        (ready && ready->find('\0') != std::string_view::npos))
        return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<CreateResult>(make_error_code(IVY_EINVAL), [&]() -> CreateResult {
        const std::string name(application_name);
        const auto ready_message = ready ? std::optional<std::string>(*ready) : std::nullopt;
        auto impl = std::make_shared<Impl>(std::move(application), std::move(die));
        impl->context = IvyContextCreate(
            name.c_str(), ready_message ? ready_message->c_str() : nullptr,
            impl->application_callback ? Impl::on_application : nullptr, impl.get(),
            impl->die_callback ? Impl::on_die : nullptr, impl.get());
        if (!impl->context)
            return std::unexpected(make_error_code(IvyGetLastError()));
        const auto registered = detail::status_result(
            IvyContextSetTransportErrorCallback(impl->context, Impl::on_transport, impl.get()));
        if (!registered)
            return std::unexpected(registered.error());
        return Bus(std::move(impl));
    });
}

Bus::~Bus() = default;
Bus::Bus(Bus&&) noexcept = default;
Bus& Bus::operator=(Bus&&) noexcept = default;

std::expected<void, std::error_code> Bus::start() noexcept {
    if (!impl_)
        return detail::status_result(IVY_ESTATE);
    return detail::status_result(IvyContextStart(impl_->context, nullptr));
}

std::expected<void, std::error_code> Bus::start(std::string_view bus) noexcept {
    if (!impl_)
        return detail::status_result(IVY_ESTATE);
    if (bus.find('\0') != std::string_view::npos)
        return detail::status_result(IVY_EINVAL);
    try {
        const std::string address(bus);
        return detail::status_result(IvyContextStart(impl_->context, address.c_str()));
    } catch (const std::bad_alloc&) {
        return detail::status_result(IVY_ENOMEM);
    } catch (const std::length_error&) {
        return detail::status_result(IVY_EINVAL);
    }
}

std::expected<void, std::error_code> Bus::stop() noexcept {
    if (!impl_)
        return {};
    return detail::status_result(IvyContextStop(impl_->context));
}

std::expected<void, std::error_code> Bus::request_stop() noexcept {
    if (!impl_)
        return {};
    return detail::status_result(IvyContextRequestStop(impl_->context));
}

std::expected<void, std::error_code> Bus::set_transport_error_callback_impl(TransportCallback callback) noexcept {
    const auto owner = impl_;
    if (!owner) return detail::status_result(IVY_ESTATE);
    if (detail::stopped(owner->context)) return detail::status_result(IVY_ESTOPPED);
    try {
        auto replacement = callback ? std::make_shared<TransportCallback>(std::move(callback)) : nullptr;
        {
            std::lock_guard lock(owner->callback_mutex);
            owner->transport_callback.swap(replacement);
        }
        return {};
    } catch (const std::bad_alloc&) {
        return detail::status_result(IVY_ENOMEM);
    }
}

IvyContextState Bus::state() const noexcept {
    return impl_ ? IvyContextGetState(impl_->context) : IVY_CTX_DESTROYED;
}

IvyContext* Bus::native_handle() const noexcept {
    return impl_ ? impl_->context : nullptr;
}

std::expected<void, std::error_code> Bus::take_callback_error() noexcept {
    if (!impl_)
        return {};
    std::lock_guard lock(impl_->callback_mutex);
    const auto error = std::exchange(impl_->callback_error, {});
    if (error)
        return std::unexpected(error);
    return {};
}

} // namespace ivy
