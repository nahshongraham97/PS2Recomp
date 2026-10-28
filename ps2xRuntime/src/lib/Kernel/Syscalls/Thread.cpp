#include "Common.h"
#include "Thread.h"
#include "runtime/ee_scheduler.h"

#include <cstdlib>
#include <iostream>

namespace ps2_syscalls
{
    namespace
    {
        bool eeThreadTraceEnabled()
        {
            const char *value = std::getenv("PS2X_EE_THREAD_TRACE");
            return value && value[0] == '1';
        }

        bool eeExitTraceEnabled()
        {
            const char *value = std::getenv("PS2X_EE_EXIT_TRACE");
            return value && value[0] == '1';
        }

        EeScheduler &scheduler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            EeScheduler &result = runtime->eeScheduler();
            result.bindMainContextForSyscall(*ctx, rdram);
            return result;
        }

        int rawThreadStatus(EeThreadStatus status)
        {
            switch (status)
            {
            case EeThreadStatus::Running:
                return THS_RUN;
            case EeThreadStatus::Ready:
                return THS_READY;
            case EeThreadStatus::Waiting:
                return THS_WAIT;
            case EeThreadStatus::WaitingSuspended:
                return THS_WAITSUSPEND;
            case EeThreadStatus::Suspended:
                return THS_SUSPEND;
            case EeThreadStatus::Dormant:
                return THS_DORMANT;
            }
            return THS_DORMANT;
        }

        int rawWaitType(EeWaitReason reason)
        {
            switch (reason)
            {
            case EeWaitReason::Sleep:
                return TSW_SLEEP;
            case EeWaitReason::Semaphore:
                return TSW_SEMA;
            case EeWaitReason::EventFlag:
                return TSW_EVENT;
            case EeWaitReason::VSync:
                return 4;
            case EeWaitReason::External:
            case EeWaitReason::Mpeg:
                return 5;
            case EeWaitReason::None:
                return TSW_NONE;
            }
            return TSW_NONE;
        }

        int waitId(const GuestThread &thread)
        {
            if (thread.wait.reason == EeWaitReason::Semaphore)
            {
                return std::get<EeSemaphoreWait>(thread.wait.payload).id;
            }
            if (thread.wait.reason == EeWaitReason::EventFlag)
            {
                return std::get<EeEventFlagWait>(thread.wait.payload).id;
            }
            return 0;
        }

        [[noreturn]] void exitThreadWithHandlers(int tid,
                                                 R5900Context *ctx,
                                                 PS2Runtime *runtime,
                                                 bool deleteThread)
        {
            EeScheduler &ee = runtime->eeScheduler();
            const auto handlers = runtime->takeEeExitHandlers(tid);
            std::vector<GuestInvocation> invocations;
            invocations.reserve(handlers.size());
            for (const PS2Runtime::EeExitHandlerRegistration &handler : handlers)
            {
                if (handler.function == 0u || !runtime->hasFunction(handler.function))
                {
                    continue;
                }
                GuestInvocation invocation{};
                invocation.kind = GuestInvocationKind::ExitHandler;
                invocation.context = *ctx;
                invocation.context.pc = handler.function;
                SET_GPR_U32(&invocation.context, 4, handler.argument);
                SET_GPR_U32(&invocation.context, 29, ee.invocationStackTop());
                SET_GPR_U32(&invocation.context, 31, 0u);
                invocations.push_back(std::move(invocation));
            }
            if (invocations.empty())
            {
                ee.exitCurrent(deleteThread);
            }
            invocations.back().onComplete = [runtime, deleteThread](const R5900Context &, R5900Context &)
            {
                runtime->eeScheduler().exitCurrent(deleteThread);
            };
            ee.invokeCurrentSequence(std::move(invocations));
        }

