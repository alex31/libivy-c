#include "ivy.hpp"

#include <cassert>
#include <iostream>
#include <string>

ivy::Bus require_bus(ivy::Bus::CreateResult result) {
    assert(result);
    return std::move(*result);
}

void expect_status(const char* pattern, IvyStatus status) {
    assert(IvyValidateAnchoredRegexp(pattern) == status);
    assert(IvyGetLastError() == status);
}

int main() {
    expect_status(nullptr, IVY_EINVAL);
    expect_status("", IVY_EUNANCHORED);
    expect_status("FOO", IVY_EUNANCHORED);
    expect_status("\\AFOO", IVY_EUNANCHORED); // The default API requires a literal leading ^.
    expect_status("^FOO (.*)$", IVY_OK);
    expect_status("^FOO|BAR", IVY_EUNANCHORED);
    expect_status("^FOO|^BAR", IVY_OK);
    expect_status("^(?:FOO|BAR)", IVY_OK);
    expect_status("^FOO|(?m)^BAR", IVY_EUNANCHORED);
    expect_status("^(", IVY_EINVAL);
    expect_status("^RANGE (?I-10#20i)$", IVY_OK);
    expect_status("^RANGE (?I-10#20)$", IVY_OK);
    expect_status("^RANGE (?I20#25f) (?I2#1i)$", IVY_OK);
    expect_status("^RANGE (?Ibad)", IVY_EINVAL);
    expect_status("^RANGE (?I1#2", IVY_EINVAL);
    expect_status("^RANGE (?I1#2z)", IVY_EINVAL);
    expect_status("^RANGE (?I999999999999999999999999#2)", IVY_EINVAL);
    std::string large = "^" + std::string(10000, 'X') + "(?I1#2i)$";
    expect_status(large.c_str(), IVY_OK);

    auto bus = require_bus(ivy::Bus::create("anchoring"));
    auto callback = [](IvyClientPtr, auto) {};
    auto valid = bus.bind(callback, R"(^TRACK ([0-9]{2}) 100%$)");
    assert(valid);
    auto alternative = bus.bind(callback, "^FOO|BAR");
    assert(!alternative && alternative.error() == ivy::make_error_code(IVY_EUNANCHORED));
    assert(alternative.error().message() == "regexp must start with '^' and be anchored");
    auto injected = bus.bind(callback, "^FOO {}", "X|BAR");
    assert(!injected && injected.error() == ivy::make_error_code(IVY_EUNANCHORED));
    auto interval = bus.bind(callback, "^RANGE (?I{}#{}i)$", -10, 20);
    assert(interval);
    auto malformed_interval = bus.bind(callback, "^RANGE (?Ibad)");
    assert(!malformed_interval && malformed_interval.error() == ivy::make_error_code(IVY_EINVAL));
    auto invalid_change = valid->change("^FOO|BAR");
    assert(!invalid_change && invalid_change.error() == ivy::make_error_code(IVY_EUNANCHORED));
    assert(valid->is_bound());
    std::string dynamic = "^DYNAMIC ([0-9]{2})$";
    assert(valid->change(ivy::runtime_regexp(dynamic)));
    auto dynamic_missing = bus.bind(callback, ivy::runtime_regexp("DYNAMIC"));
    assert(!dynamic_missing && dynamic_missing.error() == ivy::make_error_code(IVY_EUNANCHORED));
    auto invalid_syntax = bus.bind(callback, "^(");
    assert(!invalid_syntax && invalid_syntax.error() == ivy::make_error_code(IVY_EINVAL));

    auto unanchored = bus.bind_unanchored(callback, "FOO|BAR");
    assert(unanchored);
    auto malformed_unanchored = bus.bind_unanchored(callback, "(?Ibad)");
    assert(!malformed_unanchored && malformed_unanchored.error() == ivy::make_error_code(IVY_EINVAL));
    auto malformed_change = unanchored->change_unanchored("(?I1#2");
    assert(!malformed_change && unanchored->is_bound());
    assert(unanchored->change_unanchored("{} (.*)", "BAR"));
    assert(unanchored->change("^NOW_ANCHORED (.*)"));
    assert(valid->change_unanchored("ANYWHERE"));
    std::cout << "PCRE2 and Ivy expansion anchoring checks passed\n";
}
