#include "ivy.hpp"

#include <cassert>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <exception>
#include <iostream>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

// A controllable C boundary lets us exercise failure and lifetime contracts
// without relying on allocation failures or a particular network setup.
struct IvyContext {
    std::string name;
    std::optional<std::string> ready;
    std::optional<std::string> address;
    IvyApplicationCallback application;
    void* application_data;
    IvyDieCallback die;
    void* die_data;
    IvyContextState state = IVY_CTX_CREATED;
    bool in_callback = false;
    std::vector<MsgRcvPtr> bindings{};
    MsgDirectCallback direct = nullptr;
    void* direct_data = nullptr;
};

struct _clnt_lst_dict {};
struct _msg_rcv {
    MsgCallback callback;
    void* data;
    std::string regexp;
};

static IvyStatus last_error = IVY_OK;
static IvyStatus create_error = IVY_OK;
static IvyStatus start_error = IVY_OK;
static IvyStatus bind_error = IVY_OK;
static IvyStatus change_error = IVY_OK;
static std::move_only_function<void()> during_change;
static int unbind_count = 0;
static int live_contexts = 0;
static int destroyed_contexts = 0;

extern "C" {
IvyContext* IvyContextCreate(const char* name, const char* ready,
                            IvyApplicationCallback application, void* application_data,
                            IvyDieCallback die, void* die_data) {
    last_error = create_error;
    if (create_error != IVY_OK)
        return nullptr;
    auto* ctx = new IvyContext{name, ready ? std::optional<std::string>(ready) : std::nullopt,
                               std::nullopt, application, application_data, die, die_data};
    ++live_contexts;
    return ctx;
}

int IvyContextStart(IvyContext* ctx, const char* bus) {
    if (ctx->state != IVY_CTX_CREATED)
        return IVY_ESTATE;
    if (start_error != IVY_OK)
        return start_error;
    ctx->address = bus ? std::optional<std::string>(bus) : std::nullopt;
    ctx->state = IVY_CTX_RUNNING;
    return IVY_OK;
}

int IvyContextStop(IvyContext* ctx) {
    ctx->state = IVY_CTX_STOPPED;
    return IVY_OK;
}

int IvyContextDestroy(IvyContext* ctx) {
    assert(!ctx->in_callback);
    if (ctx->state == IVY_CTX_RUNNING)
        IvyContextStop(ctx);
    for (auto* binding : ctx->bindings)
        delete binding;
    delete ctx;
    --live_contexts;
    ++destroyed_contexts;
    return IVY_OK;
}

IvyContextState IvyContextGetState(const IvyContext* ctx) { return ctx->state; }
IvyStatus IvyGetLastError() { return last_error; }









MsgRcvPtr IvyContextBindMsg(IvyContext* ctx, MsgCallback callback, void* data,
                           const char* format, ...) {
    last_error = ctx->state == IVY_CTX_STOPPED ? IVY_ESTOPPED : bind_error;
    if (last_error != IVY_OK)
        return nullptr;
    // Regexps must never be used as C format strings by the wrapper.
    assert(std::strcmp(format, "%s") == 0);
    va_list args;
    va_start(args, format);
    const char* regexp = va_arg(args, const char*);
    auto* binding = new _msg_rcv{callback, data, regexp};
    va_end(args);
    ctx->bindings.push_back(binding);
    return binding;
}

int IvyContextUnbindMsg(IvyContext* ctx, MsgRcvPtr binding) {
    if (ctx->state == IVY_CTX_STOPPED)
        return IVY_ESTOPPED;
    assert(std::find(ctx->bindings.begin(), ctx->bindings.end(), binding) != ctx->bindings.end());
    std::erase(ctx->bindings, binding);
    delete binding;
    ++unbind_count;
    return IVY_OK;
}

MsgRcvPtr IvyContextChangeMsg(IvyContext* ctx, MsgRcvPtr binding, const char* format, ...) {
    last_error = ctx->state == IVY_CTX_STOPPED ? IVY_ESTOPPED : change_error;
    if (last_error != IVY_OK)
        return nullptr;
    assert(std::strcmp(format, "%s") == 0);
    va_list args;
    va_start(args, format);
    const std::string regexp(va_arg(args, const char*));
    va_end(args);
    if (during_change)
        during_change();
    assert(std::find(ctx->bindings.begin(), ctx->bindings.end(), binding) != ctx->bindings.end());
    binding->regexp = regexp;
    return binding;
}

int IvyContextBindDirectMsg(IvyContext* ctx, MsgDirectCallback callback, void* data) {
    if (ctx->state == IVY_CTX_STOPPED)
        return IVY_ESTOPPED;
    if (bind_error != IVY_OK)
        return bind_error;
    ctx->direct = callback;
    ctx->direct_data = data;
    return IVY_OK;
}
}

