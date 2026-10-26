// Included inside PS2Runtime's public section, after R5900Context is defined.
// Kept as a separate auditable source used by the pinned-source patch.
    struct DiagnosticOptions
    {
        uint64_t checkpointLimit = 10000;
        uint64_t chargeLimit = 160000;
        uint64_t journalEvery = 1024;
    };

    struct DiagnosticOutcome
    {
        std::string stopReason = "not_started";
        std::string error;
        uint64_t checkpoints = 0;
        uint64_t checkpointCharges = 0;
        uint64_t guestDispatchesStarted = 0;
        bool contextValid = false;
        int threadId = 0;
        uint32_t missingSource = 0;
        uint32_t missingTarget = 0;
        bool missingTargetValid = false;
        uint32_t unsupportedSyscall = 0;
        bool unsupportedSyscallValid = false;
        uint32_t missingSyscallOverride = 0;
        bool missingSyscallOverrideValid = false;
        R5900Context context;
    };

    bool initializeHeadless();
    DiagnosticOutcome runBoundedDiagnostic(
        const DiagnosticOptions &options,
        std::function<void(const DiagnosticOutcome &)> journal = {});
    bool diagnosticCheckpoint(R5900Context *context, uint32_t estimatedCycles) noexcept;
    bool diagnosticUnsupportedSyscall(R5900Context *context, uint32_t syscall);
    bool diagnosticUnsupportedStub(R5900Context *context, const char *name);
    bool diagnosticMissingSyscallOverride(R5900Context *context, uint32_t syscall, uint32_t handler);
    void diagnosticGuestDispatch(R5900Context *context);

private:
    bool m_diagnosticActive = false;
    DiagnosticOptions m_diagnosticOptions;
    DiagnosticOutcome m_diagnosticOutcome;
    std::unique_ptr<ArmageddonDiagnosticBudget> m_diagnosticBudget;
    std::function<void(const DiagnosticOutcome &)> m_diagnosticJournal;
    void diagnosticCapture(R5900Context *context);
public:
