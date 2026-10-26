// Authored infrastructure regression using the ACTUAL runtime/context/scheduler.
// No original game execution, registered test game bodies or kernel ABI proof.
#pragma once
#include "runtime/ee_scheduler.h"
#include <memory>
#include <stdexcept>
#include <utility>

inline int armageddonInvocationRegression()
{
    // Authored positive word values; explicit fixture setup/observation in the
    // actual 128-bit context, without importing unrelated translation macros.
    const auto setWord = [](R5900Context &context, unsigned reg, uint32_t value)
    {
        std::memset(&context.r[reg], 0, sizeof(context.r[reg]));
        std::memcpy(&context.r[reg], &value, sizeof(value));
    };
    const auto word = [](const R5900Context &context, unsigned reg)
    {
        uint32_t value;
        std::memcpy(&value, &context.r[reg], sizeof(value));
        return value;
    };
    using Kind = PS2Runtime::GuestBranchKind;
    struct Case
    {
        const char *name;
        Kind kind;
        uint32_t target;
        bool invocation;
        bool foreignContext;
        bool expectStop;
    };
    const Case cases[] = {
        {"active_invocation_return", Kind::Return, 0, true, false, false},
        {"main_return_zero", Kind::Return, 0, false, false, true},
        {"foreign_invocation_context", Kind::Return, 0, true, true, true},
        {"invocation_null_direct_call", Kind::DirectCall, 0, true, false, true},
        {"invocation_null_indirect_call", Kind::IndirectCall, 0, true, false, true},
        {"invocation_null_direct_jump", Kind::DirectJump, 0, true, false, true},
        {"invocation_null_indirect_jump", Kind::IndirectJump, 0, true, false, true},
        {"invocation_missing_return", Kind::Return, 0x0badf004u, true, false, true},
    };
    for (const auto &test : cases)
    {
        auto runtime = std::make_unique<PS2Runtime>();
        if (!runtime->initializeHeadless())
            throw std::runtime_error("Regression runtime initialization failed");
        runtime->setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        auto *rdram = runtime->memory().getRDRAM();
        R5900Context initial{};
        initial.pc = 0x00100008u;
        setWord(initial, 31, 0x00100220u);
        auto &scheduler = runtime->eeScheduler();
        scheduler.bindMainContextForSyscall(initial, rdram);
        bool completionCalled = false;
        if (test.invocation)
        {
            GuestInvocation child{};
            child.kind = GuestInvocationKind::SyscallOverride;
            child.tag = 0x83;
            child.context = *scheduler.currentContext();
            setWord(child.context, 31, 0u);
            setWord(child.context, 2, 0x12345678u);
            if (!test.expectStop)
                child.onComplete = [&](const R5900Context &completed, R5900Context &parent)
                {
                    completionCalled = true;
                    parent.r[2] = completed.r[2];
                    // Stop before the scheduler can invoke any game function.
                    runtime->requestStop();
                };
            try
            {
                scheduler.invokeCurrent(std::move(child));
            }
            catch (const EeDispatcherTransfer &)
            {
                // Actual scheduler transfers control after pushing the context.
            }
        }
        R5900Context foreign = *scheduler.currentContext();
        R5900Context *context = test.foreignContext ? &foreign : scheduler.currentContext();
        const bool inlineContinuation = runtime->dispatchGuestBranch(
            rdram, context, test.target, 0x00116b78u, 0u, test.kind, test.name);
        if (inlineContinuation || runtime->isStopRequested() != test.expectStop ||
            context->pc != test.target || runtime->hasFunction(0u))
            throw std::runtime_error(std::string("Invocation return regression failed: ")+test.name);
        std::cout << "PASS " << test.name << '\n';
        if (!test.expectStop)
        {
            scheduler.run();
            const auto *parent = scheduler.currentContext();
            const auto *owner = scheduler.currentThread();
            if (!completionCalled || !owner || !owner->invocations.empty() || !parent ||
                parent->pc != 0x00100008u || word(*parent,31) != 0x00100220u ||
                word(*parent,2) != 0x12345678u)
                throw std::runtime_error("Real scheduler invocation completion failed");
            std::cout << "PASS scheduler_pop_callback_parent_resume\n";
        }
    }
    std::cout << "actual-runtime invocation boundary: 8 cases plus completion passed; game_executed=false\n";
    return 0;
}