template<class Operation>
void expect_exception_status(IvyStatus status, Operation operation) {
    try {
        operation();
        assert(false && "expected std::system_error");
    } catch (const std::system_error& error) {
        assert(error.code() == ivy::make_error_code(status));
        assert(std::string(error.code().category().name()) == "ivy");
    }
}

void expect_error(IvyStatus status, const std::expected<void, std::error_code>& result) {
    assert(!result);
    assert(result.error() == ivy::make_error_code(status));
    assert(std::string(result.error().category().name()) == "ivy");
}

template<class Operation>
void expect_invalid_argument(Operation operation) {
    try {
        operation();
        assert(false && "expected std::invalid_argument");
    } catch (const std::invalid_argument&) {
    }
}

void application_event(IvyContext* ctx, IvyClientPtr peer, IvyApplicationEvent event) {
    assert(ctx->application);
    ctx->in_callback = true;
    ctx->application(peer, ctx->application_data, event);
    ctx->in_callback = false;
}

void die_event(IvyContext* ctx, IvyClientPtr peer, int id) {
    assert(ctx->die);
    ctx->in_callback = true;
    ctx->die(peer, ctx->die_data, id);
    ctx->in_callback = false;
}

void strings_and_lifecycle() {
    std::string name = "receiver-unused";
    std::string ready = "READY-unused";
    ivy::Bus bus(std::string_view(name).substr(0, 8),
                 std::string_view(ready).substr(0, 5));
    name.assign("changed");
    ready.assign("changed");
    auto* ctx = bus.native_handle();
    assert(ctx->name == "receiver");
    assert(ctx->ready == "READY");
    assert(!ctx->application && !ctx->die);
    assert(bus.state() == IVY_CTX_CREATED);

    std::string address = "127:2010-unused";
    assert(bus.start(std::string_view(address).substr(0, 8)));
    address.clear();
    assert(ctx->address == "127:2010");
    assert(bus.state() == IVY_CTX_RUNNING);
    expect_error(IVY_ESTATE, bus.start());
    expect_error(IVY_ESTATE, bus.start("127:2010"));
    bus.stop();
    bus.stop();
    assert(bus.state() == IVY_CTX_STOPPED);
    expect_error(IVY_ESTATE, bus.start());
    expect_error(IVY_ESTATE, bus.start("127:2010"));

    ivy::Bus absent("absent");
    ivy::Bus empty("empty", "");
    assert(!absent.native_handle()->ready);
    assert(empty.native_handle()->ready && empty.native_handle()->ready->empty());
    assert(absent.start());
    assert(empty.start(std::string_view{}));
    assert(!absent.native_handle()->address);
    assert(empty.native_handle()->address && empty.native_handle()->address->empty());

    expect_invalid_argument([] { ivy::Bus invalid(std::string_view("a\0b", 3)); });
    expect_invalid_argument([] { ivy::Bus invalid("app", std::string_view("a\0b", 3)); });
    ivy::Bus invalid_address("app");
    expect_error(IVY_EINVAL, invalid_address.start(std::string_view("a\0b", 3)));
    assert(invalid_address.state() == IVY_CTX_CREATED);
    assert(invalid_address.start("127:2010"));
}

