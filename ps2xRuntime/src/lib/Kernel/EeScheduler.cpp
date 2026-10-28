#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#if defined(PS2X_ARMAGEDDON_RELOCATIONS)
#include "relocations.h"
#endif

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    // Armageddon's no-input LGKBM HLE reply completes synchronously, unlike
    // the asynchronous hardware service. Its idle worker (FUN_00523128) can
    // therefore consume every timeslice at a higher priority than the main
    // thread. In the explicitly opt-in HLE discovery profile, lend one
    // scheduler selection to a lower-priority ready thread after that worker
    // exhausts its slice. Normal scheduling remains unchanged.
    int g_armageddonHleSkipPriorityOnce = -1;
    uint32_t g_armageddonHleFairnessTraceCount = 0u;
    std::unordered_map<uint64_t, uint64_t> g_irqHandlerFireCounts;
    std::unordered_map<uint64_t, uint64_t> g_irqDispatchCounts;
    uint64_t g_irqTraceRecords = 0u;

    bool armageddonHleIdleKeyboardPoll(const GuestThread *running, const R5900Context *context)
    {
        static const bool s_fairPollEnabled = (std::getenv("PS2X_ARMAGEDDON_HLE_BOOT") != nullptr &&
                                               std::getenv("PS2X_ARMAGEDDON_HLE_FAIR_POLL") != nullptr);
        if (!s_fairPollEnabled)
            return false;
        if (running != nullptr && (running->entry == 0x00523128u || running->id == 6))
            return true;
        if (context == nullptr)
            return false;
        const uint32_t pc = context->pc;
        const uint32_t ra = getRegU32(context, 31);
        const auto inKeyboardWorker = [](uint32_t address)
        {
            return address >= 0x00523128u && address < 0x00523818u;
        };
        return inKeyboardWorker(pc) || inKeyboardWorker(ra);
    }

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    m_deferredInvocations.clear();
    m_nextDeferredRpcId = 0;
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);
    uint64_t sifSetSregEntries = 0u;
    uint64_t sifChangeSaddrEntries = 0u;
    uint64_t sif0HandlerEntries = 0u;
    bool sifIdleStateLogged = false;
    bool bindRpcFrameWatchActive = false;
    uint32_t bindRpcFrameWatchAddress = 0u;
    uint64_t bindRpcFrameWatchValue = 0u;
    uint64_t bindRpcFrameWatchDispatch = 0u;

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                if (!sifIdleStateLogged && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                {
                    sifIdleStateLogged = true;
                    uint32_t sreg0 = 0u;
                    if (m_rdram != nullptr && 0x006505C0u <= PS2_RAM_SIZE - sizeof(sreg0))
                        std::memcpy(&sreg0, m_rdram + 0x006505C0u, sizeof(sreg0));
                    std::cerr << "[EE_IDLE_STATE] current_thread=" << m_currentThreadId
                              << " pending_invocations=" << m_pendingInvocations.size()
                              << " sreg0=0x" << std::hex << sreg0 << std::dec << std::endl;
                    for (const auto &[id, thread] : m_threads)
                    {
                        std::cerr << "[EE_IDLE_THREAD] id=" << id
                                  << " status=" << static_cast<unsigned>(thread.status)
                                  << " wait_reason=" << static_cast<unsigned>(thread.wait.reason)
                                  << " pc=0x" << std::hex << thread.activeContext().pc << std::dec
                                  << " ra=0x" << std::hex << getRegU32(&thread.activeContext(), 31)
                                  << " sp=0x" << getRegU32(&thread.activeContext(), 29)
                                  << " v0=0x" << getRegU32(&thread.activeContext(), 2)
                                  << " a0=0x" << getRegU32(&thread.activeContext(), 4)
                                  << " a1=0x" << getRegU32(&thread.activeContext(), 5)
                                  << " a2=0x" << getRegU32(&thread.activeContext(), 6)
                                  << " a3=0x" << getRegU32(&thread.activeContext(), 7) << std::dec
                                  << " invocation_depth=" << thread.invocations.size();
                        if (const auto *eventWait = std::get_if<EeEventFlagWait>(&thread.wait.payload))
                            std::cerr << " event_id=" << eventWait->id
                                      << " event_bits=0x" << std::hex << eventWait->bits
                                      << " event_mode=0x" << eventWait->mode << std::dec;
                        else if (const auto *semaphoreWait = std::get_if<EeSemaphoreWait>(&thread.wait.payload))
                            std::cerr << " semaphore_id=" << semaphoreWait->id;
                        std::cerr << std::endl;
                    }
                }
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (std::getenv("PS2X_EE_STARTUP_FLOW_TRACE") != nullptr &&
                running->id == kMainThreadId && context.branch_pc == 0x00114C98u)
            {
                const uint32_t returnSp = getRegU32(&context, 29);
                const uint32_t savedRaAddress = returnSp - 0x10u;
                uint64_t savedRa = 0u;
                bool savedRaValid = false;
                const uint32_t physical = savedRaAddress & 0x1FFFFFFFu;
                if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(savedRa))
                {
                    std::memcpy(&savedRa, m_rdram + physical, sizeof(savedRa));
                    savedRaValid = true;
                }
                std::cerr << "[EE_BINDRPC_RETURN_FRAME] thread=" << running->id
                          << " pc=0x" << std::hex << context.pc
                          << " branch_pc=0x" << context.branch_pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " sp=0x" << returnSp
                          << " saved_ra_addr=0x" << savedRaAddress
                          << " saved_ra=" << (savedRaValid ? "0x" : "<invalid>");
                if (savedRaValid)
                    std::cerr << std::hex << savedRa;
                std::cerr << std::dec << std::endl;
            }
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr && running->id == 1 &&
                !running->invocations.empty() &&
                running->invocations.back().kind == GuestInvocationKind::Interrupt)
            {
                const R5900Context *interrupted = &running->context;
                if (running->invocations.size() > 1u)
                    interrupted = &running->invocations[running->invocations.size() - 2u].context;
                const uint32_t interruptedSp = getRegU32(interrupted, 29);
                const uint32_t savedRaAddress = interruptedSp + 0x30u;
                uint64_t savedRa = 0u;
                bool savedRaValid = false;
                const uint32_t physical = savedRaAddress & 0x1FFFFFFFu;
                if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(savedRa))
                {
                    std::memcpy(&savedRa, m_rdram + physical, sizeof(savedRa));
                    savedRaValid = true;
                }
                std::cerr << "[EE_INTERRUPT_PARENT_STACK_AFTER] thread=" << running->id
                          << " depth=" << running->invocations.size()
                          << " parent_pc=0x" << std::hex << interrupted->pc
                          << " parent_ra=0x" << getRegU32(interrupted, 31)
                          << " parent_sp=0x" << interruptedSp
                          << " saved_ra_addr=0x" << savedRaAddress
                          << " saved_ra=" << (savedRaValid ? "0x" : "<invalid>");
                if (savedRaValid)
                    std::cerr << std::hex << savedRa;
                std::cerr << std::dec << std::endl;
            }
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr &&
                running->id == 1 && running->invocations.empty())
            {
                std::cerr << "[EE_THREAD_DORMANT] id=1 ra=0x" << std::hex
                          << getRegU32(&context, 31)
                          << " sp=0x" << getRegU32(&context, 29)
                          << " branch_pc=0x" << context.branch_pc << std::dec << std::endl;
            }
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

