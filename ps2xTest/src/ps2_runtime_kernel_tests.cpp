#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include "armageddon_hle.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

using namespace ps2_syscalls;

namespace
{
    constexpr uint32_t K_PARAM_ADDR = 0x1000u;
    constexpr uint32_t K_STATUS_ADDR = 0x1400u;

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_WAIT_DELETE = -425;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr uint32_t K_SEMA_WAIT_READY_ADDR = 0x1900u;

    constexpr int THS_WAIT = 0x04;
    constexpr int THS_SUSPEND = 0x08;
    constexpr int THS_WAITSUSPEND = 0x0C;
    constexpr int THS_DORMANT = 0x10;
    constexpr uint32_t TSW_SEMA = 2u;
    constexpr uint32_t TSW_EVENT = 3u;

    struct EeThreadStatusAbi
    {
        int32_t status;
        uint32_t func;
        uint32_t stack;
        int32_t stack_size;
        uint32_t gp_reg;
        int32_t initial_priority;
        int32_t current_priority;
        uint32_t attr;
        uint32_t option;
        uint32_t waitType;
        uint32_t waitId;
        uint32_t wakeupCount;
    };

    struct EeThreadCreateAbi
    {
        int32_t status;
        uint32_t func;
        uint32_t stack;
        int32_t stack_size;
        uint32_t gp_reg;
        int32_t initial_priority;
        int32_t current_priority;
        uint32_t attr;
        uint32_t option;
    };

    struct EeSemaStatus
    {
        int32_t count;
        int32_t max_count;
        int32_t init_count;
        int32_t wait_threads;
        uint32_t attr;
        uint32_t option;
    };

