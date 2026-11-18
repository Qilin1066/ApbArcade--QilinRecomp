#include <qilin/t11.hpp>

#include <iostream>

namespace {

using qilin::t11::Flags;
using qilin::t11::StopReason;
namespace ops = qilin::t11::ops;

constexpr bool test_subtraction() {
    Flags flags{};
    const auto result = ops::subtract(flags, 0, 1);
    if (result != 0xffff || !flags.n || flags.z || flags.v || !flags.c) {
        return false;
    }
    return ops::subtract(flags, 0x8000, 1) == 0x7fff &&
           !flags.n && !flags.z && flags.v && !flags.c;
}

constexpr bool test_decrement_and_test() {
    Flags flags{.c = true};
    if (ops::decrement(flags, 0x8000) != 0x7fff || !flags.v || !flags.c) {
        return false;
    }
    ops::test(flags, 0);
    return !flags.n && flags.z && !flags.v && !flags.c;
}

static_assert(test_subtraction());
static_assert(test_decrement_and_test());

} // namespace

int main() {
    qilin::t11::State complete;
    const auto halt = qilin::t11::run(complete, 100);
    if (halt.reason != StopReason::halt || halt.address != 0x022a ||
        complete.r[0] != 0 || complete.r[1] != 6 || complete.r[2] != 0 ||
        complete.r[3] != 0x8000 || complete.r[4] != 0 ||
        !complete.flags.n || complete.flags.z || !complete.flags.v || !complete.flags.c) {
        std::cerr << "Unexpected synthetic T-11 result.\n";
        return 1;
    }

    qilin::t11::State limited;
    const auto budget = qilin::t11::run(limited, 5);
    if (budget.reason != StopReason::budget || budget.address != 0x0208 ||
        limited.r[0] != 2 || limited.r[1] != 5) {
        std::cerr << "Unexpected instruction-budget result.\n";
        return 1;
    }

    std::cout << "Synthetic T-11 program ran as precompiled host code.\n";
}