#if defined(PS2X_ARMAGEDDON_RELOCATIONS)
        const uint32_t guestExecPc = armageddon_relocations::guestAlias(
            context.pc, armageddon_relocations::executionPc(&context));
        armageddon_relocations::Scope dispatchScope(&context, guestExecPc, true);
        context.pc = armageddon_relocations::canonical(&m_runtime, context.pc);
#endif
        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty() &&
                m_runtime.missingFunctionPolicy() != PS2Runtime::MissingFunctionPolicy::Stop)
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            applyPendingPreemption();
            continue;
        }

        if (std::getenv("PS2X_EE_STARTUP_FLOW_TRACE") != nullptr &&
            running->id == kMainThreadId && context.pc == 0x00294544u)
        {
            const uint32_t callerSp = getRegU32(&context, 29);
            bindRpcFrameWatchAddress = callerSp - 0x10u;
            const uint32_t physical = bindRpcFrameWatchAddress & 0x1FFFFFFFu;
            bindRpcFrameWatchValue = 0u;
            const bool valid = m_rdram && physical <= PS2_RAM_SIZE - sizeof(bindRpcFrameWatchValue);
            if (valid)
                std::memcpy(&bindRpcFrameWatchValue, m_rdram + physical, sizeof(bindRpcFrameWatchValue));
            bindRpcFrameWatchActive = valid;
            bindRpcFrameWatchDispatch = 0u;
            std::cerr << "[EE_BINDRPC_FRAME_WATCH_BEGIN] thread=" << running->id
                      << " entry_pc=0x" << std::hex << context.pc
                      << " caller_sp=0x" << callerSp
                      << " saved_ra_addr=0x" << bindRpcFrameWatchAddress
                      << " saved_ra=" << (valid ? "0x" : "<invalid>");
            if (valid)
                std::cerr << std::hex << bindRpcFrameWatchValue;
            std::cerr << std::dec << std::endl;
        }

        try
        {
            const uint32_t dispatchEntryPc = context.pc;
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            m_runtime.diagnosticGuestDispatch(&context);
            static uint32_t mainDispatchTraceCount = 0u;
            const bool traceMainDispatch =
                std::getenv("PS2X_EE_MAIN_DISPATCH_TRACE") != nullptr &&
                running->id == kMainThreadId && mainDispatchTraceCount < 4096u;
            if (traceMainDispatch)
            {
                ++mainDispatchTraceCount;
                std::cerr << "[EE_MAIN_DISPATCH_ENTRY] n=" << mainDispatchTraceCount
                          << " pc=0x" << std::hex << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " v0=0x" << getRegU32(&context, 2)
                          << " sp=0x" << getRegU32(&context, 29) << std::dec
                          << " invocation_depth=" << running->invocations.size()
                          << " priority=" << running->currentPriority << std::endl;
            }
            const bool isSif0Handler = context.pc == 0x0010FD30u;
            if (context.pc == 0x0010F5C8u)
                ++sifSetSregEntries;
            else if (context.pc == 0x0010F5E8u)
                ++sifChangeSaddrEntries;
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr &&
                (context.pc == 0x0010F5C8u || context.pc == 0x0010F5E8u))
            {
                std::cerr << "[EE_SIF_CALLBACK_ENTRY] pc=0x" << std::hex << context.pc << std::dec
                          << " count=" << (context.pc == 0x0010F5C8u ? sifSetSregEntries : sifChangeSaddrEntries)
                          << std::endl;
            }
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr && isSif0Handler)
            {
                ++sif0HandlerEntries;
                std::cerr << "[EE_SIF0_HANDLER_ENTRY] count=" << sif0HandlerEntries
                          << " thread=" << m_currentThreadId
                          << " invocation_depth=" << running->invocations.size()
                          << " queued_invocations=" << m_pendingInvocations.size()
                          << " arg0=0x" << std::hex << getRegU32(&context, 4)
                          << " arg1=0x" << getRegU32(&context, 5) << std::dec << std::endl;
            }
            static uint32_t armageddonDmacHandlerTraceCount = 0u;
            const bool traceArmageddonDmacHandler =
                context.pc == 0x001ACA60u && std::getenv("PS2X_ARMAGEDDON_DMA_HANDLER_TRACE") != nullptr &&
                armageddonDmacHandlerTraceCount < 64u;
            auto logArmageddonDmacHandler = [&](const char *phase)
            {
                if (!traceArmageddonDmacHandler)
                    return;
                uint32_t queueRead = 0u, queueWrite = 0u, status = 0u, secondaryStatus = 0u;
                std::memcpy(&queueRead, m_rdram + 0x0064E544u, sizeof(queueRead));
                std::memcpy(&queueWrite, m_rdram + 0x0064E540u, sizeof(queueWrite));
                std::memcpy(&status, m_rdram + 0x0064E554u, sizeof(status));
                std::memcpy(&secondaryStatus, m_rdram + 0x0064E550u, sizeof(secondaryStatus));
                std::cerr << "[Armageddon guest DMAC handler] phase=" << phase
                          << " count=" << armageddonDmacHandlerTraceCount + 1u
                          << " thread=" << running->id
                          << " invocation_depth=" << running->invocations.size()
                          << " cause=0x" << std::hex << getRegU32(&context, 4)
                          << " queue_read=0x" << queueRead << " queue_write=0x" << queueWrite
                          << " status=0x" << status << " secondary_status=0x" << secondaryStatus
                          << " d_stat=0x" << m_runtime.memory().readIORegister(0x1000E010u)
                          << " pc=0x" << context.pc << " ra=0x" << getRegU32(&context, 31)
                          << std::dec << std::endl;
            };
            if (traceArmageddonDmacHandler)
            {
                ++armageddonDmacHandlerTraceCount;
                logArmageddonDmacHandler("entry");
            }
            function(m_rdram, &context, &m_runtime);
            logArmageddonDmacHandler("return");
            if (bindRpcFrameWatchActive && running->id == kMainThreadId)
            {
                ++bindRpcFrameWatchDispatch;
                const uint32_t physical = bindRpcFrameWatchAddress & 0x1FFFFFFFu;
                uint64_t currentSavedRa = 0u;
                const bool valid = m_rdram && physical <= PS2_RAM_SIZE - sizeof(currentSavedRa);
                if (valid)
                    std::memcpy(&currentSavedRa, m_rdram + physical, sizeof(currentSavedRa));
                if (!valid || currentSavedRa != bindRpcFrameWatchValue)
                {
                    std::cerr << "[EE_BINDRPC_FRAME_WATCH_CHANGE] dispatch=" << bindRpcFrameWatchDispatch
                              << " entry_pc=0x" << std::hex << dispatchEntryPc
                              << " return_pc=0x" << context.pc
                              << " branch_pc=0x" << context.branch_pc
                              << " ra=0x" << getRegU32(&context, 31)
                              << " sp=0x" << getRegU32(&context, 29)
                              << " slot=0x" << bindRpcFrameWatchAddress
                              << " old=" << (valid ? "0x" : "<invalid>") << bindRpcFrameWatchValue
                              << " new=" << (valid ? "0x" : "<invalid>");
                    if (valid)
                        std::cerr << std::hex << currentSavedRa;
                    std::cerr << std::dec << std::endl;
                    if (valid)
                        bindRpcFrameWatchValue = currentSavedRa;
                    else
                        bindRpcFrameWatchActive = false;
                }
            }
            if (traceMainDispatch)
            {
                std::cerr << "[EE_MAIN_DISPATCH_RETURN] n=" << mainDispatchTraceCount
                          << " pc=0x" << std::hex << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " v0=0x" << getRegU32(&context, 2)
                          << " sp=0x" << getRegU32(&context, 29) << std::dec
                          << " invocation_depth=" << running->invocations.size()
                          << " stop_requested=" << (m_stopRequested.load(std::memory_order_acquire) ? "true" : "false")
                          << std::endl;
            }
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr && isSif0Handler)
            {
                std::cerr << "[EE_SIF0_HANDLER_RETURN] thread=" << m_currentThreadId
                          << " next_pc=0x" << std::hex << context.pc << std::dec
                          << " invocation_depth=" << running->invocations.size()
                          << " queued_invocations=" << m_pendingInvocations.size()
                          << std::endl;
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            if (std::getenv("PS2X_EE_MAIN_DISPATCH_TRACE") != nullptr &&
                running->id == kMainThreadId)
            {
                std::cerr << "[EE_MAIN_DISPATCH_TRANSFER] pc=0x" << std::hex << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " v0=0x" << getRegU32(&context, 2) << std::dec
                          << " invocation_depth=" << running->invocations.size() << std::endl;
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
    if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
        std::cerr << "[SIF_TOP_LEVEL_ENTRY_COUNTS] set_sreg=" << sifSetSregEntries
                  << " change_saddr=" << sifChangeSaddrEntries
                  << " sif0_handler=" << sif0HandlerEntries << std::endl;
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    R5900Context *const traceContext = currentContext();
    const bool traceRenderWait =
        std::getenv("PS2X_ARMAGEDDON_CHECKPOINT_TRACE") != nullptr && traceContext != nullptr &&
        (traceContext->pc == 0x001AD5FCu || traceContext->pc == 0x001AD630u);
    static uint32_t renderWaitCheckpointTraceCount = 0u;
    const uint64_t cycleBefore = m_eeCycle;
    const auto traceRenderCheckpoint = [&](const char *reason)
    {
        if (!traceRenderWait || renderWaitCheckpointTraceCount >= 128u)
            return;
        ++renderWaitCheckpointTraceCount;
        const GuestThread *const thread = currentThread();
        std::cerr << "[Armageddon HLE render-wait checkpoint] n=" << renderWaitCheckpointTraceCount
                  << " pc=0x" << std::hex << traceContext->pc
                  << " thread=" << std::dec << (thread ? thread->id : 0u)
                  << " cycles=" << cycles
                  << " cycle_before=" << cycleBefore
                  << " cycle_after=" << m_eeCycle
                  << " slice_end=" << m_sliceEndCycle
                  << " pending=" << (m_checkpointPending.load(std::memory_order_acquire) ? "true" : "false")
                  << " stop=" << (m_stopRequested.load(std::memory_order_acquire) ? "true" : "false")
                  << " reason=" << reason << std::endl;
    };

    if (m_runtime.diagnosticCheckpoint(traceContext, cycles))
    {
        traceRenderCheckpoint("diagnostic");
        return true;
    }
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        traceRenderCheckpoint("pending-or-stop");
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        traceRenderCheckpoint("event-deadline");
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        traceRenderCheckpoint("within-slice");
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && armageddonHleIdleKeyboardPoll(running, traceContext))
    {
        for (int priority = running->currentPriority; priority < kPriorityCount; ++priority)
        {
            const auto &queue = m_readyQueues[static_cast<size_t>(priority)];
            const bool hasOther = (priority > running->currentPriority && !queue.empty()) ||
                                  (priority == running->currentPriority && queue.size() > 0u);
            if (hasOther)
            {
                m_rescheduleRequested = true;
                m_timeSliceExpired = true;
                if (priority > running->currentPriority)
                    g_armageddonHleSkipPriorityOnce = running->currentPriority;
                if (g_armageddonHleFairnessTraceCount < 32u)
                {
                    ++g_armageddonHleFairnessTraceCount;
                    std::cerr << "[Armageddon HLE fairness handoff] count="
                              << g_armageddonHleFairnessTraceCount
                              << " reason=synchronous-no-input-LGKBM-poll"
                              << " thread=" << running->id
                              << " pc=0x" << std::hex << (traceContext ? traceContext->pc : 0u)
                              << " priority=" << std::dec << running->currentPriority
                              << " ready_priority=" << priority << std::endl;
                }
                traceRenderCheckpoint("Armageddon-HLE-keyboard-poll-fairness");
                return true;
            }
        }
    }

    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        traceRenderCheckpoint("timeslice-ready-thread");
        return true;
    }

    renewTimeSlice();
    traceRenderCheckpoint("renewed-slice-no-ready-thread");
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    // Priority 0 is the highest valid EE thread priority. The SDK's InitThread
    // path creates its kernel thread at priority 0 before lowering the caller.
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueDeferredInvocation(GuestInvocation invocation, uint64_t delayCycles)
{
    assertExecutor();
    const uint64_t deadlineCycle = m_eeCycle + (delayCycles > 0u ? delayCycles : 1u);
    const auto hostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(delayCycles);
    const uint32_t id = ++m_nextDeferredRpcId;
    m_deferredInvocations.emplace(id, std::move(invocation));
    scheduleEvent(deadlineCycle, hostDeadline, EeEvent{EeEventType::RpcCallback, id, 0});
}