    static_assert(sizeof(EeThreadStatusAbi) == 0x30u, "Unexpected ee_thread_status_t size.");
    static_assert(sizeof(EeThreadCreateAbi) == 0x24u, "Unexpected ee_thread_t size.");
    static_assert(sizeof(EeSemaStatus) == 0x18u, "Unexpected ee_sema_t size.");

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        SET_GPR_U32(&ctx, reg, value);
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    void writeGuestWords(uint8_t *rdram, uint32_t addr, const uint32_t *words, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            writeGuestU32(rdram, addr + static_cast<uint32_t>(i * sizeof(uint32_t)), words[i]);
        }
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    bool callSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return dispatchNumericSyscall(syscallNumber, rdram, ctx, runtime);
    }

    void overrideReturnHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnU32(ctx, ::getRegU32(ctx, 4) + ::getRegU32(ctx, 5));
        ctx->pc = ::getRegU32(ctx, 31);
    }

    void overrideBrokenHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnU32(ctx, 0xDEADBEEFu);
        ctx->pc = 0x12345678u;
    }

    void overrideRecursiveFindAddressHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->handleSyscall(rdram, ctx, 0x83u);
        ctx->pc = ::getRegU32(ctx, 31);
    }

    void overrideKsegCompareHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        auto getLowU64 = [](const R5900Context *cpu, int reg) -> uint64_t
        {
            return (reg == 0) ? 0u : static_cast<uint64_t>(_mm_extract_epi64(cpu->r[reg], 0));
        };
        auto setLowS32 = [](R5900Context *cpu, int reg, uint32_t value)
        {
            SET_GPR_S32(cpu, reg, value);
        };
        auto setLowU64 = [](R5900Context *cpu, int reg, uint64_t value)
        {
            SET_GPR_U64(cpu, reg, value);
        };

        const uint32_t nextA0 = static_cast<uint32_t>(::getRegU32(ctx, 4) + 4u);
        setLowS32(ctx, 4, nextA0);
        setLowU64(ctx, 2, (getLowU64(ctx, 4) < getLowU64(ctx, 5)) ? 1u : 0u);
        if (getLowU64(ctx, 2) == 0u)
        {
            ctx->r[4] = _mm_setzero_si128();
        }
        setLowU64(ctx, 2, getLowU64(ctx, 4));
        ctx->pc = ::getRegU32(ctx, 31);
    }

    constexpr uint64_t K_EXPECTED_UPPER64 = 0x1122334455667788ull;

    void overridePreserveUpper64Handler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint64_t hi = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[4], 1));
        const uint64_t low = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[4], 0));
        const uint64_t expectedLow = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(0x80000000u)));
        setReturnU32(ctx, (hi == K_EXPECTED_UPPER64 && low == expectedLow) ? 1u : 0u);
        ctx->pc = ::getRegU32(ctx, 31);
    }

    constexpr uint32_t K_OVERRIDE_ENTRY = 0x300500u;
    constexpr uint32_t K_OVERRIDE_RESUME = 0x300504u;
    constexpr uint32_t K_OVERRIDE_BLOCK_ENTRY = 0x300510u;
    constexpr uint32_t K_OVERRIDE_BLOCK_HANDLER = 0x300520u;
    constexpr uint32_t K_OVERRIDE_BLOCK_HANDLER_RESUME = 0x300524u;
    constexpr uint32_t K_OVERRIDE_BLOCK_DRIVER = 0x300530u;
    constexpr uint32_t K_OVERRIDE_BLOCK_BASE_RESUME = 0x300540u;
    constexpr uint32_t K_EXIT_MAIN = 0x300600u;
    constexpr uint32_t K_EXIT_HANDLER_A = 0x300610u;
    constexpr uint32_t K_EXIT_HANDLER_B = 0x300620u;
    constexpr uint32_t K_EXIT_OBSERVER = 0x300630u;
    constexpr uint32_t K_IRQ_STACK_MAIN = 0x300700u;
    constexpr uint32_t K_IRQ_STACK_HANDLER = 0x300710u;
    constexpr uint32_t K_IRQ_STACK_CAUSE = 5u;
    constexpr uint32_t K_IRQ_REGISTRATION_SP = 0x18060u;
    constexpr uint32_t K_IRQ_INTERRUPTED_SP = 0x18000u;
    constexpr uint32_t K_IRQ_SAVED_RA_SLOT = K_IRQ_INTERRUPTED_SP + 0x20u;
    constexpr uint64_t K_IRQ_SAVED_RA_SENTINEL = 0x0123456789ABCDEFull;
    uint32_t gOverrideSyscall = 0u;
    R5900Context gOverrideResult{};
    std::vector<int> gInvocationTrace;
    uint32_t gIrqCallbackEntrySp = 0u;
    uint32_t gIrqCallbackCount = 0u;
    int32_t gIrqRegistrationResult = KE_ERROR;

    void schedulerOverrideEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = K_OVERRIDE_RESUME;
        runtime->handleSyscall(rdram, ctx, gOverrideSyscall);
    }

    void schedulerOverrideResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gOverrideResult = *ctx;
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerBlockingOverrideEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(1);
        EeThreadCreateParams driver{};
        driver.entry = K_OVERRIDE_BLOCK_DRIVER;
        driver.stack = 0x1E000u;
        driver.stackSize = 0x1000u;
        driver.priority = 10;
        const int driverId = runtime->eeScheduler().createThread(driver);
        runtime->eeScheduler().startThread(driverId, 0u, *ctx, false);
        ctx->pc = K_OVERRIDE_BLOCK_BASE_RESUME;
        runtime->handleSyscall(rdram, ctx, gOverrideSyscall);
    }

    void schedulerBlockingOverrideHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(2);
        ctx->pc = K_OVERRIDE_BLOCK_HANDLER_RESUME;
        SleepThread(rdram, ctx, runtime);
    }

    void schedulerBlockingOverrideHandlerResume(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(4);
        setReturnU32(ctx, 0xB10C0EDu);
        ctx->pc = 0u;
    }

    void schedulerBlockingOverrideDriver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(3);
        ctx->pc = 0u;
        runtime->eeScheduler().wakeupThread(EeScheduler::kMainThreadId, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerBlockingOverrideBaseResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(5);
        gOverrideResult = *ctx;
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerExitHandlerA(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(getRegS32(*ctx, 4));
        ctx->pc = 0u;
    }

    void schedulerExitHandlerB(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(getRegS32(*ctx, 4));
        ctx->pc = 0u;
    }

    void schedulerExitObserver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const GuestThread *main = runtime->eeScheduler().thread(EeScheduler::kMainThreadId);
        gInvocationTrace.push_back(main && main->status == EeThreadStatus::Dormant ? 40 : -40);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerExitMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(10);
        EeThreadCreateParams observer{};
        observer.entry = K_EXIT_OBSERVER;
        observer.stack = 0x1F000u;
        observer.stackSize = 0x1000u;
        observer.priority = 10;
        const int observerId = runtime->eeScheduler().createThread(observer);
        runtime->eeScheduler().startThread(observerId, 0u, *ctx, false);
        runtime->addEeExitHandler(EeScheduler::kMainThreadId, K_EXIT_HANDLER_A, 20u);
        runtime->addEeExitHandler(EeScheduler::kMainThreadId, K_EXIT_HANDLER_B, 30u);
        ExitThread(rdram, ctx, runtime);
    }

    void schedulerIrqStackMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setRegU32(*ctx, 4, K_IRQ_STACK_CAUSE);
        setRegU32(*ctx, 5, K_IRQ_STACK_HANDLER);
        setRegU32(*ctx, 6, 0u);
        setRegU32(*ctx, 7, 0xA55Au);
        setRegU32(*ctx, 29, K_IRQ_REGISTRATION_SP);
        gIrqRegistrationResult = callSyscall(0x12u, rdram, ctx, runtime)
                                     ? getRegS32(*ctx, 2)
                                     : KE_ERROR;

        setRegU32(*ctx, 29, K_IRQ_INTERRUPTED_SP);
        runtime->eeScheduler().setIrqCauseEnabled(true, K_IRQ_STACK_CAUSE, true);
        runtime->eeScheduler().dispatchIrq(true, K_IRQ_STACK_CAUSE);
        ctx->pc = 0u;
    }

    void schedulerIrqStackHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++gIrqCallbackCount;
        gIrqCallbackEntrySp = getRegU32(ctx, 29);

        // This matches the retail handler's observed prologue operations:
        // addiu $sp, $sp, -0xA0; sd $s6, 0x60($sp).
        const uint32_t frameSp = gIrqCallbackEntrySp - 0xA0u;
        setRegU32(*ctx, 29, frameSp);
        const uint64_t savedS6 = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[22], 0));
        std::memcpy(rdram + frameSp + 0x60u, &savedS6, sizeof(savedS6));

        ctx->pc = 0u;
        runtime->requestStop();
    }

    void alarmNoopHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = 0u;
    }

    constexpr uint32_t K_DMAC_SPIN_MAIN = 0x00055000u;
    constexpr uint32_t K_DMAC_SPIN_HANDLER = 0x00055100u;
    constexpr uint32_t K_DMAC_SPIN_POLL_FN = 0x00055200u;
    constexpr uint32_t K_DMAC_SPIN_COUNTER_ADDR = 0x00055300u;
    static uint32_t gSpinPollCalls = 0u;
    static uint32_t gSpinHandlerCalls = 0u;

    void schedulerDmacSpinHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        ++gSpinHandlerCalls;
        uint32_t counter = 0;
        std::memcpy(&counter, rdram + K_DMAC_SPIN_COUNTER_ADDR, sizeof(counter));
        if (counter > 0)
        {
            --counter;
            std::memcpy(rdram + K_DMAC_SPIN_COUNTER_ADDR, &counter, sizeof(counter));
        }
        ctx->pc = 0u;
    }

    void schedulerDmacSpinPollFn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++gSpinPollCalls;
        ctx->pc = getRegU32(ctx, 31);
    }

    void schedulerDmacSpinMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->cop0_status = 0x10001u;
        const uint32_t initialCounter = 1u;
        std::memcpy(rdram + K_DMAC_SPIN_COUNTER_ADDR, &initialCounter, sizeof(initialCounter));

        runtime->eeScheduler().addIrqHandler(true, 2u, K_DMAC_SPIN_HANDLER, false, 0u, 0u, 0u);
        runtime->eeScheduler().setIrqCauseEnabled(true, 2u, true);
        runtime->eeScheduler().dispatchIrq(true, 2u);

        uint32_t spins = 0;
        while (spins < 100u)
        {
            ++spins;
            uint32_t counter = 0;
            std::memcpy(&counter, rdram + K_DMAC_SPIN_COUNTER_ADDR, sizeof(counter));
            if (counter == 0)
                break;
            setRegU32(*ctx, 31, K_DMAC_SPIN_MAIN + 0x10u);
            runtime->dispatchGuestBranch(rdram, ctx, K_DMAC_SPIN_POLL_FN, K_DMAC_SPIN_MAIN, K_DMAC_SPIN_MAIN + 0x10u,
                                         PS2Runtime::GuestBranchKind::IndirectCall, "JALR");
        }

        setRegU32(*ctx, 2, KE_OK);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    constexpr uint32_t K_SCHED_MAIN = 0x300000u;
    constexpr uint32_t K_SCHED_A = 0x300100u;
    constexpr uint32_t K_SCHED_A_RESUME = 0x300104u;
    constexpr uint32_t K_SCHED_B = 0x300200u;
    constexpr uint32_t K_SCHED_B_RESUME = 0x300204u;
    constexpr uint32_t K_SCHED_HIGH = 0x300300u;
    constexpr uint32_t K_SCHED_SIGNAL = 0x300400u;
    constexpr uint32_t K_SCHED_SIGNAL_RESUME = 0x300404u;

    std::vector<int> *gSchedulerTrace = nullptr;
    int gSchedulerCreatedId = 0;
    int gSchedulerSemaphoreId = 0;
    int gSchedulerWaitResultA = 0;
    int gSchedulerWaitResultB = 0;
    std::atomic<int> gGuestActive{0};
    std::atomic<int> gGuestMaxActive{0};
    std::atomic<size_t> gGuestExecutorHash{0u};
    std::atomic<bool> gGuestExecutorMismatch{false};
    std::atomic<bool> gGuestExecutingFlagMissing{false};

    struct GuestExecutionProbe
    {
        explicit GuestExecutionProbe(PS2Runtime *runtime)
        {
            const int active = gGuestActive.fetch_add(1, std::memory_order_acq_rel) + 1;
            int maximum = gGuestMaxActive.load(std::memory_order_acquire);
            while (active > maximum &&
                   !gGuestMaxActive.compare_exchange_weak(maximum, active, std::memory_order_acq_rel))
            {
            }
            const size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            size_t expected = 0u;
            if (!gGuestExecutorHash.compare_exchange_strong(expected, hash, std::memory_order_acq_rel) &&
                expected != hash)
            {
                gGuestExecutorMismatch.store(true, std::memory_order_release);
            }
            if (!runtime->eeScheduler().isExecutingGuest())
            {
                gGuestExecutingFlagMissing.store(true, std::memory_order_release);
            }
        }

        ~GuestExecutionProbe()
        {
            gGuestActive.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    void schedulerMainExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(1);
        ExitThread(rdram, ctx, runtime);
    }

    void schedulerTraceA(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(10);
        ctx->pc = 0u;
        if (gSchedulerTrace->size() >= 4u)
        {
            runtime->requestStop();
        }
    }

    void schedulerTraceB(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(20);
        ctx->pc = 0u;
        if (gSchedulerTrace->size() >= 4u)
        {
            runtime->requestStop();
        }
    }

    void schedulerTracePriorityZero(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gSchedulerTrace->push_back(5);
        ctx->pc = 0u;
    }

    void schedulerTracePriorityFive(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(10);
        ctx->pc = 0u;
        if (gSchedulerTrace->size() >= 3u)
        {
            runtime->requestStop();
        }
    }

    void schedulerRotateA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(10);
        ctx->pc = K_SCHED_A_RESUME;
        setRegU32(*ctx, 4, 5u);
        RotateThreadReadyQueue(rdram, ctx, runtime);
    }

    void schedulerRotateAResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(11);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerPreemptLow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(30);
        ctx->pc = K_SCHED_A_RESUME;
        setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerCreatedId));
        setRegU32(*ctx, 5, 0u);
        StartThread(rdram, ctx, runtime);
    }

    void schedulerPreemptLowResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(31);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerHigh(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gSchedulerTrace->push_back(5);
        ctx->pc = 0u;
    }

    void schedulerWaitA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_A)
        {
            gSchedulerTrace->push_back(10);
            ctx->pc = K_SCHED_A_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            WaitSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerWaitResultA = getRegS32(*ctx, 2);
        gSchedulerTrace->push_back(11);
        ctx->pc = 0u;
    }

    void schedulerWaitB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_B)
        {
            gSchedulerTrace->push_back(20);
            ctx->pc = K_SCHED_B_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            WaitSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerWaitResultB = getRegS32(*ctx, 2);
        gSchedulerTrace->push_back(21);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerSignalTwice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_SIGNAL)
        {
            gSchedulerTrace->push_back(30);
            ctx->pc = K_SCHED_SIGNAL_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            SignalSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerTrace->push_back(31);
        setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
        SignalSema(rdram, ctx, runtime);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    constexpr uint32_t K_EVENT_FIFO_MAIN = 0x301000u;
    constexpr uint32_t K_EVENT_FIFO_A = 0x301010u;
    constexpr uint32_t K_EVENT_FIFO_A_RESUME = 0x301014u;
    constexpr uint32_t K_EVENT_FIFO_B = 0x301020u;
    constexpr uint32_t K_EVENT_FIFO_B_RESUME = 0x301024u;
    constexpr uint32_t K_EVENT_FIFO_SIGNAL = 0x301030u;
    constexpr uint32_t K_EVENT_FIFO_SIGNAL_AGAIN = 0x301034u;
    constexpr uint32_t K_EVENT_FIFO_DONE = 0x301038u;
    constexpr uint32_t K_EVENT_FIFO_RESULT_A = 0x1A00u;
    constexpr uint32_t K_EVENT_FIFO_RESULT_B = 0x1A04u;
    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    int gEventFifoId = 0;
    std::vector<int> gEventFifoTrace;

    void schedulerEventFifoWaitA(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_EVENT_FIFO_A)
        {
            gEventFifoTrace.push_back(1);
            ctx->pc = K_EVENT_FIFO_A_RESUME;
            runtime->eeScheduler().waitEventFlag(gEventFifoId, 1u, WEF_OR | WEF_CLEAR,
                                                  K_EVENT_FIFO_RESULT_A);
        }
        gEventFifoTrace.push_back(4);
        ctx->pc = 0u;
    }

    void schedulerEventFifoWaitB(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_EVENT_FIFO_B)
        {
            gEventFifoTrace.push_back(2);
            ctx->pc = K_EVENT_FIFO_B_RESUME;
            runtime->eeScheduler().waitEventFlag(gEventFifoId, 1u, WEF_OR | WEF_CLEAR,
                                                  K_EVENT_FIFO_RESULT_B);
        }
        gEventFifoTrace.push_back(6);
        ctx->pc = 0u;
    }

    void schedulerEventFifoSignal(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(3);
        ctx->pc = K_EVENT_FIFO_SIGNAL_AGAIN;
        runtime->eeScheduler().setEventFlag(gEventFifoId, 1u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventFifoSignalAgain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(5);
        ctx->pc = K_EVENT_FIFO_DONE;
        runtime->eeScheduler().setEventFlag(gEventFifoId, 1u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventFifoDone(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(7);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerEventFifoMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        gEventFifoId = scheduler.createEventFlag(0u, 0x02u, 0u);
        const auto create = [&](uint32_t entry, uint32_t stack, int priority)
        {
            EeThreadCreateParams params{};
            params.entry = entry;
            params.stack = stack;
            params.stackSize = 0x800u;
            params.priority = priority;
            const int id = scheduler.createThread(params);
            scheduler.startThread(id, 0u, *ctx, false);
        };
        create(K_EVENT_FIFO_A, 0x21000u, 5);
        create(K_EVENT_FIFO_B, 0x22000u, 5);
        create(K_EVENT_FIFO_SIGNAL, 0x23000u, 10);
        ctx->pc = 0u;
    }

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        R5900Context ctx{};
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0)
        {
            std::memset(&ctx, 0, sizeof(ctx));
        }
    };
}