        void changePriorityImpl(uint8_t *rdram,
                                R5900Context *ctx,
                                PS2Runtime *runtime,
                                bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int id = static_cast<int>(getRegU32(ctx, 4));
            const int priority = static_cast<int>(getRegU32(ctx, 5));
            int oldPriority = 0;
            const int result = ee.changePriority(id, priority, interruptSafe, oldPriority);
            if (eeThreadTraceEnabled())
            {
                std::cerr << "[EE_THREAD_TRACE] ChangeThreadPriority pc=0x" << std::hex
                          << getRegU32(ctx, 31) << std::dec << " id=" << id
                          << " priority=" << priority << " result=" << result
                          << " old=" << oldPriority << std::endl;
            }
            setReturnS32(ctx, result < 0 ? result : oldPriority);
            ee.transferIfRequested(interruptSafe);
        }

        void rotateReadyQueueImpl(uint8_t *rdram,
                                  R5900Context *ctx,
                                  PS2Runtime *runtime,
                                  bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.rotateReadyQueue(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void wakeupThreadImpl(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int targetThread = static_cast<int>(getRegU32(ctx, 4));
            const char *trace = std::getenv("PS2X_EE_SEMA_TRACE");
            if (trace && trace[0] != '\0' && trace[0] != '0')
            {
                static uint32_t records = 0u;
                if (records < 512u)
                {
                    ++records;
                    std::cerr << "[EE_SEMA_TRACE] op=" << (interruptSafe ? "iwakeup-before" : "wakeup-before")
                              << " target_thread=" << targetThread
                              << " pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << std::dec << " current_thread=" << ee.currentThreadId() << '\n';
                }
            }
            const int result = ee.wakeupThread(targetThread, interruptSafe);
            if (trace && trace[0] != '\0' && trace[0] != '0')
            {
                static uint32_t recordsAfter = 0u;
                if (recordsAfter < 512u)
                {
                    ++recordsAfter;
                    std::cerr << "[EE_SEMA_TRACE] op=" << (interruptSafe ? "iwakeup-after" : "wakeup-after")
                              << " target_thread=" << targetThread
                              << " result=" << result
                              << " pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << std::dec << " current_thread=" << ee.currentThreadId() << '\n';
                }
            }
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void releaseWaitImpl(uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime,
                             bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.releaseWait(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }
    }

    void FlushCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        FlushCache(rdram, ctx, runtime);
    }

    void EnableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void DisableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void ResetEE(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void SetMemoryMode(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void InitThread(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, EeScheduler::kMainThreadId);
    }

    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t address = getRegU32(ctx, 4);
        if (address == 0u)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        const auto *param = getEeGuestStruct<ee_thread_t>(rdram, address);
        if (!param)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        if (param->stack_size < 0)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (param->stack != 0u)
        {
            uint32_t stackOffset = 0u;
            bool scratch = false;
            if (!resolveEeGuestRange(param->stack,
                                     static_cast<size_t>(param->stack_size),
                                     stackOffset,
                                     scratch))
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }
        }

        // PS2SDK EE t_ee_thread: status, func, stack, stack_size, gp_reg,
        // initial_priority, current_priority, attr, option.
        const EeThreadCreateParams decoded{
            param->attr,
            param->func,
            param->stack,
            static_cast<uint32_t>(param->stack_size),
            param->gp_reg,
            param->initial_priority,
            param->option,
        };
        const int result = scheduler(rdram, ctx, runtime).createThread(decoded);
        if (eeThreadTraceEnabled())
        {
            std::cerr << "[EE_THREAD_TRACE] CreateThread pc=0x" << std::hex << getRegU32(ctx, 31)
                      << " desc=0x" << address << " entry=0x" << decoded.entry
                      << " stack=0x" << decoded.stack << " stack_size=0x" << decoded.stackSize
                      << " gp=0x" << decoded.gp << std::dec
                      << " initial_priority=" << decoded.priority
                      << " attr=0x" << std::hex << decoded.attr << " option=0x" << decoded.option
                      << std::dec << " result=" << result << std::endl;
        }
        setReturnS32(ctx, result);
    }

    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        uint32_t ownedStack = 0;
        const int result = ee.deleteThread(id, ownedStack);
        if (result == KE_OK)
        {
            runtime->removeEeExitHandlers(id);
        }
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        const uint32_t arg = getRegU32(ctx, 5);
        const auto traceStart = [&](int result, const char *reason)
        {
            if (eeThreadTraceEnabled())
            {
                const GuestThread *traceTarget = ee.thread(id);
                std::cerr << "[EE_THREAD_TRACE] StartThread pc=0x" << std::hex << getRegU32(ctx, 31)
                          << std::dec << " id=" << id << " arg=0x" << std::hex << arg << std::dec
                          << " result=" << result << " reason=" << reason
                          << " status=" << (traceTarget ? rawThreadStatus(traceTarget->status) : -1)
                          << " entry=0x" << std::hex << (traceTarget ? traceTarget->entry : 0u)
                          << std::dec << std::endl;
            }
        };
        GuestThread *target = ee.thread(id);
        if (!target)
        {
            traceStart(KE_UNKNOWN_THID, "unknown_thread");
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        if (target->status != EeThreadStatus::Dormant)
        {
            traceStart(KE_NOT_DORMANT, "not_dormant");
            setReturnS32(ctx, KE_NOT_DORMANT);
            return;
        }
        if (!runtime->hasFunction(target->entry))
        {
            traceStart(KE_ERROR, "unregistered_entry");
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (target->stack == 0u && target->stackSize != 0u)
        {
            target->stack = runtime->guestMalloc(target->stackSize, 16u);
            if (target->stack == 0u)
            {
                traceStart(KE_ERROR, "guest_stack_allocation_failed");
                setReturnS32(ctx, KE_ERROR);
                return;
            }
            target->ownsStack = true;
        }
        const int result = ee.startThread(id, arg, *ctx, false);
        traceStart(result, result == KE_OK ? "queued" : "scheduler_rejected");
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        if (eeExitTraceEnabled())
        {
            std::cerr << "[EE_EXIT_TRACE] ExitThread tid=" << ee.currentThreadId()
                      << " pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " a0_status=0x" << getRegU32(ctx, 4)
                      << " v0=0x" << getRegU32(ctx, 2)
                      << " v1=0x" << getRegU32(ctx, 3)
                      << " sp=0x" << getRegU32(ctx, 29)
                      << std::dec << std::endl;
        }
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, false);
    }

    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, true);
    }

    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        uint32_t ownedStack = 0;
        const int result = ee.terminateThread(static_cast<int>(getRegU32(ctx, 4)), ownedStack, false);
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.suspendThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.resumeThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, scheduler(rdram, ctx, runtime).currentThreadId());
    }

    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        int id = static_cast<int>(getRegU32(ctx, 4));
        if (id == 0)
        {
            id = ee.currentThreadId();
        }
        const GuestThread *thread = ee.thread(id);
        if (!thread)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        auto *status = getEeGuestStruct<ee_thread_status_t>(rdram, getRegU32(ctx, 5));
        if (!status)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        *status = {};
        status->status = rawThreadStatus(thread->status);
        status->func = thread->entry;
        status->stack = thread->stack;
        status->stack_size = static_cast<int>(thread->stackSize);
        status->gp_reg = thread->gp;
        status->initial_priority = thread->initialPriority;
        status->current_priority = thread->currentPriority;
        status->attr = thread->attr;
        status->option = thread->option;
        status->waitType = rawWaitType(thread->wait.reason);
        status->waitId = waitId(*thread);
        status->wakeupCount = thread->wakeupCount;
        setReturnS32(ctx, KE_OK);
    }

    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReferThreadStatus(rdram, ctx, runtime);
    }

    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        ee.sleepCurrent();
        setReturnS32(ctx, KE_OK);
    }

    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, false);
    }

    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, true);
    }

    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx,
                     scheduler(rdram, ctx, runtime).cancelWakeup(static_cast<int>(getRegU32(ctx, 4))));
    }

    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == 0u)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }
        CancelWakeupThread(rdram, ctx, runtime);
    }

    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        changePriorityImpl(rdram, ctx, runtime, false);
    }

    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        changePriorityImpl(rdram, ctx, runtime, true);
    }

    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, false);
    }

    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, true);
    }

    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, false);
    }

    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, true);
    }
}