void move_only_callbacks() {
    _clnt_lst_dict peer;
    int application_result = 0;
    int die_result = 0;
    ivy::Bus original("movable", std::nullopt,
        [value = std::make_unique<int>(40), &peer, &application_result]
        (IvyClientPtr app, IvyApplicationEvent event) mutable {
            assert(app == &peer);
            assert(event == IvyApplicationConnected);
            application_result = ++*value;
        },
        [value = std::make_unique<int>(7), &peer, &die_result](IvyClientPtr app, int id) {
            assert(app == &peer);
            die_result = *value + id;
        });
    auto* context = original.native_handle();
    void* callback_data = context->application_data;
    std::vector<ivy::Bus> buses;
    buses.reserve(1);
    buses.push_back(std::move(original));
    buses.emplace_back("force reallocation");
    assert(!original.native_handle());
    assert(original.state() == IVY_CTX_DESTROYED);
    original.stop();
    original.rethrow_callback_exception();
    expect_error(IVY_ESTATE, original.start());
    expect_error(IVY_ESTATE, original.start("127:2010"));
    assert(buses[0].native_handle() == context);
    assert(context->application_data == callback_data);
    application_event(context, &peer, IvyApplicationConnected);
    assert(application_result == 41);

    ivy::Bus target("replace me");
    assert(target.start());
    int destroyed_before = destroyed_contexts;
    target = std::move(buses[0]);
    assert(destroyed_contexts == destroyed_before + 1);
    assert(target.native_handle() == context);
    assert(!buses[0].native_handle());
    application_event(context, &peer, IvyApplicationConnected);
    die_event(context, &peer, 5);
    assert(application_result == 42 && die_result == 12);
}

void errors_and_callback_exceptions() {
    const int before = live_contexts;
    create_error = IVY_ENOMEM;
    expect_exception_status(IVY_ENOMEM, [] { ivy::Bus failed("failed"); });
    create_error = IVY_OK;
    assert(live_contexts == before);

    ivy::Bus retry("retry");
    for (IvyStatus error : {IVY_EIO, IVY_ENOMEM, IVY_EINVAL, IVY_ESTOPPED}) {
        start_error = error;
        expect_error(error, retry.start());
        expect_error(error, retry.start("127:2010"));
        assert(retry.state() == IVY_CTX_CREATED);
    }
    start_error = IVY_OK;
    assert(retry.start());

    _clnt_lst_dict peer;
    ivy::Bus throwing("throwing", std::nullopt,
        [](IvyClientPtr, IvyApplicationEvent) { throw std::runtime_error("application failed"); },
        [](IvyClientPtr, int id) { throw id; });
    assert(throwing.start());
    application_event(throwing.native_handle(), &peer, IvyApplicationConnected);
    die_event(throwing.native_handle(), &peer, 42);
    assert(throwing.state() == IVY_CTX_STOPPED);
    try {
        throwing.rethrow_callback_exception();
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "application failed");
    }
    throwing.rethrow_callback_exception();
    die_event(throwing.native_handle(), &peer, 77);
    try {
        throwing.rethrow_callback_exception();
        assert(false);
    } catch (int id) {
        assert(id == 77);
    }
    throwing.rethrow_callback_exception();
}

void subscriptions_and_formats() {
    ivy::Bus bus("bindings");
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    std::vector<std::string> received;
    int calls = 0;
    auto callback = [count = std::make_unique<int>(0), &calls, &received, &peer]
        (IvyClientPtr app, std::span<const std::string_view> args) {
            assert(app == &peer);
            calls = ++*count;
            received.assign(args.begin(), args.end());
        };
    std::string pattern = R"(^TRACK ([0-9]{2}) 100%$-suffix)";
    auto result = bus.bind(std::move(callback), std::string_view(std::string_view(pattern).substr(0, pattern.size() - 7)));
    assert(result && result->is_bound());
    auto* binding = ctx->bindings.back();
    assert(binding->regexp == R"(^TRACK ([0-9]{2}) 100%$)");
    const auto pending_callback = binding->callback;
    void* pending_data = binding->data;
    char value[] = "42";
    char* arguments[] = {value};
    pending_callback(&peer, pending_data, 1, arguments);
    assert(calls == 1 && received == std::vector<std::string>{"42"});
    pending_callback(&peer, pending_data, 0, nullptr);
    assert(calls == 2 && received.empty());

    auto ignore = [](IvyClientPtr, std::span<const std::string_view>) {};
    auto formatted = bus.bind(ignore, R"(^TRACK {} ([0-9]{{2}}) 100%$)", 42);
    assert(formatted);
    assert(ctx->bindings.back()->regexp == R"(^TRACK 42 ([0-9]{2}) 100%$)");
    auto braces = bus.bind(ignore, "{}");
    assert(braces && ctx->bindings.back()->regexp == "{}");
    const auto count = ctx->bindings.size();
    auto invalid_format = bus.bind(ignore, "^{:{}d}", 42, -1);
    assert(!invalid_format && invalid_format.error() == ivy::make_error_code(IVY_EINVAL));
    assert(!bus.bind(ignore, std::string_view(std::string_view("a\0b", 3))));
    assert(!bus.bind(ivy::Bus::MessageCallback{}, "^regexp"));
    assert(!bus.bind(ivy::Bus::DirectCallback{}));
    assert(ctx->bindings.size() == count);

    bind_error = IVY_ENOMEM;
    auto failed = bus.bind(ignore, "^failed");
    bind_error = IVY_OK;
    assert(!failed && failed.error() == ivy::make_error_code(IVY_ENOMEM));
    assert(ctx->bindings.size() == count);
    ivy::Bus moved(std::move(bus));
    assert(result->is_bound());
    auto from_moved = bus.bind(ignore, "^failed");
    assert(!from_moved && from_moved.error() == ivy::make_error_code(IVY_ESTATE));

    const int before = unbind_count;
    ivy::Subscription subscription(std::move(*result));
    assert(!result->is_bound());
    assert(subscription.unbind());
    assert(subscription.unbind());
    assert(unbind_count == before + 1);
    pending_callback(&peer, pending_data, 1, arguments);
    assert(calls == 2); // C may invoke a callback copied before unbind.
    moved.stop();
    assert(!formatted->is_bound());
    assert(formatted->unbind());
    auto after_stop = moved.bind(ignore, "^failed");
    assert(!after_stop && after_stop.error() == ivy::make_error_code(IVY_ESTOPPED));
}