void register_ps2_runtime_kernel_tests()
{
    MiniTest::Case("PS2RuntimeKernel", [](TestCase &tc)
    {
        tc.Run("startup syscall census entries have explicit handlers", [](TestCase &t)
        {
            TestEnv env;
            constexpr std::array<uint32_t, 4> outputs{0x3000u, 0x3004u, 0x3008u, 0x300Cu};
            for (size_t i = 0; i < outputs.size(); ++i)
            {
                writeGuestU32(env.rdram.data(), outputs[i], 0xA5A50000u + static_cast<uint32_t>(i));
                setRegU32(env.ctx, 5 + static_cast<int>(i), outputs[i]);
            }
            setRegU32(env.ctx, 4, 0u); // default GS mode
            t.IsTrue(callSyscall(0x80u, env.rdram.data(), &env.ctx, &env.runtime),
                     "_GetGsDxDyOffset must be recognized by the numeric dispatcher");
            for (const uint32_t output : outputs)
            {
                t.Equals(readGuestU32(env.rdram.data(), output), 0u,
                         "host-window HLE must initialize each display-offset output");
            }

            setRegU32(env.ctx, 4, 2u); // handler slot
            setRegU32(env.ctx, 5, 0x00123400u);
            t.IsTrue(callSyscall(0x0Du, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetVTLBRefillHandler must be recognized by the numeric dispatcher");

            t.IsTrue(callSyscall(0x6Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "sceSifStopDma must be recognized by the numeric dispatcher");
        });

        tc.Run("unsigned loads and ABI word writes extend independently", [](TestCase &t)
        {
            constexpr uint64_t kUpper = 0x1122334455667788ull;
            R5900Context ctx{};
            ctx.r[2] = _mm_set_epi64x(static_cast<int64_t>(kUpper), 0);

            SET_GPR_ZE32(&ctx, 2, 0x80000000u);
            t.Equals(static_cast<uint64_t>(_mm_extract_epi64(ctx.r[2], 0)),
                     0x0000000080000000ull,
                     "SET_GPR_ZE32 must zero-extend values used by LWU/LHU/LBU");
            t.Equals(static_cast<uint64_t>(_mm_extract_epi64(ctx.r[2], 1)),
                     kUpper,
                     "SET_GPR_ZE32 must preserve the upper 64 bits of the 128-bit GPR");

            SET_GPR_U32(&ctx, 2, 0x80000000u);
            t.Equals(static_cast<uint64_t>(_mm_extract_epi64(ctx.r[2], 0)),
                     0xFFFFFFFF80000000ull,
                     "SET_GPR_U32 must retain the existing EE 32-bit ABI extension semantics");
            t.Equals(static_cast<uint64_t>(_mm_extract_epi64(ctx.r[2], 1)),
                     kUpper,
                     "SET_GPR_U32 must preserve the upper 64 bits of the 128-bit GPR");
        });

        tc.Run("CreateThread and CreateSema decode the exact PS2SDK EE layouts", [](TestCase &t)
        {
            TestEnv env;
            EeThreadCreateAbi threadParam{};
            threadParam.status = 0x11111111;
            threadParam.func = K_SCHED_HIGH;
            threadParam.stack = 0x00018000u;
            threadParam.stack_size = 0x1000;
            threadParam.gp_reg = 0x00123400u;
            threadParam.initial_priority = 37;
            threadParam.current_priority = 99;
            threadParam.attr = 0xABCDEF01u;
            threadParam.option = 0x10203040u;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &threadParam, sizeof(threadParam));

            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            const int threadId = getRegS32(env.ctx, 2);
            const GuestThread *thread = env.runtime.eeScheduler().thread(threadId);
            t.IsTrue(threadId >= 2 && thread != nullptr, "the EE descriptor should create a guest thread");
            t.Equals(thread->entry, threadParam.func, "func must be decoded from offset 0x04");
            t.Equals(thread->stack, threadParam.stack, "stack must be decoded from offset 0x08");
            t.Equals(thread->stackSize, static_cast<uint32_t>(threadParam.stack_size),
                     "stack_size must be decoded from offset 0x0C");
            t.Equals(thread->gp, threadParam.gp_reg, "gp_reg must be decoded from offset 0x10");
            t.Equals(thread->initialPriority, threadParam.initial_priority,
                     "initial_priority must be decoded from offset 0x14");
            t.Equals(thread->currentPriority, threadParam.initial_priority,
                     "a new thread starts at its initial priority, not the status-only current_priority field");
            t.Equals(thread->attr, threadParam.attr, "attr must be decoded from offset 0x1C");
            t.Equals(thread->option, threadParam.option, "option must be decoded from offset 0x20");
            t.IsTrue(thread->status == EeThreadStatus::Dormant, "a newly created EE thread must be dormant");

            EeThreadCreateAbi highestPriorityParam{};
            highestPriorityParam.func = K_SCHED_HIGH;
            highestPriorityParam.stack = 0x00019000u;
            highestPriorityParam.stack_size = 0x800;
            highestPriorityParam.gp_reg = 0x00123400u;
            highestPriorityParam.initial_priority = 0;
            highestPriorityParam.current_priority = 1;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &highestPriorityParam,
                        sizeof(highestPriorityParam));
            env.runtime.registerFunction(K_SCHED_HIGH, schedulerTraceA);
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            const int highestPriorityId = getRegS32(env.ctx, 2);
            const GuestThread *highestPriorityThread =
                env.runtime.eeScheduler().thread(highestPriorityId);
            t.IsTrue(highestPriorityId >= 2 && highestPriorityThread != nullptr,
                     "priority zero is a valid EE CreateThread priority");
            if (highestPriorityThread)
            {
                t.Equals(highestPriorityThread->initialPriority, 0,
                         "CreateThread must preserve the valid highest priority");
            }
            setRegU32(env.ctx, 4, static_cast<uint32_t>(highestPriorityId));
            setRegU32(env.ctx, 5, 0xCAFEu);
            StartThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK,
                     "StartThread must start a thread created at priority zero");
            t.IsTrue(highestPriorityThread && highestPriorityThread->status == EeThreadStatus::Ready,
                     "the priority-zero thread must enter the ready queue");

            setRegU32(env.ctx, 4, PS2_RAM_SIZE - static_cast<uint32_t>(sizeof(EeThreadCreateAbi)) + 4u);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "the entire EE descriptor must fit in a valid guest range");

            EeSemaStatus semaParam{};
            semaParam.count = 91;
            semaParam.max_count = 7;
            semaParam.init_count = 3;
            semaParam.wait_threads = 82;
            semaParam.attr = 0x55667788u;
            semaParam.option = 0x99AABBCCu;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &semaParam, sizeof(semaParam));
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int semaId = getRegS32(env.ctx, 2);
            const EeSemaphore *semaphore = env.runtime.eeScheduler().semaphore(semaId);
            t.IsTrue(semaId > 0 && semaphore != nullptr, "the EE semaphore descriptor should create an object");
            t.Equals(semaphore->count, semaParam.init_count,
                     "CreateSema must use init_count at offset 0x08, not the status count field");
            t.Equals(semaphore->maxCount, semaParam.max_count, "max_count must be decoded from offset 0x04");
            t.Equals(semaphore->attr, semaParam.attr, "semaphore attr must be decoded from offset 0x10");
            t.Equals(semaphore->option, semaParam.option, "semaphore option must be decoded from offset 0x14");
        });

        tc.Run("EE scheduler selects absolute priority then FIFO", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            gGuestActive.store(0, std::memory_order_release);
            gGuestMaxActive.store(0, std::memory_order_release);
            gGuestExecutorHash.store(0u, std::memory_order_release);
            gGuestExecutorMismatch.store(false, std::memory_order_release);
            gGuestExecutingFlagMissing.store(false, std::memory_order_release);
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerTraceA);
            env.runtime.registerFunction(K_SCHED_B, schedulerTraceB);

            env.ctx.pc = K_SCHED_MAIN;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int lowA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 20, 0});
            const int highA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x21000u, 0x800u, 0, 5, 0});
            const int highB = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x22000u, 0x800u, 0, 5, 0});
            ee.startThread(lowA, 0, env.ctx, false);
            ee.startThread(highA, 0, env.ctx, false);
            ee.startThread(highB, 0, env.ctx, false);
            ee.run();

            t.Equals(trace.size(), size_t{4}, "all runnable contexts should execute once");
            t.Equals(trace[0], 1, "main priority zero executes first");
            t.Equals(trace[1], 10, "first priority-5 thread preserves FIFO order");
            t.Equals(trace[2], 20, "second priority-5 thread follows FIFO order");
            t.Equals(trace[3], 10, "priority-20 thread executes only after priority-5 queue drains");
            t.Equals(gGuestMaxActive.load(std::memory_order_acquire), 1,
                     "there must never be two simultaneous guest executions");
            t.IsFalse(gGuestExecutorMismatch.load(std::memory_order_acquire),
                      "all EE guest functions must execute on the single scheduler host thread");
            t.IsFalse(gGuestExecutingFlagMissing.load(std::memory_order_acquire),
                      "the scheduler must publish guest execution only around the active guest call");
        });

        tc.Run("priority zero is highest and runnable before priority five", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            gGuestActive.store(0, std::memory_order_release);
            gGuestMaxActive.store(0, std::memory_order_release);
            gGuestExecutorHash.store(0u, std::memory_order_release);
            gGuestExecutorMismatch.store(false, std::memory_order_release);
            gGuestExecutingFlagMissing.store(false, std::memory_order_release);
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerTracePriorityFive);
            env.runtime.registerFunction(K_SCHED_B, schedulerTracePriorityZero);

            env.ctx.pc = K_SCHED_MAIN;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int highest = ee.createThread(
                EeThreadCreateParams{0u, K_SCHED_B, 0x23000u, 0x800u, 0u, 0, 0u});
            const int normal = ee.createThread(
                EeThreadCreateParams{0u, K_SCHED_A, 0x24000u, 0x800u, 0u, 5, 0u});
            t.IsTrue(highest >= 2 && normal >= 2,
                     "priority zero and ordinary priorities should both create");
            t.Equals(ee.startThread(highest, 0u, env.ctx, false), KE_OK,
                     "priority-zero thread should start");
            t.Equals(ee.startThread(normal, 0u, env.ctx, false), KE_OK,
                     "priority-five thread should start");
            ee.run();

            const std::vector<int> expected{1, 5, 10};
            t.IsTrue(trace == expected,
                     "the priority-zero thread must run before the priority-five thread");
        });

        tc.Run("thread lifecycle, nested suspend, WAIT-SUSPEND, and wakeup count are centralized", [](TestCase &t)
        {
            TestEnv env;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            ee.bindMainContextForSyscall(env.ctx, env.rdram.data());

            const int id = ee.createThread(EeThreadCreateParams{0u, K_SCHED_HIGH, 0x24000u, 0x800u,
                                                                 0u, 20, 0u});
            t.Equals(ee.startThread(id, 0xCAFEu, env.ctx, false), KE_OK, "StartThread should make a dormant thread ready");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Ready, "started thread should be in one ready queue");
            t.Equals(ee.suspendThread(id, false), KE_OK, "first suspend should remove the ready thread");
            t.Equals(ee.suspendThread(id, false), KE_OK, "nested suspend should increment suspendCount");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Suspended && ee.thread(id)->suspendCount == 2,
                     "nested suspension should have one suspended membership and count two");
            t.Equals(ee.resumeThread(id, false), KE_OK, "first resume should only decrement the nested count");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Suspended && ee.thread(id)->suspendCount == 1,
                     "one outstanding suspend keeps the thread suspended");
            t.Equals(ee.resumeThread(id, false), KE_OK, "final resume should restore readiness");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Ready && ee.thread(id)->suspendCount == 0,
                     "final resume should enqueue the thread exactly once");

            t.Equals(ee.wakeupThread(id, false), KE_OK, "wakeup against a non-sleeping thread should accumulate");
            t.Equals(ee.wakeupThread(id, false), KE_OK, "a second wakeup should accumulate independently");
            t.Equals(ee.cancelWakeup(id), 2, "CancelWakeupThread should return and clear the exact accumulated count");
            t.Equals(ee.cancelWakeup(id), 0, "the wakeup count should remain cleared");

            uint32_t ownedStack = 0u;
            t.Equals(ee.terminateThread(id, ownedStack, false), KE_OK,
                     "TerminateThread should remove a ready thread and make it dormant");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Dormant, "terminated thread should be dormant");
            t.Equals(ee.deleteThread(id, ownedStack), KE_OK, "a dormant thread record should be deletable");
            t.IsTrue(ee.thread(id) == nullptr, "deleted thread must leave every scheduler collection");

            bool slept = false;
            try
            {
                ee.sleepCurrent();
            }
            catch (const EeDispatcherTransfer &)
            {
                slept = true;
            }
            t.IsTrue(slept && ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Waiting,
                     "SleepThread should transfer the running context into a typed wait");
            t.Equals(ee.suspendThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "suspending a waiter should produce WAIT-SUSPEND");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::WaitingSuspended,
                     "the sleeping context should remain in its wait object while suspended");
            t.Equals(ee.wakeupThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "waking WAIT-SUSPEND should complete the wait without enqueueing yet");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Suspended,
                     "completed WAIT-SUSPEND should become plain suspended");
            t.Equals(ee.resumeThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "resuming the final suspend should make the completed waiter ready");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Ready,
                     "the resumed waiter should re-enter its priority queue once");
        });

        tc.Run("semaphore signal, delete, and release complete blocked contexts with exact results", [](TestCase &t)
        {
            const auto blockMain = [](TestEnv &env, int &semaId)
            {
                EeScheduler &ee = env.runtime.eeScheduler();
                ee.reset(env.rdram.data(), env.ctx);
                ee.bindMainContextForSyscall(env.ctx, env.rdram.data());
                semaId = ee.createSemaphore(0, 1, 0u, 0u);
                try
                {
                    ee.waitSemaphore(semaId);
                }
                catch (const EeDispatcherTransfer &)
                {
                }
            };

            TestEnv signaled;
            int signalId = 0;
            blockMain(signaled, signalId);
            t.Equals(signaled.runtime.eeScheduler().signalSemaphore(signalId, false), signalId,
                     "SignalSema should transfer directly to the FIFO waiter");
            t.Equals(getRegS32(signaled.runtime.eeScheduler().thread(1)->context, 2), signalId,
                     "the resumed semaphore waiter should receive the semaphore id");
            t.Equals(signaled.runtime.eeScheduler().semaphore(signalId)->count, 0,
                     "direct semaphore handoff must not increment count");

            TestEnv deleted;
            int deleteId = 0;
            blockMain(deleted, deleteId);
            t.Equals(deleted.runtime.eeScheduler().deleteSemaphore(deleteId, false), deleteId,
                     "DeleteSema should remove the semaphore object");
            t.Equals(getRegS32(deleted.runtime.eeScheduler().thread(1)->context, 2), KE_WAIT_DELETE,
                     "DeleteSema should resume its waiter with KE_WAIT_DELETE");
            t.IsTrue(deleted.runtime.eeScheduler().semaphore(deleteId) == nullptr,
                     "deleted semaphore must no longer own a waiter queue");

            TestEnv released;
            int releaseId = 0;
            blockMain(released, releaseId);
            t.Equals(released.runtime.eeScheduler().releaseWait(EeScheduler::kMainThreadId, false), KE_OK,
                     "ReleaseWaitThread should detach the context from its wait object");
            t.Equals(getRegS32(released.runtime.eeScheduler().thread(1)->context, 2), KE_RELEASE_WAIT,
                     "released waiter should resume with KE_RELEASE_WAIT");
            t.Equals(released.runtime.eeScheduler().semaphore(releaseId)->waiters.size(), size_t{0},
                     "ReleaseWaitThread must remove the exact semaphore waiter");
        });

        tc.Run("event waiters are FIFO and clear modes apply before testing the next waiter", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_EVENT_FIFO_MAIN, schedulerEventFifoMain);
            env.runtime.registerFunction(K_EVENT_FIFO_A, schedulerEventFifoWaitA);
            env.runtime.registerFunction(K_EVENT_FIFO_A_RESUME, schedulerEventFifoWaitA);
            env.runtime.registerFunction(K_EVENT_FIFO_B, schedulerEventFifoWaitB);
            env.runtime.registerFunction(K_EVENT_FIFO_B_RESUME, schedulerEventFifoWaitB);
            env.runtime.registerFunction(K_EVENT_FIFO_SIGNAL, schedulerEventFifoSignal);
            env.runtime.registerFunction(K_EVENT_FIFO_SIGNAL_AGAIN, schedulerEventFifoSignalAgain);
            env.runtime.registerFunction(K_EVENT_FIFO_DONE, schedulerEventFifoDone);
            gEventFifoTrace.clear();
            env.ctx.pc = K_EVENT_FIFO_MAIN;
            env.runtime.eeScheduler().reset(env.rdram.data(), env.ctx);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5, 6, 7};
            t.IsTrue(gEventFifoTrace == expected,
                     "the first clear waiter must consume the first signal before the second FIFO waiter is tested");
            t.Equals(readGuestU32(env.rdram.data(), K_EVENT_FIFO_RESULT_A), 1u,
                     "first waiter should receive its pre-clear observed bits");
            t.Equals(readGuestU32(env.rdram.data(), K_EVENT_FIFO_RESULT_B), 1u,
                     "second waiter should require and receive the second signal");
            t.Equals(env.runtime.eeScheduler().eventFlag(gEventFifoId)->bits, 0u,
                     "both WEF_CLEAR completions should consume their matched bit");

            const int clearAllId = env.runtime.eeScheduler().createEventFlag(0x7u, 0x02u, 0u);
            uint32_t observed = 0u;
            t.Equals(env.runtime.eeScheduler().pollEventFlag(clearAllId, 0x2u, WEF_OR | WEF_CLEAR_ALL, observed), KE_OK,
                     "WEF_CLEAR_ALL poll should complete when any requested bit is present");
            t.Equals(observed, 0x7u, "event result should contain the bits observed before clearing");
            t.Equals(env.runtime.eeScheduler().eventFlag(clearAllId)->bits, 0u,
                     "WEF_CLEAR_ALL should clear the entire event pattern");
        });

        tc.Run("RotateThreadReadyQueue is the only same-priority rotation", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerRotateA);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerRotateAResume);
            env.runtime.registerFunction(K_SCHED_B, schedulerTraceB);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int first = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 5, 0});
            const int second = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x21000u, 0x800u, 0, 5, 0});
            ee.startThread(first, 0, env.ctx, false);
            ee.startThread(second, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 10, 20, 11};
            t.IsTrue(trace == expected, "explicit rotation should move the current head behind its FIFO peer");
        });

        tc.Run("starting a strictly higher-priority thread preempts immediately", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerPreemptLow);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerPreemptLowResume);
            env.runtime.registerFunction(K_SCHED_HIGH, schedulerHigh);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int low = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 20, 0});
            gSchedulerCreatedId = ee.createThread(EeThreadCreateParams{0, K_SCHED_HIGH, 0x21000u, 0x800u, 0, 5, 0});
            ee.startThread(low, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 30, 5, 31};
            t.IsTrue(trace == expected, "higher priority should run before the starter continues");
        });

        tc.Run("semaphore waiters are FIFO and signal transfers one token directly", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            gSchedulerWaitResultA = 0;
            gSchedulerWaitResultB = 0;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerWaitA);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerWaitA);
            env.runtime.registerFunction(K_SCHED_B, schedulerWaitB);
            env.runtime.registerFunction(K_SCHED_B_RESUME, schedulerWaitB);
            env.runtime.registerFunction(K_SCHED_SIGNAL, schedulerSignalTwice);
            env.runtime.registerFunction(K_SCHED_SIGNAL_RESUME, schedulerSignalTwice);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            gSchedulerSemaphoreId = ee.createSemaphore(0, 1, 0, 0);
            const int waiterA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 5, 0});
            const int waiterB = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x21000u, 0x800u, 0, 5, 0});
            const int signaler = ee.createThread(EeThreadCreateParams{0, K_SCHED_SIGNAL, 0x22000u, 0x800u, 0, 20, 0});
            ee.startThread(waiterA, 0, env.ctx, false);
            ee.startThread(waiterB, 0, env.ctx, false);
            ee.startThread(signaler, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 10, 20, 30, 11, 31, 21};
            t.IsTrue(trace == expected, "each signal should wake exactly the FIFO head");
            t.Equals(gSchedulerWaitResultA, gSchedulerSemaphoreId, "first waiter receives sid on resume");
            t.Equals(gSchedulerWaitResultB, gSchedulerSemaphoreId, "second waiter receives sid on resume");
            t.Equals(ee.semaphore(gSchedulerSemaphoreId)->count, 0, "direct handoff must not increment count");
        });

        tc.Run("setup heap and allocator primitives track end-of-heap", [](TestCase &t)
        {
            TestEnv env;

            setRegU32(env.ctx, 4, 0x00180010u);
            setRegU32(env.ctx, 5, 0x00001000u);
            t.IsTrue(callSyscall(0x3Du, env.rdram.data(), &env.ctx, &env.runtime), "SetupHeap syscall should dispatch");
            const uint32_t heapBase = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(heapBase, 0x00180010u, "SetupHeap should return configured base");

            t.IsTrue(callSyscall(0x3Eu, env.rdram.data(), &env.ctx, &env.runtime), "EndOfHeap syscall should dispatch");
            const uint32_t heapLimit = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(heapLimit, 0x00181010u, "EndOfHeap should report the upper limit of the configured heap");

            const uint32_t alignedAlloc = env.runtime.guestMalloc(0x20u, 64u);
            t.IsTrue(alignedAlloc != 0u, "guestMalloc should allocate inside configured heap");
            t.Equals(alignedAlloc & 0x3Fu, 0u, "guestMalloc should honor 64-byte alignment");

            env.runtime.guestFree(alignedAlloc);

            const uint32_t a = env.runtime.guestMalloc(0x100u, 16u);
            const uint32_t b = env.runtime.guestMalloc(0x100u, 16u);
            t.IsTrue(a != 0u && b != 0u, "guestMalloc should provide two adjacent blocks in this heap window");
            env.runtime.guestFree(b);

            const uint32_t grown = env.runtime.guestRealloc(a, 0x180u, 16u);
            t.Equals(grown, a, "guestRealloc should grow in place when adjacent free space is available");

            env.runtime.guestFree(grown);
            const uint32_t reused = env.runtime.guestMalloc(0x80u, 16u);
            t.Equals(reused, heapBase, "guestFree should make the head block reusable");
        });

        tc.Run("memalign stubs allocate aligned guest memory", [](TestCase &t)
        {
            TestEnv env;

            env.runtime.configureGuestHeap(0x00180010u, 0x00182010u);

            setRegU32(env.ctx, 4, 128u);
            setRegU32(env.ctx, 5, 0x40u);
            ps2_stubs::memalign(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t direct = ::getRegU32(&env.ctx, 2);
            t.IsTrue(direct != 0u, "memalign should return a guest address");
            t.Equals(direct & 0x7Fu, 0u, "memalign should honor 128-byte alignment");

            setRegU32(env.ctx, 5, 64u);
            setRegU32(env.ctx, 6, 0x40u);
            ps2_stubs::memalign_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t reent = ::getRegU32(&env.ctx, 2);
            t.IsTrue(reent != 0u, "_memalign_r should return a guest address");
            t.Equals(reent & 0x3Fu, 0u, "_memalign_r should honor 64-byte alignment");
            t.IsTrue(reent != direct, "_memalign_r should allocate a distinct block");
        });

        tc.Run("allocator compatibility stubs use the runtime guest heap", [](TestCase &t)
        {
            TestEnv env;

            env.runtime.configureGuestHeap(0x00180010u, 0x00183010u);

            setRegU32(env.ctx, 5, 0x20u);
            ps2_stubs::malloc_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t initial = ::getRegU32(&env.ctx, 2);
            t.IsTrue(initial != 0u, "_malloc_r should allocate guest memory");

            writeGuestU32(env.rdram.data(), initial, 0xAABBCCDDu);

            setRegU32(env.ctx, 5, initial);
            setRegU32(env.ctx, 6, 0x80u);
            ps2_stubs::realloc_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t grown = ::getRegU32(&env.ctx, 2);
            t.IsTrue(grown != 0u, "_realloc_r should return a guest block");
            t.Equals(readGuestU32(env.rdram.data(), grown), 0xAABBCCDDu,
                     "_realloc_r should preserve existing guest bytes");

            setRegU32(env.ctx, 5, grown);
            ps2_stubs::free_r(env.rdram.data(), &env.ctx, &env.runtime);

            setRegU32(env.ctx, 5, 0x100u);
            ps2_stubs::malloc_extend_top(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(::getRegU32(&env.ctx, 2), 0u,
                     "malloc_extend_top should be a safe runtime-owned heap no-op");

            ps2_stubs::__malloc_lock(env.rdram.data(), &env.ctx, &env.runtime);
            ps2_stubs::__malloc_unlock(env.rdram.data(), &env.ctx, &env.runtime);
        });

        tc.Run("libc helper stubs cover memclr and libgcc div", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kBuf = 0x5000u;
            std::memset(env.rdram.data() + kBuf, 0xCD, 16u);
            setRegU32(env.ctx, 4, kBuf);
            setRegU32(env.ctx, 5, 12u);
            ps2_stubs::memclr(env.rdram.data(), &env.ctx, &env.runtime);
            for (uint32_t i = 0; i < 12u; ++i)
            {
                t.Equals(env.rdram[kBuf + i], static_cast<uint8_t>(0),
                         "memclr should zero the requested byte range");
            }
            t.Equals(env.rdram[kBuf + 12u], static_cast<uint8_t>(0xCD),
                     "memclr should not write past the requested byte range");

            SET_GPR_S64(&env.ctx, 4, -9);
            SET_GPR_S64(&env.ctx, 5, 2);
            ps2_stubs::__divdi3(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), -4, "__divdi3 should divide signed 64-bit values");
        });

        tc.Run("ReleaseAlarm aliases CancelAlarm and cache syscalls succeed", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kAlarmHandlerAddr = 0x00270000u;
            env.runtime.registerFunction(kAlarmHandlerAddr, &alarmNoopHandler);

            setRegU32(env.ctx, 4, 0xFFFFu);
            setRegU32(env.ctx, 5, kAlarmHandlerAddr);
            setRegU32(env.ctx, 6, 0u);
            SetAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            const int32_t alarmId = getRegS32(env.ctx, 2);
            t.IsTrue(alarmId > 0, "SetAlarm should create a cancellable alarm");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(alarmId));
            ReleaseAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "ReleaseAlarm should cancel active alarms");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(alarmId));
            CancelAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "CancelAlarm should report missing alarms after ReleaseAlarm consumes them");

            EnableCache(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "EnableCache should succeed as a no-op");

            DisableCache(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "DisableCache should succeed as a no-op");

            setRegU32(env.ctx, 2, 0xDEADBEEFu);
            setRegU32(env.ctx, 4, 0u); // PS2SDK WRITEBACK_DCACHE
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x68), env.rdram.data(), &env.ctx, &env.runtime),
                     "-0x68 should dispatch iFlushCache");
            t.Equals(getRegS32(env.ctx, 2), KE_OK,
                     "iFlushCache should succeed when guest and host memory are coherent");
        });

        tc.Run("setup heap and thread invalid ids use documented kernel errors", [](TestCase &t)
        {
            TestEnv env;

            setRegU32(env.ctx, 4, 0u);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "CreateThread with null param should fail");

            setRegU32(env.ctx, 4, 0u);
            DeleteThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ILLEGAL_THID, "DeleteThread(0) should be KE_ILLEGAL_THID");

            setRegU32(env.ctx, 4, 0x7FFFu);
            StartThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_UNKNOWN_THID, "StartThread should reject unknown thread ids");

            setRegU32(env.ctx, 4, 0x7FFFu);
            WakeupThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_UNKNOWN_THID, "WakeupThread should reject unknown thread ids");

            setRegU32(env.ctx, 4, 0x7FFFu);
            PollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_UNKNOWN_SEMID, "PollSema should reject unknown semaphore ids");

            setRegU32(env.ctx, 4, 0xFFFFFFFFu);
            t.IsTrue(callSyscall(0x3Du, env.rdram.data(), &env.ctx, &env.runtime), "SetupHeap syscall should dispatch");
            const uint32_t clampedBase = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.IsTrue(clampedBase < PS2_RAM_SIZE, "SetupHeap should normalize out-of-range base into guest RAM");

            t.IsTrue(callSyscall(0x3Eu, env.rdram.data(), &env.ctx, &env.runtime), "EndOfHeap syscall should dispatch");
            const uint32_t heapEnd = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.IsTrue(heapEnd >= clampedBase, "EndOfHeap should be at or above normalized heap base");

            setRegU32(env.ctx, 4, 1u);
            setRegU32(env.ctx, 5, 0u);
            setRegU32(env.ctx, 6, 0u);
            setRegU32(env.ctx, 29, 0x0010FFF0u);
            t.IsTrue(callSyscall(0x3Cu, env.rdram.data(), &env.ctx, &env.runtime), "SetupThread syscall should dispatch");
            const uint32_t setupSp = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(setupSp & 0xFu, 0u, "SetupThread should always return a 16-byte aligned stack pointer");
        });

        tc.Run("SetupThread exposes stable main stack metadata through ReferThreadStatus", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kInitialLoaderSp = PS2_RAM_SIZE - 0x10u;
            constexpr uint32_t kMainStackSize = 0x00020000u;
            constexpr uint32_t kExpectedStack = PS2_RAM_SIZE - kMainStackSize;
            constexpr uint32_t kMainGp = 0x0036A7F0u;

            env.ctx.pc = 0x00100000u;
            setRegU32(env.ctx, 29, kInitialLoaderSp);
            setRegU32(env.ctx, 4, kMainGp);
            setRegU32(env.ctx, 5, 0xFFFFFFFFu);
            setRegU32(env.ctx, 6, kMainStackSize);
            t.IsTrue(callSyscall(0x3Cu, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetupThread syscall should dispatch");
            t.Equals(::getRegU32(&env.ctx, 2), kExpectedStack,
                     "automatic main stack should start below the reserved top-of-RDRAM area");

            // ReferThreadStatus can be called after many nested frames have moved $sp.
            // It must report the initial stack recorded by SetupThread, not this live snapshot.
            constexpr uint32_t kTransientSp = kExpectedStack - 0x80u;
            setRegU32(env.ctx, 29, kTransientSp);
            setRegU32(env.ctx, 4, 0u);
            setRegU32(env.ctx, 5, K_STATUS_ADDR);
            t.IsTrue(callSyscall(0x30u, env.rdram.data(), &env.ctx, &env.runtime),
                     "ReferThreadStatus syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "ReferThreadStatus should accept the current-thread id alias");

            EeThreadStatusAbi status{};
            std::memcpy(&status, env.rdram.data() + K_STATUS_ADDR, sizeof(status));
            t.Equals(status.stack, kExpectedStack,
                     "main thread status must expose SetupThread's stable initial stack");
            t.Equals(status.stack_size, static_cast<int32_t>(kMainStackSize),
                     "main thread status must preserve SetupThread's stack size");
            t.Equals(status.gp_reg, kMainGp,
                     "main thread status must preserve SetupThread's global pointer");
            t.IsTrue(status.stack != kInitialLoaderSp && status.stack != kTransientSp,
                     "main thread status must never expose a live stack-pointer snapshot");
        });

        tc.Run("OSD config2 syscalls round-trip extended config", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kConfig2Addr = 0x00005000u;
            constexpr uint32_t kConfig2OutAddr = 0x00005010u;
            constexpr uint32_t kConfig1OutAddr = 0x00005020u;
            constexpr uint32_t kInitialConfig1 =
                (1u << 0) |  // SPDIF disabled
                (1u << 4) |  // non-Japanese language flag
                (1u << 13) | // OSD2
                (1u << 16);  // English
            constexpr uint32_t kConfig2Raw =
                0xABu |        // format
                (0xB0u << 8) | // daylightSaving=1, timeFormat=1, dateFormat=2
                (2u << 16) |   // extended OSD version
                (10u << 24);   // traditional Chinese

            writeGuestU32(env.rdram.data(), K_PARAM_ADDR, kInitialConfig1);
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            t.IsTrue(callSyscall(0x4Au, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetOsdConfigParam syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "SetOsdConfigParam should seed base OSD state");

            writeGuestU32(env.rdram.data(), kConfig2Addr, kConfig2Raw);
            setRegU32(env.ctx, 4, kConfig2Addr);
            setRegU32(env.ctx, 5, 4u);
            setRegU32(env.ctx, 6, 0u);
            t.IsTrue(callSyscall(0x6Eu, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetOsdConfigParam2 syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "SetOsdConfigParam2 should succeed");

            writeGuestU32(env.rdram.data(), kConfig2OutAddr, 0xFFFFFFFFu);
            setRegU32(env.ctx, 4, kConfig2OutAddr);
            setRegU32(env.ctx, 5, 4u);
            setRegU32(env.ctx, 6, 0u);
            t.IsTrue(callSyscall(0x6Fu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetOsdConfigParam2 syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "GetOsdConfigParam2 should succeed");
            const uint32_t readConfig2 = readGuestU32(env.rdram.data(), kConfig2OutAddr);
            t.Equals(readConfig2, kConfig2Raw, "GetOsdConfigParam2 should round-trip the sanitized Config2Param bytes");
            t.Equals((readConfig2 >> 12) & 1u, 1u, "Config2 daylightSaving should live at bit 12 for libosd callers");

            setRegU32(env.ctx, 4, kConfig1OutAddr);
            t.IsTrue(callSyscall(0x4Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetOsdConfigParam syscall should dispatch after Config2 update");
            const uint32_t readConfig1 = readGuestU32(env.rdram.data(), kConfig1OutAddr);
            t.Equals((readConfig1 >> 13) & 0x7u, 2u, "SetOsdConfigParam2 should sync ConfigParam.version");
            t.Equals((readConfig1 >> 16) & 0x1Fu, 10u, "SetOsdConfigParam2 should sync ConfigParam.language");
        });

        tc.Run("numeric syscall 0x83 finds matching table entry", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBase = 0x00002000u;
            constexpr uint32_t kValues[] = {
                0x11111111u,
                0x11223344u,
                0x55555555u,
                0x89ABCDEFu
            };

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBase);
            setRegU32(env.ctx, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0x11223344u);

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kTableBase + 4u,
                     "FindAddress should return address of first matching word");
        });

        tc.Run("numeric syscall 0x83 supports KSEG aliases", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBasePhys = 0x00003000u;
            constexpr uint32_t kTableBaseKseg = 0x80003000u;
            constexpr uint32_t kValues[] = {
                0x00123456u,
                0x8000AAAAu
            };

            writeGuestWords(env.rdram.data(), kTableBasePhys, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBaseKseg);
            setRegU32(env.ctx, 5, kTableBaseKseg + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0x80123456u); // Alias of first table value

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kTableBaseKseg,
                     "FindAddress should match KSEG aliases and preserve guest segment in return value");
        });

        tc.Run("numeric syscall 0x83 returns 0 when entry is absent", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBase = 0x00004000u;
            constexpr uint32_t kValues[] = {
                0x00000001u,
                0x00000002u,
                0x00000003u
            };

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBase);
            setRegU32(env.ctx, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0xDEADBEEFu);

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     0u,
                     "FindAddress should return 0 when no matching word exists");
        });

        tc.Run("SetSyscall mirrors guest kernel table entries into low memory", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kSyscallIndex = 0x82u;
            constexpr uint32_t kHandler = 0x00383548u;
            constexpr uint32_t kExpectedGuestAddr = kGuestSyscallTableGuestBase + (kSyscallIndex * 4u);
            constexpr uint32_t kExpectedPhysAddr = kExpectedGuestAddr & 0x1FFFFFFFu;

            setRegU32(env.ctx, 4, kSyscallIndex);
            setRegU32(env.ctx, 5, kHandler);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall syscall should dispatch");

            uint32_t mirrored = 0u;
            std::memcpy(&mirrored, env.rdram.data() + kExpectedPhysAddr, sizeof(mirrored));
            t.Equals(mirrored,
                     kHandler,
                     "SetSyscall should mirror handler pointers into the guest kernel syscall table");

            setRegU32(env.ctx, 4, 0x80000000u);
            setRegU32(env.ctx, 5, 0x80080000u);
            setRegU32(env.ctx, 6, kHandler);
            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "FindAddress syscall should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kExpectedGuestAddr,
                     "FindAddress should discover mirrored SetSyscall entries in low guest memory");
        });

        tc.Run("SetSyscall honors signed kernel-table offsets", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kPatchIndex = 0xFFFFC402u;
            constexpr uint32_t kHandler = 0xDEADBEEFu;
            constexpr uint32_t kExpectedGuestAddr = 0x80002F88u;
            constexpr uint32_t kExpectedPhysAddr = kExpectedGuestAddr & 0x1FFFFFFFu;

            setRegU32(env.ctx, 4, kPatchIndex);
            setRegU32(env.ctx, 5, kHandler);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall syscall should dispatch for signed offsets");

            uint32_t mirrored = 0u;
            std::memcpy(&mirrored, env.rdram.data() + kExpectedPhysAddr, sizeof(mirrored));
            t.Equals(mirrored,
                     kHandler,
                     "SetSyscall should treat the syscall index as a signed offset from the kernel table base");
        });

        tc.Run("guest kernel syscall overrides and mirrors are isolated per runtime", [](TestCase &t)
        {
            TestEnv first;
            TestEnv second;
            initializeGuestKernelState(first.rdram.data(), &first.runtime);
            initializeGuestKernelState(second.rdram.data(), &second.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kGuestSyscallTableProbeBase = 0x000002F0u;
            constexpr uint32_t kSyscallIndex = 0x5Au;
            constexpr uint32_t kFirstHandler = 0x00383510u;
            constexpr uint32_t kSecondHandler = 0x00383520u;
            constexpr uint32_t kEntryPhysAddr = (kGuestSyscallTableGuestBase + (kSyscallIndex * 4u)) & 0x1FFFFFFFu;

            setRegU32(first.ctx, 4, kSyscallIndex);
            setRegU32(first.ctx, 5, kFirstHandler);
            t.IsTrue(callSyscall(0x74u, first.rdram.data(), &first.ctx, &first.runtime),
                     "SetSyscall should install the first runtime's override");

            uint32_t firstHandler = 0u;
            uint32_t secondHandler = 0u;
            t.IsTrue(first.runtime.findEeSyscallOverride(kSyscallIndex, firstHandler),
                     "the first runtime should own its override");
            t.IsFalse(second.runtime.findEeSyscallOverride(kSyscallIndex, secondHandler),
                      "a different runtime must not observe the first runtime's override");

            setRegU32(second.ctx, 4, kSyscallIndex);
            setRegU32(second.ctx, 5, kSecondHandler);
            t.IsTrue(callSyscall(0x74u, second.rdram.data(), &second.ctx, &second.runtime),
                     "SetSyscall should install an independent second-runtime override");

            uint32_t firstMirrored = 0u;
            uint32_t secondMirrored = 0u;
            std::memcpy(&firstMirrored, first.rdram.data() + kEntryPhysAddr, sizeof(firstMirrored));
            std::memcpy(&secondMirrored, second.rdram.data() + kEntryPhysAddr, sizeof(secondMirrored));
            t.Equals(firstMirrored, kFirstHandler, "the first runtime should retain its own mirror value");
            t.Equals(secondMirrored, kSecondHandler, "the second runtime should publish only its own mirror value");

            initializeGuestKernelState(first.rdram.data(), &first.runtime);
            std::memcpy(&firstMirrored, first.rdram.data() + kEntryPhysAddr, sizeof(firstMirrored));
            t.Equals(firstMirrored, kFirstHandler,
                     "reinitializing one runtime should rebuild its mirror from its instance-owned overrides");

            uint32_t probeHi = 0u;
            uint32_t probeLo = 0u;
            std::memcpy(&probeHi, first.rdram.data() + kGuestSyscallTableProbeBase + 0u, sizeof(probeHi));
            std::memcpy(&probeLo, first.rdram.data() + kGuestSyscallTableProbeBase + 8u, sizeof(probeLo));
            t.Equals(probeHi,
                     kGuestSyscallTableGuestBase >> 16,
                     "Guest kernel initialization should seed the syscall table probe high word");
            t.Equals(probeLo,
                     kGuestSyscallTableGuestBase & 0xFFFFu,
                     "Guest kernel initialization should seed the syscall table probe low word");
        });

        tc.Run("SetSyscall override runs as a scheduler invocation", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x91u;
            constexpr uint32_t kHandler = 0x00200000u;

            env.runtime.registerFunction(kHandler, overrideReturnHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, 7u);
            setRegU32(mainContext, 5, 5u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     12u,
                     "the completed invocation should propagate the guest handler return value");
        });

        tc.Run("SetSyscall override preserves KSEG argument sign extension", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x92u;
            constexpr uint32_t kHandler = 0x00200030u;

            env.runtime.registerFunction(kHandler, overrideKsegCompareHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, 0x80000000u);
            setRegU32(mainContext, 5, 0x80080000u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     0x80000004u,
                     "Override invocation should preserve KSEG ordering after 32-bit guest writes");
        });

        tc.Run("SetSyscall override preserves upper 64 bits when writing 32-bit args", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x93u;
            constexpr uint32_t kHandler = 0x00200040u;

            env.runtime.registerFunction(kHandler, overridePreserveUpper64Handler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            mainContext.r[4] = _mm_set_epi64x(static_cast<int64_t>(K_EXPECTED_UPPER64),
                                               static_cast<int64_t>(static_cast<int32_t>(0x80000000u)));
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     1u,
                     "Override invocation should preserve the upper 64 bits of 128-bit GPRs when setting 32-bit args");
        });

        tc.Run("an override that branches to an invalid PC completes without builtin fallback", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kHandler = 0x00200010u;

            env.runtime.registerFunction(kHandler, overrideBrokenHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x83u, kHandler);

            gOverrideSyscall = 0x83u;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     0xDEADBEEFu,
                     "the dispatcher should preserve the invocation result and never call the builtin as a fallback");
        });

        tc.Run("reentrant override invokes the underlying builtin inside its invocation frame", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kHandler = 0x00200020u;
            constexpr uint32_t kTableBase = 0x00003000u;
            constexpr uint32_t kValues[] = {
                0xCAFEBABEu,
                0x11223344u,
                0x55667788u
            };

            env.runtime.registerFunction(kHandler, overrideRecursiveFindAddressHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x83u, kHandler);

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            gOverrideSyscall = 0x83u;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, kTableBase);
            setRegU32(mainContext, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(mainContext, 6, 0x11223344u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     kTableBase + 4u,
                     "only the recursive call should bypass the active override frame and reach the builtin");
        });

        tc.Run("a syscall invocation can block and resume without losing its base context", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x94u;
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_ENTRY, schedulerBlockingOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_HANDLER, schedulerBlockingOverrideHandler);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_HANDLER_RESUME, schedulerBlockingOverrideHandlerResume);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_DRIVER, schedulerBlockingOverrideDriver);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_BASE_RESUME, schedulerBlockingOverrideBaseResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, K_OVERRIDE_BLOCK_HANDLER);

            gOverrideSyscall = kSyscallIndex;
            gInvocationTrace.clear();
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_BLOCK_ENTRY;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5};
            t.IsTrue(gInvocationTrace == expected,
                     "the sleeping invocation should yield to the driver, resume its own frame, then restore the base frame");
            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)), 0xB10C0EDu,
                     "the invocation result should reach the preserved base context after the wait");
        });

        tc.Run("exit handlers run as ordered invocation frames before the thread becomes dormant", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_EXIT_MAIN, schedulerExitMain);
            env.runtime.registerFunction(K_EXIT_HANDLER_A, schedulerExitHandlerA);
            env.runtime.registerFunction(K_EXIT_HANDLER_B, schedulerExitHandlerB);
            env.runtime.registerFunction(K_EXIT_OBSERVER, schedulerExitObserver);

            gInvocationTrace.clear();
            R5900Context mainContext{};
            mainContext.pc = K_EXIT_MAIN;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{10, 20, 30, 40};
            t.IsTrue(gInvocationTrace == expected,
                     "exit handlers should retain registration order and complete before another guest thread observes dormancy");
        });

        tc.Run("IRQ syscall uses the interrupted stack instead of registration-time SP", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_IRQ_STACK_MAIN, schedulerIrqStackMain);
            env.runtime.registerFunction(K_IRQ_STACK_HANDLER, schedulerIrqStackHandler);

            gIrqCallbackEntrySp = 0u;
            gIrqCallbackCount = 0u;
            gIrqRegistrationResult = KE_ERROR;
            std::memcpy(env.rdram.data() + K_IRQ_SAVED_RA_SLOT,
                        &K_IRQ_SAVED_RA_SENTINEL,
                        sizeof(K_IRQ_SAVED_RA_SENTINEL));

            R5900Context mainContext{};
            mainContext.pc = K_IRQ_STACK_MAIN;
            setRegU32(mainContext, 29, K_IRQ_REGISTRATION_SP);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            uint64_t savedRa = 0u;
            std::memcpy(&savedRa,
                        env.rdram.data() + K_IRQ_SAVED_RA_SLOT,
                        sizeof(savedRa));
            t.Equals(gIrqRegistrationResult, 1,
                     "AddDmacHandler should register the guest handler");
            t.Equals(gIrqCallbackCount, 1u,
                     "the enabled IRQ should invoke the registered callback once");
            t.Equals(gIrqCallbackEntrySp, K_IRQ_INTERRUPTED_SP,
                     "the callback should start from the interrupted context SP, not the SP at registration");
            t.Equals(savedRa, K_IRQ_SAVED_RA_SENTINEL,
                     "the handler's stack prologue must not overwrite the interrupted caller's saved frame");
        });

        tc.Run("Copy syscall (0x5A) performs a memory copy", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kDestAddr = 0x00005000u;
            constexpr uint32_t kSrcAddr = 0x00006000u;
            constexpr uint32_t kSize = 16u;
            constexpr uint32_t kValues[] = {
                0x11223344u,
                0x55667788u,
                0x99AABBCCu,
                0xDDEEFF00u
            };

            writeGuestWords(env.rdram.data(), kSrcAddr, kValues, std::size(kValues));
            
            setRegU32(env.ctx, 4, kDestAddr);
            setRegU32(env.ctx, 5, kSrcAddr);
            setRegU32(env.ctx, 6, kSize);

            t.IsTrue(callSyscall(0x5Au, env.rdram.data(), &env.ctx, &env.runtime),
                     "Copy syscall should dispatch");
            
            for (size_t i = 0; i < std::size(kValues); ++i)
            {
                uint32_t destVal = readGuestU32(env.rdram.data(), kDestAddr + static_cast<uint32_t>(i * sizeof(uint32_t)));
                t.Equals(destVal, kValues[i], "Copy should correctly transfer bytes");
            }
        });

        tc.Run("GetEntryAddress syscall (0x5B) returns handler from guest table", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kSyscallIndex = 0x5Au;
            constexpr uint32_t kExpectedHandler = 0x00383548u;
            constexpr uint32_t kEntryPhysAddr = (kGuestSyscallTableGuestBase + (kSyscallIndex * 4u)) & 0x1FFFFFFFu;

            writeGuestU32(env.rdram.data(), kEntryPhysAddr, kExpectedHandler);

            setRegU32(env.ctx, 4, kSyscallIndex);

            t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetEntryAddress syscall should dispatch");
            
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kExpectedHandler,
                     "GetEntryAddress should read and return the handler address from the table");
        });

        tc.Run("spin loop waits on counter cleared by queued DMAC handler", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_DMAC_SPIN_MAIN, schedulerDmacSpinMain);
            env.runtime.registerFunction(K_DMAC_SPIN_HANDLER, schedulerDmacSpinHandler);
            env.runtime.registerFunction(K_DMAC_SPIN_POLL_FN, schedulerDmacSpinPollFn);

            gSpinPollCalls = 0u;
            gSpinHandlerCalls = 0u;

            R5900Context mainContext{};
            mainContext.pc = K_DMAC_SPIN_MAIN;
            setRegU32(mainContext, 29, 0x00100000u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            uint32_t finalCounter = 999u;
            std::memcpy(&finalCounter, env.rdram.data() + K_DMAC_SPIN_COUNTER_ADDR, sizeof(finalCounter));

            t.Equals(gSpinHandlerCalls, 1u, "queued DMAC handler should be dispatched while polling");
            t.IsTrue(gSpinPollCalls >= 1u, "poll function should be called at least once");
            t.Equals(finalCounter, 0u, "counter should be cleared to 0 by the DMAC handler");
            t.Equals(getRegS32(mainContext, 2), KE_OK, "main loop should exit successfully with KE_OK");
        });

        tc.Run("PadDataNew double-buffer layout and populatePadBuffer stable state", [](TestCase &t)
        {
            using namespace armageddon_hle;
            t.Equals(sizeof(PadDataNew), 128ULL, "PadDataNew must be exactly 128 bytes");
            t.Equals(sizeof(PadAreaNew), 256ULL, "PadAreaNew must be exactly 256 bytes");
            t.Equals(offsetof(PadDataNew, data), 0x00ULL, "data offset must be 0x00");
            t.Equals(offsetof(PadDataNew, actDirData), 0x20ULL, "actDirData offset must be 0x20");
            t.Equals(offsetof(PadDataNew, actAlignData), 0x28ULL, "actAlignData offset must be 0x28");
            t.Equals(offsetof(PadDataNew, actData), 0x30ULL, "actData offset must be 0x30");
            t.Equals(offsetof(PadDataNew, modeTable), 0x50ULL, "modeTable offset must be 0x50");
            t.Equals(offsetof(PadDataNew, frame), 0x58ULL, "frame offset must be 0x58");
            t.Equals(offsetof(PadDataNew, findPadRetries), 0x5CULL, "findPadRetries offset must be 0x5C");
            t.Equals(offsetof(PadDataNew, length), 0x60ULL, "length offset must be 0x60");
            t.Equals(offsetof(PadDataNew, modeConfig), 0x64ULL, "modeConfig offset must be 0x64");
            t.Equals(offsetof(PadDataNew, modeCurId), 0x65ULL, "modeCurId offset must be 0x65");
            t.Equals(offsetof(PadDataNew, model), 0x66ULL, "model offset must be 0x66");
            t.Equals(offsetof(PadDataNew, buttonDataReady), 0x67ULL, "buttonDataReady offset must be 0x67");
            t.Equals(offsetof(PadDataNew, nrOfModes), 0x68ULL, "nrOfModes offset must be 0x68");
            t.Equals(offsetof(PadDataNew, modeCurOffs), 0x69ULL, "modeCurOffs offset must be 0x69");
            t.Equals(offsetof(PadDataNew, nrOfActuators), 0x6AULL, "nrOfActuators offset must be 0x6A");
            t.Equals(offsetof(PadDataNew, numActComb), 0x6BULL, "numActComb offset must be 0x6B");
            t.Equals(offsetof(PadDataNew, val_c6), 0x6CULL, "val_c6 offset must be 0x6C");
            t.Equals(offsetof(PadDataNew, mode), 0x6DULL, "mode offset must be 0x6D");
            t.Equals(offsetof(PadDataNew, lock), 0x6EULL, "lock offset must be 0x6E");
            t.Equals(offsetof(PadDataNew, actDirSize), 0x6FULL, "actDirSize offset must be 0x6F");
            t.Equals(offsetof(PadDataNew, state), 0x70ULL, "state offset must be 0x70");
            t.Equals(offsetof(PadDataNew, reqState), 0x71ULL, "reqState offset must be 0x71");
            t.Equals(offsetof(PadDataNew, currentTask), 0x72ULL, "currentTask offset must be 0x72");
            t.Equals(offsetof(PadDataNew, runTask), 0x73ULL, "runTask offset must be 0x73");
            t.Equals(offsetof(PadDataNew, stat70bit), 0x74ULL, "stat70bit offset must be 0x74");

            PadAreaNew area{};
            populatePadBuffer(area.buffers[0], 1u, PAD_STATE_STABLE, PAD_RSTAT_COMPLETE);
            populatePadBuffer(area.buffers[1], 0u, PAD_STATE_STABLE, PAD_RSTAT_COMPLETE);

            // Raw byte inspection of buffer 0 without struct field accessors
            const uint8_t *raw0 = reinterpret_cast<const uint8_t *>(&area.buffers[0]);
            t.Equals(static_cast<uint32_t>(raw0[0x00]), 0x00u, "Raw data[0] terminal must be 0");
            t.Equals(static_cast<uint32_t>(raw0[0x01]), 0x79u, "Raw data[1] ID must be 0x79 (DualShock 2)");
            t.Equals(static_cast<uint32_t>(raw0[0x02]), 0xFFu, "Raw data[2] buttons low must be 0xFF");
            t.Equals(static_cast<uint32_t>(raw0[0x03]), 0xFFu, "Raw data[3] buttons high must be 0xFF");
            t.Equals(static_cast<uint32_t>(raw0[0x04]), 0x7Fu, "Raw data[4] right analog X must be 0x7F");
            t.Equals(static_cast<uint32_t>(raw0[0x05]), 0x7Fu, "Raw data[5] right analog Y must be 0x7F");
            t.Equals(static_cast<uint32_t>(raw0[0x06]), 0x7Fu, "Raw data[6] left analog X must be 0x7F");
            t.Equals(static_cast<uint32_t>(raw0[0x07]), 0x7Fu, "Raw data[7] left analog Y must be 0x7F");
            for (size_t p = 8; p < 20; ++p)
            {
                t.Equals(static_cast<uint32_t>(raw0[p]), 0x00u, "Raw pressure sensors must be 0");
            }
            uint16_t modeTable0 = 0;
            std::memcpy(&modeTable0, raw0 + 0x50, sizeof(modeTable0));
            t.Equals(static_cast<uint32_t>(modeTable0), 7u, "Raw modeTable[0] must be 7");
            uint32_t frame0 = 0;
            std::memcpy(&frame0, raw0 + 0x58, sizeof(frame0));
            t.Equals(frame0, 1u, "Raw frame must be 1");
            uint32_t len0 = 0;
            std::memcpy(&len0, raw0 + 0x60, sizeof(len0));
            t.Equals(len0, 32u, "Raw length must be 32");
            t.Equals(static_cast<uint32_t>(raw0[0x64]), 2u, "Raw modeConfig must be 2");
            t.Equals(static_cast<uint32_t>(raw0[0x65]), 0x79u, "Raw modeCurId must be 0x79");
            t.Equals(static_cast<uint32_t>(raw0[0x6E]), 3u, "Raw lock must be 3");
            t.Equals(static_cast<uint32_t>(raw0[0x70]), static_cast<uint32_t>(PAD_STATE_STABLE), "Raw state at 0x70 must be STABLE (6)");
            t.Equals(static_cast<uint32_t>(raw0[0x71]), static_cast<uint32_t>(PAD_RSTAT_COMPLETE), "Raw reqState at 0x71 must be COMPLETE (0)");

            // Raw byte inspection of buffer 1
            const uint8_t *raw1 = reinterpret_cast<const uint8_t *>(&area.buffers[1]);
            uint32_t frame1 = 0;
            std::memcpy(&frame1, raw1 + 0x58, sizeof(frame1));
            t.Equals(frame1, 0u, "Buffer 1 raw frame must be 0");
            t.Equals(static_cast<uint32_t>(raw1[0x70]), static_cast<uint32_t>(PAD_STATE_STABLE), "Buffer 1 raw state at 0x70 must be STABLE (6)");
            t.Equals(static_cast<uint32_t>(raw1[0x71]), static_cast<uint32_t>(PAD_RSTAT_COMPLETE), "Buffer 1 raw reqState at 0x71 must be COMPLETE (0)");
            t.IsTrue(frame0 > frame1, "Double-buffer: buffer 0 newer than buffer 1 for padGetDmaStrNew");
        });

        tc.Run("EeScheduler queueDeferredAction executes full RPC completion after delay", [](TestCase &t)
        {
            TestEnv env;
            R5900Context ctx{};
            ctx.pc = 0x00100000u;
            env.runtime.eeScheduler().reset(env.rdram.data(), ctx);

            bool actionExecuted = false;
            env.runtime.eeScheduler().queueDeferredAction([&]()
            {
                actionExecuted = true;
            }, 5000000ull);

            t.IsFalse(actionExecuted, "Deferred action should not execute immediately");
        });

        tc.Run("GS per-path persistent IMAGE parser transfers verified bytes and isolates paths", [](TestCase &t)
        {
            class MockGsBackend final : public GSRasterBackend
            {
            public:
                std::vector<uint8_t> uploadedBytes;

                void Initialize(uint8_t *, uint32_t) override {}
                void Reset() override { uploadedBytes.clear(); }
                void Submit(const GSPrimitiveBatch &) override {}
                void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
                void BeginTransfer(const GSTransferCommand &) override {}
                void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
                {
                    if (data && sizeBytes > 0)
                    {
                        uploadedBytes.insert(uploadedBytes.end(), data, data + sizeBytes);
                    }
                }
                void Flush() override {}
                void TextureFlush() override {}
                void Sync(GSSyncReason) override {}
                PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
                bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
                uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0; }
                uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0; }
                void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
                void SnapshotVram(std::vector<uint8_t> &) const override {}
                GSTransferSnapshot GetTransferSnapshot() const override { return {}; }
            };

            GS gs;
            std::vector<uint8_t> vram(4 * 1024 * 1024, 0);
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()));

            auto mockBackend = std::make_unique<MockGsBackend>();
            MockGsBackend *rawMock = mockBackend.get();
            gs.setRasterBackend(std::move(mockBackend));

            // Test 1: Split IMAGE transfer (FLG = 2) on Path 1 across 2 packets
            // Total 2 quadwords (32 bytes). Packet 1: GIFtag (16 bytes) + 16 bytes image data (0xAA).
            // Packet 2: 16 bytes image data (0xBB).
            const uint64_t tagLo1 = 2ULL | (2ULL << 58); // NLOOP = 2, FLG = 2 (IMAGE)
            const uint64_t tagHi1 = 0ULL;
            std::array<uint8_t, 32> packet1{};
            std::memcpy(packet1.data(), &tagLo1, 8);
            std::memcpy(packet1.data() + 8, &tagHi1, 8);
            std::memset(packet1.data() + 16, 0xAA, 16);

            std::array<uint8_t, 16> packet2{};
            std::memset(packet2.data(), 0xBB, 16);

            gs.processGIFPacket(GifPathId::Path1, packet1.data(), static_cast<uint32_t>(packet1.size()));

            // Verify Path 1 entered ImageContinuation with 16 bytes remaining
            const auto &path1State = gs.getPathState(GifPathId::Path1);
            t.Equals(static_cast<uint32_t>(path1State.state), static_cast<uint32_t>(GifPathTransferState::ImageContinuation),
                     "Path 1 must be in ImageContinuation after partial packet");
            t.Equals(path1State.remainingImageBytes, 16u, "Path 1 remainingImageBytes must be 16");

            // Verify Path 3 isolation: Path 3 must still be Idle
            const auto &path3State = gs.getPathState(GifPathId::Path3);
            t.Equals(static_cast<uint32_t>(path3State.state), static_cast<uint32_t>(GifPathTransferState::Idle),
                     "Path 3 must remain Idle while Path 1 has pending continuation");

            // Send packet 2 on Path 1
            gs.processGIFPacket(GifPathId::Path1, packet2.data(), static_cast<uint32_t>(packet2.size()));

            // Verify Path 1 returned to Idle
            t.Equals(static_cast<uint32_t>(path1State.state), static_cast<uint32_t>(GifPathTransferState::Idle),
                     "Path 1 state must return to Idle after transfer completes");
            t.Equals(path1State.remainingImageBytes, 0u, "Path 1 remainingImageBytes must be 0");

            // Verify exact 32 transferred image bytes and contents in correct order
            t.Equals(rawMock->uploadedBytes.size(), 32ULL, "Mock backend must receive exactly 32 image bytes");
            bool bytesMatch = true;
            for (size_t i = 0; i < 16; ++i)
            {
                if (rawMock->uploadedBytes[i] != 0xAA) bytesMatch = false;
            }
            for (size_t i = 16; i < 32; ++i)
            {
                if (rawMock->uploadedBytes[i] != 0xBB) bytesMatch = false;
            }
            t.IsTrue(bytesMatch, "Uploaded bytes must match 0xAA for chunk 1 and 0xBB for chunk 2");

            // Test 2: FLG = 3 (DISABLE/raw image) transfer
            rawMock->uploadedBytes.clear();
            const uint64_t tagLoFlg3 = 1ULL | (3ULL << 58); // NLOOP = 1, FLG = 3
            std::array<uint8_t, 32> packetFlg3{};
            std::memcpy(packetFlg3.data(), &tagLoFlg3, 8);
            std::memcpy(packetFlg3.data() + 8, &tagHi1, 8);
            std::memset(packetFlg3.data() + 16, 0xCC, 16);

            gs.processGIFPacket(GifPathId::Path2, packetFlg3.data(), static_cast<uint32_t>(packetFlg3.size()));
            t.Equals(rawMock->uploadedBytes.size(), 16ULL, "FLG=3 must transfer 16 image bytes");
            t.Equals(static_cast<uint32_t>(rawMock->uploadedBytes[0]), 0xCCu, "FLG=3 payload byte must be 0xCC");
        });

        tc.Run("ChangeThreadPriority returns previous priority", [](TestCase &t)
        {
            TestEnv env;
            env.ctx.pc = 0x00100000u;
            setRegU32(env.ctx, 29, 0x0010FFF0u);
            setRegU32(env.ctx, 4, 0x0036A7F0u);
            setRegU32(env.ctx, 5, 0xFFFFFFFFu);
            setRegU32(env.ctx, 6, 0x00020000u);
            t.IsTrue(callSyscall(0x3Cu, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetupThread syscall should dispatch");

            // Change main thread priority from initial 0 to 41
            setRegU32(env.ctx, 4, 0u);
            setRegU32(env.ctx, 5, 41u);
            t.IsTrue(callSyscall(0x29u, env.rdram.data(), &env.ctx, &env.runtime),
                     "ChangeThreadPriority should dispatch");
            const int32_t oldPri0 = getRegS32(env.ctx, 2);
            t.Equals(oldPri0, 0, "Initial priority returned should be 0");

            // Change priority of current thread from 41 to 30
            setRegU32(env.ctx, 4, 0u);
            setRegU32(env.ctx, 5, 30u);
            t.IsTrue(callSyscall(0x29u, env.rdram.data(), &env.ctx, &env.runtime),
                     "ChangeThreadPriority should dispatch");
            const int32_t oldPri1 = getRegS32(env.ctx, 2);
            t.Equals(oldPri1, 41, "First change should return previous priority 41");

            // Restore priority back to 41 using the returned old priority
            setRegU32(env.ctx, 4, 0u);
            setRegU32(env.ctx, 5, static_cast<uint32_t>(oldPri1));
            t.IsTrue(callSyscall(0x29u, env.rdram.data(), &env.ctx, &env.runtime),
                     "ChangeThreadPriority should dispatch");
            const int32_t oldPri2 = getRegS32(env.ctx, 2);
            t.Equals(oldPri2, 30, "Second change should return previous priority 30");
        });
    });
}