void EeScheduler::queueDeferredAction(std::function<void()> action, uint64_t delayCycles)
{
    assertExecutor();
    const uint64_t deadlineCycle = m_eeCycle + (delayCycles > 0u ? delayCycles : 1u);
    const auto hostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(delayCycles);
    const uint32_t id = ++m_nextDeferredActionId;
    m_deferredActions.emplace(id, std::move(action));
    scheduleEvent(deadlineCycle, hostDeadline, EeEvent{EeEventType::DeferredAction, id, 0});
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    const int32_t threadId = owner ? owner->id : 0;
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(threadId)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    std::cerr << "[EE_IRQ_REGISTER] type=" << (dmac ? "DMAC" : "INTC")
              << " cause=" << cause
              << " id=" << id
              << " handler=0x" << std::hex << handler
              << " arg=0x" << argument
              << " gp=0x" << gp
              << " sp=0x" << sp << std::dec << std::endl;
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    // Hardware status latches on the event even when no guest callback is
    // installed or the interrupt source is masked. Armageddon polls
    // INTC_STAT directly during startup (0x0010CCB0), so publishing only
    // enabled callbacks leaves its wait loop stuck forever.
    if (!dmac && cause < 15u)
        m_runtime.memory().writeIORegister(0xFFFFFFFCu, 1u << cause);
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    const bool traceSif0 = dmac && cause == 5u && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr;
    const bool traceArmageddonDma = dmac && cause <= 2u &&
                                    std::getenv("PS2X_ARMAGEDDON_DMA_IRQ_TRACE") != nullptr;
    const uint64_t dispatchKey = (static_cast<uint64_t>(dmac ? 1u : 0u) << 63u) |
                                 (static_cast<uint64_t>(cause & 0x7FFFFFFFu) << 32u);
    const uint64_t dispatchCount = ++g_irqDispatchCounts[dispatchKey];
    const char *irqTrace = std::getenv("PS2X_EE_IRQ_TRACE");
    if (irqTrace && irqTrace[0] != '\0' && irqTrace[0] != '0' && g_irqTraceRecords < 8192u)
    {
        ++g_irqTraceRecords;
        std::cerr << "[EE_IRQ_EVENT] type=" << (dmac ? "DMAC" : "INTC")
                  << " cause=" << cause
                  << " dispatch_count=" << dispatchCount
                  << " cause_enabled=" << ((cause >= 32u || (mask & (1u << cause)) != 0u) ? "true" : "false")
                  << " tick=" << m_vsyncTick
                  << " intc_stat=0x" << std::hex << m_runtime.memory().readIORegister(0x1000F000u)
                  << " intc_mask=0x" << m_runtime.memory().readIORegister(0x1000F010u)
                  << " d_stat=0x" << m_runtime.memory().readIORegister(0x1000E010u)
                  << std::dec << '\n';
    }
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        if (traceArmageddonDma)
        {
            size_t registered = 0u;
            size_t enabled = 0u;
            for (const auto &[id, handler] : m_dmacHandlers)
            {
                (void)id;
                if (handler.cause == cause)
                {
                    ++registered;
                    enabled += handler.enabled ? 1u : 0u;
                }
            }
            std::cerr << "[Armageddon DMAC IRQ] cause=" << cause
                      << " enabled_mask=0x" << std::hex << mask << std::dec
                      << " cause_enabled=false registered_handlers=" << registered
                      << " enabled_handlers=" << enabled << std::endl;
        }
        if (traceSif0)
            std::cerr << "[EE_SIF0_IRQ] cause=5 enabled=false handlers=0" << std::endl;
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    if (traceArmageddonDma)
    {
        std::cerr << "[Armageddon DMAC IRQ] cause=" << cause
                  << " enabled_mask=0x" << std::hex << mask << std::dec
                  << " cause_enabled=true registered_handlers=" << matching.size();
        for (const EeIrqHandler &handler : matching)
            std::cerr << " handler=0x" << std::hex << handler.handler << std::dec;
        std::cerr << std::endl;
    }
    if (traceSif0)
    {
        std::cerr << "[EE_SIF0_IRQ] cause=5 enabled=true handlers=" << matching.size();
        for (const EeIrqHandler &handler : matching)
            std::cerr << " handler=0x" << std::hex << handler.handler << std::dec;
        std::cerr << std::endl;

        if (const GuestThread *owner = currentThread(); owner && owner->id == 1)
        {
            const R5900Context &interrupted = owner->activeContext();
            const uint32_t interruptedSp = getRegU32(&interrupted, 29);
            const uint32_t savedRaAddress = interruptedSp + 0x30u;
            uint64_t savedRa = 0u;
            bool savedRaValid = false;
            const uint32_t physical = savedRaAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(savedRa))
            {
                std::memcpy(&savedRa, m_rdram + physical, sizeof(savedRa));
                savedRaValid = true;
            }
            std::cerr << "[EE_INTERRUPT_PARENT_STACK_BEFORE] thread=" << owner->id
                      << " depth=" << owner->invocations.size()
                      << " parent_pc=0x" << std::hex << interrupted.pc
                      << " parent_ra=0x" << getRegU32(&interrupted, 31)
                      << " parent_sp=0x" << interruptedSp
                      << " saved_ra_addr=0x" << savedRaAddress
                      << " saved_ra=" << (savedRaValid ? "0x" : "<invalid>");
            if (savedRaValid)
                std::cerr << std::hex << savedRa;
            std::cerr << std::dec << std::endl;
        }

        // Keep the first SIF0 command packet and the guest's registered dispatch table
        // observable at the actual EE interrupt boundary. This is diagnostic-only and
        // reads guest RAM; it does not alter dispatch or packet contents.
        if (m_rdram != nullptr)
        {
            auto readGuest32 = [this](uint32_t address, uint32_t &value)
            {
                const uint32_t physical = address & 0x1FFFFFFFu;
                if (physical > PS2_RAM_SIZE - sizeof(value))
                    return false;
                std::memcpy(&value, m_rdram + physical, sizeof(value));
                return true;
            };
            uint32_t packetAddress = 0u;
            uint32_t callbackTable = 0u;
            (void)readGuest32(0x00650418u, packetAddress);
            (void)readGuest32(0x00650424u, callbackTable);
            std::cerr << "[EE_SIF0_PACKET] packet_ptr=0x" << std::hex << packetAddress
                      << " callback_table=0x" << callbackTable << " data=";
            const uint32_t packetPhysical = packetAddress & 0x1FFFFFFFu;
            if (packetAddress != 0u && packetPhysical <= PS2_RAM_SIZE - 32u)
            {
                for (uint32_t i = 0u; i < 32u; ++i)
                    std::cerr << std::setw(2) << std::setfill('0')
                              << static_cast<uint32_t>(m_rdram[packetPhysical + i]);
            }
            else
            {
                std::cerr << "<invalid>";
            }
            std::cerr << " cmd_data_words=";
            for (uint32_t word = 0u; word < 7u; ++word)
            {
                uint32_t value = 0u;
                (void)readGuest32(0x00650418u + word * 4u, value);
                if (word != 0u)
                    std::cerr << ',';
                std::cerr << "0x" << std::hex << value;
            }
            const uint32_t commandIds[] = {1u, 2u, 8u, 9u};
            for (const uint32_t command : commandIds)
            {
                uint32_t handlerAddress = 0u;
                uint32_t argument = 0u;
                uint32_t gp = 0u;
                (void)readGuest32(callbackTable + command * 12u, handlerAddress);
                (void)readGuest32(callbackTable + command * 12u + 4u, argument);
                (void)readGuest32(callbackTable + command * 12u + 8u, gp);
                std::cerr << " cmd" << command << "={handler=0x" << std::hex << handlerAddress
                          << ",arg=0x" << argument << ",gp=0x" << gp << "}";
            }
            std::cerr << std::dec << std::setfill(' ') << std::endl;
        }
    }
    for (const EeIrqHandler &handler : matching)
    {
        const uint64_t key = (static_cast<uint64_t>(dmac ? 1u : 0u) << 63u) |
                             (static_cast<uint64_t>(cause & 0x7FFFFFFFu) << 32u) |
                             static_cast<uint32_t>(handler.id);
        const uint64_t fireCount = ++g_irqHandlerFireCounts[key];
        const char *irqTrace = std::getenv("PS2X_EE_IRQ_TRACE");
        if (irqTrace && irqTrace[0] != '\0' && irqTrace[0] != '0' && g_irqTraceRecords < 8192u)
        {
            ++g_irqTraceRecords;
            uint32_t interruptedPc = 0u;
            uint32_t interruptedRa = 0u;
            if (const GuestThread *owner = currentThread())
            {
                interruptedPc = owner->activeContext().pc;
                interruptedRa = getRegU32(&owner->activeContext(), 31);
            }
            std::cerr << "[EE_IRQ_FIRE] type=" << (dmac ? "DMAC" : "INTC")
                      << " cause=" << cause
                      << " handler_id=" << handler.id
                      << " handler=0x" << std::hex << handler.handler
                      << " arg=0x" << handler.argument
                      << " source_pc=0x" << interruptedPc
                      << " source_ra=0x" << interruptedRa
                      << " d_stat=0x" << m_runtime.memory().readIORegister(0x1000E010u)
                      << " intc_stat=0x" << m_runtime.memory().readIORegister(0x1000F000u)
                      << std::dec << " fire_count=" << fireCount << '\n';
        }
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        // A hardware interrupt nests on the interrupted EE stack unless the
        // handler was explicitly registered with its own stack. Starting at
        // the shared async-callback stack top can overlap a live guest frame
        // (the bounded Armageddon SIF0 trace caught its saved $ra being erased).
        uint32_t interruptSp = handler.sp;
        if (interruptSp == 0u)
        {
            if (const GuestThread *owner = currentThread())
                interruptSp = getRegU32(&owner->activeContext(), 29);
        }
        SET_GPR_U32(&invocation.context, 29, interruptSp);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

bool EeScheduler::hasPendingInterrupt() const noexcept
{
    for (const auto &inv : m_pendingInvocations)
    {
        if (inv.kind == GuestInvocationKind::Interrupt)
            return true;
    }
    return false;
}

size_t EeScheduler::registeredHandlerCount(bool dmac, uint32_t cause) const noexcept
{
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    size_t count = 0;
    for (const auto &[id, handler] : handlers)
    {
        if (handler.cause == cause)
            ++count;
    }
    return count;
}

void EeScheduler::dispatchPendingInterrupts(uint8_t *rdram, R5900Context *callerContext)
{
    assertExecutor();
    if (m_insideInterrupt)
    {
        return;
    }

    while (!m_pendingInvocations.empty())
    {
        auto it = std::find_if(m_pendingInvocations.begin(), m_pendingInvocations.end(),
                               [](const GuestInvocation &inv) {
                                   return inv.kind == GuestInvocationKind::Interrupt;
                               });
        if (it == m_pendingInvocations.end())
            break;

        GuestInvocation invocation = std::move(*it);
        m_pendingInvocations.erase(it);

        const bool wasInside = m_insideInterrupt;
        m_insideInterrupt = true;
        uint8_t *effectiveRdram = rdram != nullptr ? rdram : m_rdram;

        static uint32_t armageddonDmacHandlerTraceCount = 0u;
        const bool traceArmageddonDmacHandler =
            invocation.context.pc == 0x001ACA60u && std::getenv("PS2X_ARMAGEDDON_DMA_HANDLER_TRACE") != nullptr &&
            armageddonDmacHandlerTraceCount < 64u;
        auto logArmageddonDmacHandler = [&](const char *phase)
        {
            if (!traceArmageddonDmacHandler || effectiveRdram == nullptr)
                return;
            uint32_t queueRead = 0u, queueWrite = 0u, status = 0u, secondaryStatus = 0u;
            std::memcpy(&queueRead, effectiveRdram + 0x0064E544u, sizeof(queueRead));
            std::memcpy(&queueWrite, effectiveRdram + 0x0064E540u, sizeof(queueWrite));
            std::memcpy(&status, effectiveRdram + 0x0064E554u, sizeof(status));
            std::memcpy(&secondaryStatus, effectiveRdram + 0x0064E550u, sizeof(secondaryStatus));
            std::cerr << "[Armageddon guest DMAC handler (preempt)] phase=" << phase
                      << " count=" << armageddonDmacHandlerTraceCount + 1u
                      << " cause=0x" << std::hex << getRegU32(&invocation.context, 4)
                      << " queue_read=0x" << queueRead << " queue_write=0x" << queueWrite
                      << " status=0x" << status << " secondary_status=0x" << secondaryStatus
                      << " d_stat=0x" << m_runtime.memory().readIORegister(0x1000E010u)
                      << " pc=0x" << invocation.context.pc << " ra=0x" << getRegU32(&invocation.context, 31)
                      << std::dec << std::endl;
        };

        if (traceArmageddonDmacHandler)
        {
            ++armageddonDmacHandlerTraceCount;
            logArmageddonDmacHandler("entry");
        }

        PS2Runtime::RecompiledFunction fn = m_runtime.lookupFunction(invocation.context.pc);
        if (fn)
        {
            fn(effectiveRdram, &invocation.context, &m_runtime);
        }
        else
        {
            m_runtime.reportMissingFunction(effectiveRdram,
                                            &invocation.context,
                                            invocation.context.pc,
                                            invocation.context.pc,
                                            PS2Runtime::GuestBranchKind::DirectJump,
                                            "EE interrupt dispatch");
        }

        if (traceArmageddonDmacHandler)
        {
            logArmageddonDmacHandler("return");
        }

        m_insideInterrupt = wasInside;

        if (invocation.onComplete && callerContext != nullptr)
        {
            try
            {
                invocation.onComplete(invocation.context, *callerContext);
            }
            catch (const EeDispatcherTransfer &)
            {
            }
        }
    }

    if (m_pendingInvocations.empty() && m_eeCycle < m_nextDeadlineCycle.load(std::memory_order_relaxed))
    {
        m_checkpointPending.store(false, std::memory_order_release);
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    if (g_armageddonHleSkipPriorityOnce >= 0)
    {
        const int firstLowerPriority = std::clamp(g_armageddonHleSkipPriorityOnce + 1,
                                                  0,
                                                  kPriorityCount - 1);
        g_armageddonHleSkipPriorityOnce = -1;
        for (int priority = firstLowerPriority; priority < kPriorityCount; ++priority)
        {
            auto &queue = m_readyQueues[static_cast<size_t>(priority)];
            if (queue.empty())
                continue;
            const int id = queue.front();
            queue.pop_front();
            GuestThread *selected = thread(id);
            assert(selected != nullptr);
            assert(selected->status == EeThreadStatus::Ready);
            return selected;
        }
    }

    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                const bool cycleDue = item.deadlineCycle <= m_eeCycle;
                const bool hostDue = item.hostDeadline <= now;
                if ((cycleDue || hostDue) &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return (item.deadlineCycle <= m_eeCycle) ||
                                                       (item.hostDeadline <= pacedNow); });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.deadlineCycle > m_eeCycle)
            {
                m_eeCycle = scheduled.deadlineCycle;
            }
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::RpcCallback:
    {
        auto it = m_deferredInvocations.find(event.id);
        if (it != m_deferredInvocations.end())
        {
            GuestInvocation inv = std::move(it->second);
            m_deferredInvocations.erase(it);
            queueInvocation(std::move(inv));
        }
        break;
    }
    case EeEventType::DeferredAction:
    {
        auto it = m_deferredActions.find(event.id);
        if (it != m_deferredActions.end())
        {
            auto action = std::move(it->second);
            m_deferredActions.erase(it);
            if (action)
            {
                action();
            }
        }
        break;
    }
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