void direct_subscriptions() {
    ivy::Bus bus("direct");
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    int old_calls = 0;
    int new_calls = 0;
    auto old = bus.bind([value = std::make_unique<int>(7), &old_calls, &peer]
                       (IvyClientPtr app, int id, std::string_view message) {
        assert(app == &peer && id == *value && message == "payload");
        ++old_calls;
    });
    assert(old);
    const auto old_callback = ctx->direct;
    void* old_data = ctx->direct_data;
    char message[] = "payload";
    old_callback(&peer, old_data, 7, message);
    assert(old_calls == 1);

    auto replacement = bus.bind([&](IvyClientPtr, int id, std::string_view text) {
        assert(id == 8 && text == "payload");
        ++new_calls;
    });
    assert(replacement && replacement->is_bound() && !old->is_bound());
    old_callback(&peer, old_data, 7, message);
    assert(old_calls == 1);
    assert(old->unbind());
    ctx->direct(&peer, ctx->direct_data, 8, message);
    assert(new_calls == 1);
    bind_error = IVY_EIO;
    auto failed = bus.bind([](IvyClientPtr, int, std::string_view) {});
    bind_error = IVY_OK;
    assert(!failed && replacement->is_bound());
    ctx->direct(&peer, ctx->direct_data, 8, message);
    assert(new_calls == 2);
    assert(replacement->unbind());
    assert(!ctx->direct);
}

void subscription_lifetimes() {
    ivy::Subscription survivor;
    std::weak_ptr<int> weak_capture;
    assert(!survivor.is_bound() && survivor.unbind());
    {
        ivy::Bus bus("lifetimes");
        auto capture = std::make_shared<int>(42);
        weak_capture = capture;
        auto result = bus.bind([capture = std::move(capture)](IvyClientPtr, auto) {}, "^first");
        assert(result);
        survivor = std::move(*result);
        const int before = unbind_count;
        {
            auto scoped = bus.bind([](IvyClientPtr, auto) {}, "^scoped");
            assert(scoped);
        }
        assert(unbind_count == before + 1);
        assert(!weak_capture.expired());
    }
    assert(!survivor.is_bound() && survivor.unbind());
    assert(weak_capture.expired());

    ivy::Bus bus("self-unbind");
    std::optional<ivy::Subscription> self;
    auto capture = std::make_shared<int>(42);
    weak_capture = capture;
    auto result = bus.bind([capture = std::move(capture), &self, &weak_capture](IvyClientPtr, auto) {
        self.reset();
        assert(!weak_capture.expired() && *capture == 42);
    }, "^self");
    assert(result);
    self.emplace(std::move(*result));
    auto* binding = bus.native_handle()->bindings.back();
    const auto callback = binding->callback;
    void* data = binding->data;
    callback(nullptr, data, 0, nullptr);
    assert(!self && weak_capture.expired());
    callback(nullptr, data, 0, nullptr);
}

