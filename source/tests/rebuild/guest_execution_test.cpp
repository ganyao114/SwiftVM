#include "runtime/execution/fault.h"
#include "runtime/execution/guest_execution.h"
#include "tests/support/check.h"

#include <chrono>
#include <future>

namespace {
void ExecutionOwnership() {
    namespace execution = swift::runtime::execution;
    using namespace std::chrono_literals;
    execution::FaultContext context;
    execution::GuestState state;
    std::promise<void> attempting, entered;
    auto attempted = attempting.get_future(), acquired = entered.get_future();
    std::future<void> contender;
    bool blocked{};
    {
        execution::FaultScope registration{context, state};
        // A software atomic reacquires the execution owner's lock on this
        // thread; another participant must wait for the whole guest run.
        {
            execution::GuestExecutionLock commit;
        }
        contender = std::async(std::launch::async, [&] {
            attempting.set_value();
            execution::GuestExecutionLock owner;
            entered.set_value();
        });
        attempted.wait();
        blocked = acquired.wait_for(20ms) == std::future_status::timeout;
    }
    contender.get();
    swift::tests::Check(blocked && context.restore_error == 0,
                        "guest execution excludes software commits until fault scope teardown");
}
}  // namespace

TEST_CASE("Guest execution owns the atomic lock", "[compile][execution][atomic]") {
    CHECK_NOTHROW(ExecutionOwnership());
}
