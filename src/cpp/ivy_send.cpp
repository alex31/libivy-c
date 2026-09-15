/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

#include <climits>
#include <new>
#include <stdexcept>
#include <string>

namespace ivy {
namespace {

IvyStatus send_state(IvyContext* context) noexcept {
    if (!context) return IVY_ESTATE;
    const auto state = IvyContextGetState(context);
    if (state == IVY_CTX_STOPPING || state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    return state == IVY_CTX_RUNNING ? IVY_OK : IVY_ESTATE;
}

bool valid_message(std::string_view message) noexcept {
    return message.size() < static_cast<std::size_t>(INT_MAX) &&
        message.find_first_of(std::string_view("\0\n\002\003", 4)) == std::string_view::npos;
}

} // namespace

SendReport Bus::send_report(std::string_view message) noexcept {
    SendReport result;
    const auto owner = impl_;
    const auto state = send_state(owner ? owner->context : nullptr);
    if (state != IVY_OK || !valid_message(message)) {
        result.error = make_error_code(state != IVY_OK ? state : IVY_EINVAL);
        return result;
    }
    IvySendReport report{};
    const int status = IvyContextSendMsgEx(owner->context, &report, "%.*s",
        static_cast<int>(message.size()), message.empty() ? "" : message.data());
    result.matched = report.matched;
    result.accepted = report.accepted;
    result.failed = report.failed;
    result.error = make_error_code(static_cast<IvyStatus>(status));
    if (report.system_error)
        result.system_error = {report.system_error, std::system_category()};
    return result;
}

Bus::SendResult Bus::send(std::string_view message) noexcept {
    const auto result = send_report(message);
    if (result.error) return std::unexpected(result.error);
    return result.accepted;
}

std::expected<void, std::error_code> Bus::send(IvyClientPtr peer, int id, std::string_view message) noexcept {
    const auto owner = impl_;
    const auto state = send_state(owner ? owner->context : nullptr);
    if (state != IVY_OK) return detail::status_result(state);
    if (!peer || !valid_message(message)) return detail::status_result(IVY_EINVAL);
    try {
        std::string text(message);
        return detail::status_result(IvyContextSendDirectMsg(owner->context, peer, id, text.data()));
    } catch (const std::bad_alloc&) {
        return detail::status_result(IVY_ENOMEM);
    } catch (const std::length_error&) {
        return detail::status_result(IVY_EINVAL);
    }
}


std::expected<void, std::error_code> Bus::send_die(IvyClientPtr peer) noexcept {
    const auto owner = impl_;
    const auto state = send_state(owner ? owner->context : nullptr);
    if (state != IVY_OK) return detail::status_result(state);
    if (!peer) return detail::status_result(IVY_EINVAL);
    return detail::status_result(IvyContextSendDieMsg(owner->context, peer));
}

std::expected<void, std::error_code>
Bus::send_error(IvyClientPtr peer, int id, std::string_view message) noexcept {
    const auto owner = impl_;
    const auto state = send_state(owner ? owner->context : nullptr);
    if (state != IVY_OK) return detail::status_result(state);
    if (!peer || !valid_message(message)) return detail::status_result(IVY_EINVAL);
    return detail::status_result(IvyContextSendError(owner->context, peer, id, "%.*s",
        static_cast<int>(message.size()), message.empty() ? "" : message.data()));
}

} // namespace ivy