void concurrent_unbind() {
    ivy::Bus bus("concurrent unbind");
    std::latch entered(1), release(1);
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak_capture = capture;
    auto result = bus.bind([capture = std::move(capture), &entered, &release](IvyClientPtr, auto) {
        entered.count_down();
        release.wait();
        assert(*capture == 42);
    }, "^blocked");
    assert(result);
    const auto callback = bus.native_handle()->bindings.back()->callback;
    void* data = bus.native_handle()->bindings.back()->data;
    std::thread worker([&] { callback(nullptr, data, 0, nullptr); });
    entered.wait();
    assert(result->unbind());
    assert(!result->is_bound() && !weak_capture.expired());
    release.count_down();
    worker.join();
    assert(weak_capture.expired());
    callback(nullptr, data, 0, nullptr);
}

void subscription_exceptions() {
    ivy::Bus messages("message exception");
    auto subscription = messages.bind([](IvyClientPtr, auto) { throw 41; }, "^exception");
    assert(subscription);
    auto* binding = messages.native_handle()->bindings.back();
    binding->callback(nullptr, binding->data, 0, nullptr);
    assert(messages.state() == IVY_CTX_STOPPED);
    try { messages.rethrow_callback_exception(); assert(false); }
    catch (int value) { assert(value == 41); }

    ivy::Bus direct("direct exception");
    auto direct_subscription = direct.bind([](IvyClientPtr, int, std::string_view) { throw 42; });
    assert(direct_subscription);
    direct.native_handle()->direct(nullptr, direct.native_handle()->direct_data, 0, nullptr);
    assert(direct.state() == IVY_CTX_STOPPED);
    try { direct.rethrow_callback_exception(); assert(false); }
    catch (int value) { assert(value == 42); }
}

void change_preserves_subscription() {
    ivy::Subscription empty;
    expect_error(IVY_ESTATE, empty.change("^pattern"));
    ivy::Subscription survivor;
    {
        ivy::Bus bus("change");
        int calls = 0;
        auto result = bus.bind([count = std::make_unique<int>(0), &calls](IvyClientPtr, auto) {
            calls = ++*count;
        }, "^OLD$");
        assert(result);
        auto* ctx = bus.native_handle();
        auto* original = ctx->bindings.back();
        const auto callback = original->callback;
        void* data = original->data;
        callback(nullptr, data, 0, nullptr);
        assert(calls == 1);

        const std::string pattern = R"(^NEW [0-9]{2} 100%$-suffix)";
        assert(result->change(std::string_view(std::string_view(pattern).substr(0, pattern.size() - 7))));
        assert(ctx->bindings.size() == 1 && ctx->bindings.back() == original);
        assert(original->regexp == R"(^NEW [0-9]{2} 100%$)");
        assert(original->callback == callback && original->data == data);
        callback(nullptr, data, 0, nullptr);
        assert(calls == 2);

        assert(result->change(R"(^NEW {} [0-9]{{2}} 100%$)", 42));
        const auto previous_pattern = original->regexp;
        assert(previous_pattern == R"(^NEW 42 [0-9]{2} 100%$)");
        change_error = IVY_ENOMEM;
        expect_error(IVY_ENOMEM, result->change("^failed"));
        change_error = IVY_OK;
        expect_error(IVY_EINVAL, result->change(std::string_view(std::string_view("a\0b", 3))));
        expect_error(IVY_EINVAL, result->change("^{:{}d}", 42, -1));
        assert(original->regexp == previous_pattern && result->is_bound());
        callback(nullptr, data, 0, nullptr);
        assert(calls == 3);

        survivor = std::move(*result);
        expect_error(IVY_ESTATE, result->change("^moved-from"));
        assert(survivor.change("^MOVED$"));
        bus.stop();
        expect_error(IVY_ESTOPPED, survivor.change("^stopped"));
    }
    expect_error(IVY_ESTATE, survivor.change("^destroyed bus"));
    assert(!survivor.is_bound() && survivor.unbind());
}

