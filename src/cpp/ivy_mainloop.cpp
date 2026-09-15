/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_internal.hpp"

namespace ivy {
std::expected<void, std::error_code> Bus::run() noexcept {
    if (!impl_)
        return detail::status_result(IVY_ESTATE);
    return detail::status_result(IvyContextRun(impl_->context));
}
} // namespace ivy
