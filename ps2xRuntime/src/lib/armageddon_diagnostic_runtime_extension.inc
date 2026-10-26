// Compiled as part of ps2_runtime.cpp; uses the actual runtime's private state.
bool PS2Runtime::initializeHeadless()
{
    try
    {
        if (!m_memory.initialize() || !syncCoreSubsystems())
            return false;
        // Headless diagnostics intentionally have no host sound device/window.
        // Existing guest audio/syscall implementations are not replaced here.
        m_audioBackend.setAudioReady(false);
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Headless runtime initialization failed: " << error.what() << std::endl;
        return false;
    }
}

void PS2Runtime::diagnosticCapture(R5900Context *context)
{
    if (!m_diagnosticActive || !context || m_diagnosticOutcome.stopReason != "running")
        return;
    m_diagnosticOutcome.context = *context;
    m_diagnosticOutcome.contextValid = true;
    m_diagnosticOutcome.threadId = m_eeScheduler->currentThreadId();
}

void PS2Runtime::diagnosticGuestDispatch(R5900Context *context)
{
    if (!m_diagnosticActive)
        return;
    diagnosticCapture(context);
    // A journal failure before invocation must not count an uncalled handler.
    if (m_diagnosticJournal && m_diagnosticOutcome.guestDispatchesStarted == 0)
        m_diagnosticJournal(m_diagnosticOutcome);
    ++m_diagnosticOutcome.guestDispatchesStarted;
}

bool PS2Runtime::diagnosticCheckpoint(R5900Context *context, uint32_t estimatedCycles) noexcept
{
    if (!m_diagnosticActive)
        return false;
    if (m_diagnosticOutcome.stopReason != "running")
        return true;
    diagnosticCapture(context);
    const bool limitReached = m_diagnosticBudget->charge(estimatedCycles);
    m_diagnosticOutcome.checkpoints = m_diagnosticBudget->checkpoints;
    m_diagnosticOutcome.checkpointCharges = m_diagnosticBudget->charges;
    if (limitReached)
    {
        if (m_diagnosticOutcome.stopReason == "running")
            m_diagnosticOutcome.stopReason = m_diagnosticBudget->stopReason;
        requestStop();
    }
    if (m_diagnosticJournal &&
        (limitReached || m_diagnosticOutcome.checkpoints == 1 ||
         (m_diagnosticOptions.journalEvery != 0 &&
          m_diagnosticOutcome.checkpoints % m_diagnosticOptions.journalEvery == 0)))
    {
        // checkpointDue/eeCheckpointDue are noexcept in the pinned runtime.
        // In particular, disk-full/journal errors must not escape that boundary.
        if (!armageddonTryDiagnosticJournal(m_diagnosticJournal, m_diagnosticOutcome))
        {
            m_diagnosticOutcome.stopReason = "journal_failed";
            m_diagnosticOutcome.error.clear();
            requestStop();
            return true;
        }
    }
    return limitReached;
}

bool PS2Runtime::diagnosticMissingSyscallOverride(R5900Context *context,
                                                uint32_t syscall, uint32_t handler)
{
    if (!m_diagnosticActive)
        return false;
    if (m_diagnosticOutcome.stopReason == "running")
    {
        m_diagnosticOutcome.missingSyscallOverride = syscall;
        m_diagnosticOutcome.missingSyscallOverrideValid = true;
        m_diagnosticOutcome.error = "Unregistered guest syscall override handler";
        reportMissingFunction(m_memory.getRDRAM(), context, handler,
                              context ? context->pc : 0u,
                              GuestBranchKind::IndirectCall, "syscall override");
    }
    return true;
}

bool PS2Runtime::diagnosticUnsupportedSyscall(R5900Context *context, uint32_t syscall)
{
    if (!m_diagnosticActive)
        return false;
    if (m_diagnosticOutcome.stopReason != "running")
        return true;
    diagnosticCapture(context);
    m_diagnosticOutcome.stopReason = "unsupported_syscall";
    m_diagnosticOutcome.unsupportedSyscall = syscall;
    m_diagnosticOutcome.unsupportedSyscallValid = true;
    requestStop();
    return true;
}

bool PS2Runtime::diagnosticUnsupportedStub(R5900Context *context, const char *name)
{
    if (!m_diagnosticActive)
        return false;
    if (m_diagnosticOutcome.stopReason != "running")
        return true;
    diagnosticCapture(context);
    m_diagnosticOutcome.stopReason = "unsupported_named_stub";
    m_diagnosticOutcome.error = name ? name : "unknown";
    requestStop();
    return true;
}