void change_and_unbind() {
    ivy::Bus bus("change and unbind");
    auto result = bus.bind([](IvyClientPtr, auto) {}, "^initial");
    assert(result);
    const int before = unbind_count;
    during_change = [&] {
        assert(result->unbind());
        assert(!result->is_bound());
        assert(unbind_count == before && bus.native_handle()->bindings.size() == 1);
    };
    expect_error(IVY_ESTATE, result->change("^cancelled reentrantly"));
    during_change = nullptr;
    assert(unbind_count == before + 1 && bus.native_handle()->bindings.empty());
    expect_error(IVY_ESTATE, result->change("^unbound"));
    assert(result->unbind());

    auto concurrent = bus.bind([](IvyClientPtr, auto) {}, "^concurrent");
    assert(concurrent);
    std::latch entered(1), release(1);
    during_change = [&] { entered.count_down(); release.wait(); };
    std::expected<void, std::error_code> changed;
    std::thread worker([&] { changed = concurrent->change("^while unbinding"); });
    entered.wait();
    assert(concurrent->unbind() && !concurrent->is_bound());
    assert(bus.native_handle()->bindings.size() == 1);
    release.count_down();
    worker.join();
    during_change = nullptr;
    expect_error(IVY_ESTATE, changed);
    assert(bus.native_handle()->bindings.empty());

    // Destruction from a C callback dispatched by change must also defer removal.
    std::optional<ivy::Subscription> self;
    auto self_result = bus.bind([](IvyClientPtr, auto) {}, "^self");
    assert(self_result);
    self.emplace(std::move(*self_result));
    during_change = [&] { self.reset(); };
    expect_error(IVY_ESTATE, self->change("^destroyed during change"));
    during_change = nullptr;
    assert(!self && bus.native_handle()->bindings.empty());
}

void direct_token_lifetimes() {
    ivy::DirectSubscription survivor;
    assert(!survivor.is_bound() && survivor.unbind());
    std::weak_ptr<int> weak_capture;
    {
        ivy::Bus first("first direct"), second("second direct");
        auto capture = std::make_shared<int>(42);
        weak_capture = capture;
        auto a = first.bind([](IvyClientPtr, int, std::string_view) {});
        auto b = second.bind([capture = std::move(capture)](IvyClientPtr, int, std::string_view) {});
        assert(a && b);
        *a = std::move(*b);
        assert(!b->is_bound() && a->is_bound());
        assert(!first.native_handle()->direct && second.native_handle()->direct);
        survivor = std::move(*a);
        assert(!a->is_bound() && survivor.is_bound());
    }
    assert(!survivor.is_bound() && survivor.unbind());
    assert(weak_capture.expired());
}



template<class T>
concept HasChange = requires(T& subscription) { subscription.change("^regexp"); };



int main() {
    static_assert(HasChange<ivy::Subscription>);
    static_assert(!HasChange<ivy::DirectSubscription>);
    static_assert(!std::is_convertible_v<ivy::DirectSubscription, ivy::Subscription>);
    static_assert(!std::is_copy_constructible_v<ivy::DirectSubscription>);
    static_assert(!std::is_copy_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
        std::declval<ivy::Bus::MessageCallback>(), "^regexp")),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
        std::declval<ivy::Bus::MessageCallback>(), "^{}", 42)),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
        std::declval<ivy::Bus::DirectCallback>())),
        std::expected<ivy::DirectSubscription, std::error_code>>);
    static_assert(!std::is_copy_constructible_v<ivy::Subscription>);
    static_assert(!std::is_copy_assignable_v<ivy::Subscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::Subscription>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::Subscription>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().start()),
                                 std::expected<void, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().start("127:2010")),
                                 std::expected<void, std::error_code>>);
    static_assert(noexcept(std::declval<ivy::Bus&>().start()));
    static_assert(noexcept(std::declval<ivy::Bus&>().start("127:2010")));
    static_assert(!std::is_copy_constructible_v<ivy::Bus>);
    static_assert(!std::is_copy_assignable_v<ivy::Bus>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::Bus>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::Bus>);
    static_assert(std::is_nothrow_destructible_v<ivy::Bus>);
    strings_and_lifecycle();
    move_only_callbacks();
    errors_and_callback_exceptions();
    subscriptions_and_formats();
    direct_subscriptions();
    subscription_lifetimes();
    concurrent_unbind();
    subscription_exceptions();
    change_preserves_subscription();
    change_and_unbind();
    direct_token_lifetimes();
    assert(live_contexts == 0);
    std::cout << "C++ bus boundary tests passed\n";
}
