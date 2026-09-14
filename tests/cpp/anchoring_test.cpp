#include "ivy.h"
#include <cassert>
#include <iostream>

int main() {
    assert(IvyValidateAnchoredRegexp("^TRACK ([0-9]{2})$") == IVY_OK);
    assert(IvyValidateAnchoredRegexp("TRACK (.*)") == IVY_EUNANCHORED);
    assert(IvyValidateAnchoredRegexp("^TRACK|OTHER") == IVY_EUNANCHORED);
    assert(IvyValidateAnchoredRegexp("^(") == IVY_EINVAL);
    assert(IvyValidateAnchoredRegexp("^RANGE (?I1#12i)$") == IVY_OK);
    assert(IvyValidateAnchoredRegexp(nullptr) == IVY_EINVAL);
    std::cout << "C regexp anchoring and interval checks passed\n";
}
