/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"
#include "ivy_query_internal.h"

namespace ivy {
namespace {

struct Snapshot {
    IvyStringSnapshot value{};
    Snapshot() = default;
    ~Snapshot() { IvyStringSnapshotFreeInternal(&value); }
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
};

std::expected<std::vector<std::string>, std::error_code> strings_result(int status, const Snapshot& snapshot) {
    if (status != IVY_OK) return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
    std::vector<std::string> result;
    result.reserve(snapshot.value.count);
    for (std::size_t i = 0; i < snapshot.value.count; ++i)
        result.emplace_back(snapshot.value.items[i]);
    return result;
}

} // namespace

std::expected<std::optional<IvyClientPtr>, std::error_code>
Bus::find_application(std::string_view name) const noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    if (name.find('\0') != std::string_view::npos) return std::unexpected(make_error_code(IVY_EINVAL));
    return detail::guard<std::expected<std::optional<IvyClientPtr>, std::error_code>>(
        make_error_code(IVY_EINVAL), [&]() -> std::expected<std::optional<IvyClientPtr>, std::error_code> {
            std::string text(name);
            const auto peer = IvyContextGetApplication(owner->context, text.data());
            if (peer) return peer;
            const auto status = IvyGetLastError();
            if (status != IVY_OK) return std::unexpected(make_error_code(status));
            return std::nullopt;
        });
}

std::expected<std::pair<std::string, std::string>, std::error_code>
Bus::application(IvyClientPtr peer) const noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    return detail::guard<std::expected<std::pair<std::string, std::string>, std::error_code>>(
        make_error_code(IVY_EINVAL), [&]() -> std::expected<std::pair<std::string, std::string>, std::error_code> {
            Snapshot snapshot;
            const auto status = IvyContextCopyApplicationInternal(owner->context, peer, &snapshot.value);
            if (status != IVY_OK) return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
            return std::pair{std::string(snapshot.value.items[0]), std::string(snapshot.value.items[1])};
        });
}

std::expected<std::vector<std::string>, std::error_code> Bus::applications() const noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    return detail::guard<std::expected<std::vector<std::string>, std::error_code>>(
        make_error_code(IVY_EINVAL), [&] {
            Snapshot snapshot;
            const auto status = IvyContextCopyApplicationsInternal(owner->context, &snapshot.value);
            return strings_result(status, snapshot);
        });
}

std::expected<ApplicationInfo, std::error_code> Bus::application_info(IvyClientPtr peer) const noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    return detail::guard<std::expected<ApplicationInfo, std::error_code>>(
        make_error_code(IVY_EINVAL), [&]() -> std::expected<ApplicationInfo, std::error_code> {
            Snapshot snapshot;
            unsigned short port = 0;
            const auto status = IvyContextCopyApplicationInfoInternal(owner->context, peer, &snapshot.value, &port);
            if (status != IVY_OK) return std::unexpected(make_error_code(static_cast<IvyStatus>(status)));
            return ApplicationInfo{std::string(snapshot.value.items[0]),
                std::string(snapshot.value.items[1]), static_cast<std::uint16_t>(port)};
        });
}

std::expected<std::vector<std::string>, std::error_code>
Bus::application_regexps(IvyClientPtr peer) const noexcept {
    const auto owner = impl_;
    if (!owner) return std::unexpected(make_error_code(IVY_ESTATE));
    return detail::guard<std::expected<std::vector<std::string>, std::error_code>>(
        make_error_code(IVY_EINVAL), [&] {
            Snapshot snapshot;
            const auto status = IvyContextCopyApplicationRegexpsInternal(owner->context, peer, &snapshot.value);
            return strings_result(status, snapshot);
        });
}

} // namespace ivy
