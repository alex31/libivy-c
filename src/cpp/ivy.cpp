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

} // namespace

std::error_code make_error_code(IvyStatus status) noexcept {
    static IvyErrorCategory category;
    return {static_cast<int>(status), category};
}

struct Bus::Impl {
    ApplicationCallback application_callback;
    DieCallback die_callback;
    IvyContext* context = nullptr;
    std::mutex exception_mutex;
    std::exception_ptr callback_exception;
    Impl(ApplicationCallback application, DieCallback die)
        : application_callback(std::move(application)), die_callback(std::move(die)) {}

    ~Impl() {
        // Releasing callback storage after a rejected destruction would leave
        // dangling C user_data pointers. Destruction from a callback is forbidden.
        if (context && IvyContextDestroy(context) != IVY_OK)
            std::terminate();
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
