#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

// These are scheduler checkpoint calls and estimated scheduler charges. They are
// deliberately not named instructions or hardware cycles. This header has no
// dependence on game data, the runtime, graphics or platform libraries.
struct ArmageddonDiagnosticBudget
{
    uint64_t checkpointLimit;
    uint64_t chargeLimit;
    uint64_t checkpoints = 0;
    uint64_t charges = 0;
    const char *stopReason = nullptr;

    ArmageddonDiagnosticBudget(uint64_t checkpointMaximum, uint64_t chargeMaximum)
        : checkpointLimit(checkpointMaximum), chargeLimit(chargeMaximum)
    {
        if (checkpointMaximum == 0 || chargeMaximum == 0)
            throw std::invalid_argument("Diagnostic limits must be positive");
    }

    bool charge(uint32_t estimatedCycles) noexcept
    {
        if (stopReason)
            return true;
        ++checkpoints;
        const uint64_t amount = estimatedCycles == 0 ? 1 : estimatedCycles;
        charges = amount > std::numeric_limits<uint64_t>::max() - charges
                    ? std::numeric_limits<uint64_t>::max() : charges + amount;
        if (checkpoints >= checkpointLimit)
            stopReason = "checkpoint_limit";
        else if (charges >= chargeLimit)
            stopReason = "checkpoint_charge_limit";
        return stopReason != nullptr;
    }
};

// Scheduler checkpoints are noexcept. A journal callback may throw (disk full,
// broken output stream); keep that failure inside the diagnostic boundary.
template <typename Callback, typename Value>
bool armageddonTryDiagnosticJournal(Callback &callback, const Value &value) noexcept
{
    try
    {
        callback(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