PS2Runtime::DiagnosticOutcome PS2Runtime::runBoundedDiagnostic(
    const DiagnosticOptions &options,
    std::function<void(const DiagnosticOutcome &)> journal)
{
#if defined(PS2X_ARMAGEDDON_RELOCATIONS)
    armageddon_relocations::reset();
#endif
    // Construction validates nonzero limits before any guest work starts.
    m_diagnosticBudget = std::make_unique<ArmageddonDiagnosticBudget>(
        options.checkpointLimit, options.chargeLimit);
    m_diagnosticOptions = options;
    m_diagnosticOutcome = DiagnosticOutcome{};
    m_diagnosticOutcome.stopReason = "running";
    m_diagnosticJournal = std::move(journal);
    m_diagnosticActive = true;
    setMissingFunctionPolicy(MissingFunctionPolicy::Stop);
    resetMissingFunctionReportOnce();
    try
    {
        // Same non-UI boot setup as pinned PS2Runtime::run(). No invented GP.
        m_stopRequested.store(false, std::memory_order_relaxed);
        ps2_stubs::resetSifState();
        resetIop();

        bool startupReady = true;
        const std::string elfName = getIoPaths().elfPath.filename().string();
        const char *rom0Path = std::getenv("PS2X_ARMAGEDDON_ROM0");
        const bool armageddonEntry = elfName == "SLUS_214.10" &&
                                     m_cpuContext.pc == 0x00100008u;
        if (armageddonEntry && rom0Path && rom0Path[0] != '\0')
        {
            std::cout << "[Armageddon bounded IOP bootstrap] entry_state_seeded=false"
                      << " initial_MSFLAG=0x0 initial_SMFLAG=0x0" << std::endl;
            if (!bootstrapArmageddonIopFromRom0(std::filesystem::path(rom0Path)))
            {
                m_diagnosticOutcome.stopReason = "iop_bootstrap_failed";
                m_diagnosticOutcome.error = "Original ROM0 SIFMAN/SIFCMD startup did not complete";
                startupReady = false;
                requestStop();
            }
            else
            {
                uint32_t msflag = 0u;
                uint32_t smflag = 0u;
                if (readIopSifRegister(3u, msflag) && readIopSifRegister(4u, smflag))
                {
                    std::cout << "[Armageddon bounded IOP bootstrap] startup_prepared=true"
                              << " complete=false"
                              << " MSFLAG=0x" << std::hex << msflag
                              << " SMFLAG=0x" << smflag << std::dec
                              << " source=ROM0_SIFMAN+SIFCMD"
                              << " ee_sifinit_assumption=true"
                              << " SIFCMD_init_may_be_waiting_for_EE_INIT_CMD=true"
                              << std::endl;
                }
            }
        }
        else if (armageddonEntry)
        {
            std::cout << "[Armageddon bounded IOP bootstrap] disabled; strict unseeded baseline"
                      << " MSFLAG=0x0 SMFLAG=0x0" << std::endl;
        }

        if (startupReady)
        {
            ps2_stubs::resetAudioStubState();
            ps2_stubs::resetMpegStubState();
            initializeEeKernelState(m_memory.getRDRAM());
            m_cpuContext.r[4] = _mm_setzero_si128();
            m_cpuContext.r[5] = _mm_setzero_si128();
            m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            diagnosticCapture(&m_cpuContext);
            if (m_diagnosticJournal)
                m_diagnosticJournal(m_diagnosticOutcome);
            m_eeScheduler->run();
            // Preserve the active invocation at failure/limit, not the possibly
            // different main-thread mirror copied by scheduler shutdown.
            if (m_diagnosticOutcome.stopReason == "running")
            {
                diagnosticCapture(m_eeScheduler->currentContext());
                m_diagnosticOutcome.stopReason = "runtime_stopped";
            }
        }
        else
        {
            diagnosticCapture(&m_cpuContext);
        }
    }
    catch (const std::exception &error)
    {
        diagnosticCapture(m_eeScheduler->currentContext());
        if (m_diagnosticOutcome.stopReason == "running")
            m_diagnosticOutcome.stopReason = "runtime_exception";
        m_diagnosticOutcome.error = error.what();
        requestStop();
    }
    catch (...)
    {
        diagnosticCapture(m_eeScheduler->currentContext());
        if (m_diagnosticOutcome.stopReason == "running")
            m_diagnosticOutcome.stopReason = "runtime_exception";
        m_diagnosticOutcome.error = "non-standard exception";
        requestStop();
    }
    if (m_diagnosticJournal)
    {
        if (!armageddonTryDiagnosticJournal(m_diagnosticJournal, m_diagnosticOutcome))
        {
            // Keep the captured registers/missing target available to the
            // separate native report even when the journal cannot be written.
            if (m_diagnosticOutcome.stopReason == "running" ||
                m_diagnosticOutcome.stopReason == "runtime_stopped")
                m_diagnosticOutcome.stopReason = "journal_failed";
        }
    }
    m_diagnosticActive = false;
    m_diagnosticJournal = {};
    return m_diagnosticOutcome;
}
