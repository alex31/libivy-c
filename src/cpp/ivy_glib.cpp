/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_glib.hpp"

namespace ivy::glib::detail {
ContextScope::ContextScope(GMainContext* context) noexcept
    : context_(context ? g_main_context_ref(context) : g_main_context_ref_thread_default()),
      acquired_(g_main_context_acquire(context_)) {
    if (acquired_)
        g_main_context_push_thread_default(context_);
}

ContextScope::~ContextScope() {
    if (acquired_) {
        g_main_context_pop_thread_default(context_);
        g_main_context_release(context_);
    }
    g_main_context_unref(context_);
}
} // namespace ivy::glib::detail
