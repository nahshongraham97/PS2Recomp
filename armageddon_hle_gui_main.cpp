#include "ps2_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include "ps2_runtime_macros.h"
#include <string>
#include <unordered_map>

#include "../../mapfix-entry-2e1ea0-generated/entry_002e1ea0_0x2e1ea0.cpp"
#include "../../mapfix-owner-2e0370/generated/FUN_002e0370_hle_full_0x2e0370.cpp"
#include "../../mapfix-owner-2193e0/generated/FUN_002193e0_hle_full_0x2193e0.cpp"
#include "../../mapfix-owner-1df1c0/generated/FUN_001df1c0_hle_full_0x1df1c0.cpp"
#include "../../mapfix-owner-118cb0/generated/FUN_00118cb0_hle_full_0x118cb0.cpp"
#include "../../mapfix-owner-110fa0-full/generated/FUN_00110fa0_hle_full_0x110fa0.cpp"
#include "../../mapfix-entry-327b40-hle/generated/FUN_00327b40_hle_0x327b40.cpp"
#include "../../mapfix-owner-3ddcd0-full-verified/generated/FUN_003ddcd0_hle_full_0x3ddcd0.cpp"
#include "../../mapfix-entry-2209b0-hle/generated/entry_002209b0_hle_0x2209b0.cpp"
#include "../map-19c770-hinted-hle/generated/FUN_0019c770_hle_full_0x19c770.cpp"
#include "../map-1da240-full-hle/generated/FUN_001da240_hle_full_0x1da240.cpp"
#include "../map-1de6a8-entry-hle/generated/FUN_001de6a8_hle_0x1de6a8.cpp"
#include "../map-524350-full-hle/generated/FUN_00524350_hle_full_0x524350.cpp"
#include "../map-523708-full-hle/generated/FUN_00523708_hle_full_0x523708.cpp"
#include "../map-523600-full-hle/generated/FUN_00523600_hle_full_0x523600.cpp"
#include "../map-524af0-full-hle/generated/FUN_00524af0_hle_full_0x524af0.cpp"
#include "../map-1a1180-full-hle/generated/FUN_001a1180_hle_full_0x1a1180.cpp"
#include "../map-1ad1b0-tail-hle/generated/FUN_001ad1b0_tail_0x1ad1b0.cpp"
#include "../map-220950-wrapper-group-hle/generated/gap_wrapper_00220950_0x220950.cpp"
#include "../map-220950-wrapper-group-hle/generated/gap_wrapper_00220970_0x220970.cpp"
#include "../map-220950-wrapper-group-hle/generated/gap_wrapper_002209A0_0x2209a0.cpp"
#include "../map-220950-wrapper-group-hle/generated/gap_wrapper_002209B0_0x2209b0.cpp"
#include "../map-429a90-full-owner-hle/generated/FUN_00429a90_hle_full_0x429a90.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f060_hle_0x33f060.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f070_hle_0x33f070.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f090_hle_0x33f090.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f0b0_hle_0x33f0b0.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f0c0_hle_0x33f0c0.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f0e0_hle_0x33f0e0.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f100_hle_0x33f100.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f110_hle_0x33f110.cpp"
#include "../map-33f060-wrappers-hle/generated/entry_0033f130_hle_0x33f130.cpp"
#include "../map-1b9cf0-full-owner/generated/FUN_001b9cf0_hle_full_0x1b9cf0.cpp"
#include "../map-1b7370-full-owner/generated/FUN_001b7370_hle_full_0x1b7370.cpp"
#include "../map-11ea20-full-owner/generated/FUN_0011ea20_hle_full_0x11ea20.cpp"
#include "../map-1b7060-direct-jal/generated/FUN_001b7060_hle_0x1b7060.cpp"

void FUN_0032d040_hle_full_0x32d040(uint8_t *, R5900Context *, PS2Runtime *);
void entry_003293c0_hle_0x3293c0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_00328750_hle_full_0x328750(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_00328850_hle_full_0x328850(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_00327c60_hle_full_0x327c60(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_0042b8b0_hle_full_0x42b8b0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_00428e00_hle_full_0x428e00(uint8_t *, R5900Context *, PS2Runtime *);
void entry_0032d8d0_hle_0x32d8d0(uint8_t *, R5900Context *, PS2Runtime *);
void entry_0022031c_0x22031c(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_0012ed10_hle_full_0x12ed10(uint8_t *, R5900Context *, PS2Runtime *);
bool registerArmageddonBulkOwnerGaps(PS2Runtime &);
bool registerArmageddonObservedPointerTargets(PS2Runtime &);
void FUN_002404a0_hle_full_0x2404a0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_002811f0_hle_full_0x2811f0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_0019c550_hle_full_0x19c550(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_002e0370_hle_full_0x2e0370(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_002193e0_hle_full_0x2193e0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_001df1c0_hle_full_0x1df1c0(uint8_t *, R5900Context *, PS2Runtime *);
void FUN_00118cb0_hle_full_0x118cb0(uint8_t *, R5900Context *, PS2Runtime *);

#if defined(PS2X_ARMAGEDDON_SUPPLEMENTAL_REGISTRATION)
bool registerArmageddonSupplementalFunctions();
bool registerArmageddonOwner507f60();
bool registerArmageddonOwner12a4f0();
bool registerArmageddonOwner12a170();
bool registerArmageddonOwner3160d0();
bool registerArmageddonEntry12b380();
#endif

namespace
{
    PS2Runtime::RecompiledFunction gOriginalGlobalConstructors = nullptr;
    PS2Runtime::RecompiledFunction gOriginalHeapInitialization = nullptr;
    PS2Runtime::RecompiledFunction gOriginalHeapCreate = nullptr;
    PS2Runtime::RecompiledFunction gOriginalHeapRegistry = nullptr;
    PS2Runtime::RecompiledFunction gOriginalGameManagerInit = nullptr;
    PS2Runtime::RecompiledFunction gOriginalFirstManagerAllocation = nullptr;
    PS2Runtime::RecompiledFunction gOriginalGuestHeapAllocation = nullptr;
    PS2Runtime::RecompiledFunction gOriginalGuestFileRequest = nullptr;
    PS2Runtime::RecompiledFunction gOriginalResourceManagerStart = nullptr;
    PS2Runtime::RecompiledFunction gOriginalSifRequestStateCheck = nullptr;
    PS2Runtime::RecompiledFunction gOriginalAllocatorPoll = nullptr;
    PS2Runtime::RecompiledFunction gOriginalMainSubsystemGate = nullptr;
    PS2Runtime::RecompiledFunction gOriginalMainServiceGate = nullptr;
    PS2Runtime::RecompiledFunction gOriginalMainServiceInit = nullptr;
    PS2Runtime::RecompiledFunction gOriginalSifDmaInitializer = nullptr;
    PS2Runtime::RecompiledFunction gOriginalStartupCoordinator = nullptr;
    PS2Runtime::RecompiledFunction gOriginalStartupSubsystem = nullptr;
    struct NestedFunctionProbe
    {
        const char *name;
        PS2Runtime::RecompiledFunction original;
    };
    std::unordered_map<uint32_t, NestedFunctionProbe> gNestedFunctionProbes;
    uint64_t gStartupCoordinatorTraceCount = 0u;
    uint64_t gStartupSubsystemTraceCount = 0u;
    uint64_t gManagerCallbackTraceCount = 0u;
    std::mutex gHleAllocatorMutex;
    std::mutex gHleModuleMutex;
    std::unordered_map<std::string, uint32_t> gHleModuleIds;
    uint32_t gNextHleModuleId = 0x12u;
    uint32_t gHleAllocatorNext = 0x00A00000u;
    uint64_t gHleAllocatorCalls = 0u;
    uint64_t gHlePowCalls = 0u;
    uint64_t gAllocatorPollCalls = 0u;
    bool gHleRenderQueueDrainApplied = false;
    std::array<uint32_t, 6> gLastAllocatorPollState{};
    bool gHaveLastAllocatorPollState = false;
    PS2Runtime::RecompiledFunction gOriginalQueueService = nullptr;
    uint64_t gQueueServiceCalls = 0u;
    std::array<uint32_t, 4> gLastQueueServiceState{};
    bool gHaveLastQueueServiceState = false;
    uint64_t gAd520ProbeCalls = 0u;
    std::array<uint32_t, 7> gLastAd520State{};
    bool gHaveLastAd520State = false;

    uint32_t readGuestWord(uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0xffffffffu;
        if (rdram && address <= (32u * 1024u * 1024u) - sizeof(value))
            std::memcpy(&value, rdram + address, sizeof(value));
        return value;
    }

    std::string readGuestCString(uint8_t *rdram, uint32_t address)
    {
        if (!rdram)
            return "<no-rdram>";
        const uint32_t physical = address & 0x1fffffffu;
        if (physical >= 32u * 1024u * 1024u)
            return "<out-of-range>";
        std::string value;
        value.reserve(128u);
        for (uint32_t i = 0u; i < 256u && physical + i < 32u * 1024u * 1024u; ++i)
        {
            const char ch = static_cast<char>(rdram[physical + i]);
            if (ch == '\0')
                return value;
            value.push_back(ch);
        }
        return value + "<unterminated>";
    }

    void dumpStartupHeapState(const char *stage, uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t gp = ctx ? getRegU32(ctx, 28) : 0u;
        const uint32_t gpHeapBase = gp - 0x54b0u;
        std::cout << "[Armageddon HLE heap probe] stage=" << stage
                  << " pc=0x" << std::hex << (ctx ? ctx->pc : 0u)
                  << " gp=0x" << gp
                  << " permanent=0x" << readGuestWord(rdram, gpHeapBase)
                  << " mwfile=0x" << readGuestWord(rdram, gpHeapBase + 4u)
                  << " section=0x" << readGuestWord(rdram, gpHeapBase + 8u)
                  << " wave=0x" << readGuestWord(rdram, gpHeapBase + 12u)
                  << " heap_counter=0x" << readGuestWord(rdram, gp - 0x4ed0u)
                  << " system_arena=0x" << readGuestWord(rdram, gp - 0x4ed4u)
                  << " system_heap=0x" << readGuestWord(rdram, gp - 0x4eccu)
                  << "\n" << std::dec;
    }

    void probeGlobalConstructors(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        dumpStartupHeapState("global-ctors-before", rdram, ctx);
        if (gOriginalGlobalConstructors)
            gOriginalGlobalConstructors(rdram, ctx, runtime);
        dumpStartupHeapState("global-ctors-after", rdram, ctx);
    }

    void probeHeapInitialization(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        dumpStartupHeapState("heap-init-before", rdram, ctx);
        if (gOriginalHeapInitialization)
            gOriginalHeapInitialization(rdram, ctx, runtime);
        dumpStartupHeapState("heap-init-after", rdram, ctx);
    }

    void probeHeapCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t callerRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t arg0 = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t arg1 = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t arg2 = ctx ? getRegU32(ctx, 6) : 0u;
        const uint32_t gp = ctx ? getRegU32(ctx, 28) : 0u;
        const uint32_t heapCounterBefore = readGuestWord(rdram, gp - 0x4ed0u);
        const uint32_t descriptor[8] = {
            readGuestWord(rdram, arg0 + 0x00u), readGuestWord(rdram, arg0 + 0x04u),
            readGuestWord(rdram, arg0 + 0x08u), readGuestWord(rdram, arg0 + 0x0cu),
            readGuestWord(rdram, arg0 + 0x10u), readGuestWord(rdram, arg0 + 0x14u),
            readGuestWord(rdram, arg0 + 0x18u), readGuestWord(rdram, arg0 + 0x1cu)};
        const uint32_t backingHeapType = readGuestWord(rdram, descriptor[0] + 0x14u);
        if (gOriginalHeapCreate)
            gOriginalHeapCreate(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE heap create] entry_pc=0x" << std::hex << entryPc
                  << " caller_ra=0x" << callerRa
                  << " args=0x" << arg0
                  << ",0x" << arg1
                  << ",0x" << arg2
                  << " descriptor=[0x" << descriptor[0] << ",0x" << descriptor[1]
                  << ",0x" << descriptor[2] << ",0x" << descriptor[3]
                  << ",0x" << descriptor[4] << ",0x" << descriptor[5]
                  << ",0x" << descriptor[6] << ",0x" << descriptor[7] << "]"
                  << " backing_type=0x" << backingHeapType
                  << " manager_count=0x" << readGuestWord(rdram, gp - 0x4ed0u)
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " heap_counter_before=0x" << heapCounterBefore
                  << " heap_counter_after=0x" << readGuestWord(rdram, gp - 0x4ed0u)
                  << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                  << "\n" << std::dec;
    }

    void probeHeapRegistry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t callerRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t args[4] = {
            ctx ? getRegU32(ctx, 4) : 0u,
            ctx ? getRegU32(ctx, 5) : 0u,
            ctx ? getRegU32(ctx, 6) : 0u,
            ctx ? getRegU32(ctx, 7) : 0u};
        const uint32_t gp = ctx ? getRegU32(ctx, 28) : 0u;
        const uint32_t countBefore = readGuestWord(rdram, gp - 0x4ed0u);
        if (gOriginalHeapRegistry)
            gOriginalHeapRegistry(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE heap registry] entry_pc=0x" << std::hex << entryPc
                  << " caller_ra=0x" << callerRa
                  << " args=0x" << args[0] << ",0x" << args[1] << ",0x" << args[2] << ",0x" << args[3]
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " count_before=0x" << countBefore
                  << " count_after=0x" << readGuestWord(rdram, gp - 0x4ed0u)
                  << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                  << "\n" << std::dec;
    }

    void probeGameManagerInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        dumpStartupHeapState("manager-init-before", rdram, ctx);
        if (gOriginalGameManagerInit)
            gOriginalGameManagerInit(rdram, ctx, runtime);
        dumpStartupHeapState("manager-init-after", rdram, ctx);
        std::cout << "[Armageddon HLE manager init] result=0x" << std::hex
                  << (ctx ? getRegU32(ctx, 2) : 0u) << "\n" << std::dec;
    }

    void probeFirstManagerAllocation(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool isTargetAllocation = ctx && getRegU32(ctx, 5) == 0x1cu && getRegU32(ctx, 6) == 3u;
        const uint32_t entryRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t a1 = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t a2 = ctx ? getRegU32(ctx, 6) : 0u;
        const uint32_t a3 = ctx ? getRegU32(ctx, 7) : 0u;
        const uint32_t t0 = ctx ? getRegU32(ctx, 8) : 0u;
        const uint32_t t1 = ctx ? getRegU32(ctx, 9) : 0u;
        const bool isStartupCallback = entryRa >= 0x001A0E00u && entryRa < 0x001A1100u;
        if (isStartupCallback && gManagerCallbackTraceCount < 128u)
        {
            ++gManagerCallbackTraceCount;
            std::cout << "[Armageddon HLE indirect callback enter] call=" << gManagerCallbackTraceCount
                      << " entry_pc=0x" << std::hex << entryPc << " ra=0x" << entryRa
                      << " args=0x" << a0 << ",0x" << a1 << ",0x" << a2 << ",0x" << a3
                      << " t0=0x" << t0 << " t1=0x" << t1 << std::dec << std::endl;
        }
        if (isTargetAllocation)
        {
            dumpStartupHeapState("manager-allocation-before", rdram, ctx);
            std::cout << "[Armageddon HLE allocation request] pool=0x" << std::hex
                      << getRegU32(ctx, 4) << " size=0x" << getRegU32(ctx, 5)
                      << " alignment=0x" << getRegU32(ctx, 6) << "\n" << std::dec;
        }
        if (gOriginalFirstManagerAllocation)
            gOriginalFirstManagerAllocation(rdram, ctx, runtime);
        if (isStartupCallback && gManagerCallbackTraceCount <= 128u)
        {
            std::cout << "[Armageddon HLE indirect callback return] call=" << gManagerCallbackTraceCount
                      << " result_v0=0x" << std::hex << (ctx ? getRegU32(ctx, 2) : 0u)
                      << " result_v1=0x" << (ctx ? getRegU32(ctx, 3) : 0u)
                      << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                      << " after_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                      << " completed=" << (ctx && ctx->pc == entryRa ? "true" : "false")
                      << std::dec << std::endl;
        }
        if (isTargetAllocation)
        {
            std::cout << "[Armageddon HLE allocation result] value=0x" << std::hex
                      << getRegU32(ctx, 2) << "\n" << std::dec;
        }
    }

    void hleGuestHeapAllocation(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t requestedSize = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t alignmentExponent = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t tagAddress = ctx ? getRegU32(ctx, 6) : 0u;
        const uint32_t returnPc = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t allocationAlignment =
            alignmentExponent < 21u ? std::max(16u, 1u << alignmentExponent) : 16u;
        uint32_t resultAddress = 0u;
        uint32_t nextAddress = 0u;
        uint64_t callCount = 0u;

        if (rdram && ctx && requestedSize != 0u)
        {
            std::lock_guard<std::mutex> lock(gHleAllocatorMutex);
            const uint64_t aligned = (static_cast<uint64_t>(gHleAllocatorNext) + allocationAlignment - 1u) &
                                     ~static_cast<uint64_t>(allocationAlignment - 1u);
            const uint64_t end = aligned + requestedSize;
            constexpr uint32_t kHleArenaEnd = 0x01D00000u;
            if (aligned >= 0x00A00000u && end <= kHleArenaEnd && end <= 32u * 1024u * 1024u)
            {
                resultAddress = static_cast<uint32_t>(aligned);
                std::memset(rdram + resultAddress, 0, requestedSize);
                gHleAllocatorNext = static_cast<uint32_t>(end);
                ++gHleAllocatorCalls;
            }
            nextAddress = gHleAllocatorNext;
            callCount = gHleAllocatorCalls;
        }

        if (ctx)
        {
            SET_GPR_U32(ctx, 2, resultAddress);
            ctx->pc = returnPc;
        }
        if (callCount <= 128u || resultAddress == 0u)
        {
            std::cout << "[Armageddon HLE guest allocator] request=0x" << std::hex << requestedSize
                      << " align_exp=0x" << alignmentExponent
                      << " alignment=0x" << allocationAlignment
                      << " tag=0x" << tagAddress
                      << " result=0x" << resultAddress
                      << " next=0x" << nextAddress
                      << " return=0x" << returnPc
                      << " mode=zeroed-bump-arena-assumption" << std::dec << std::endl;
        }
    }

    void probeGuestFileRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t path = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t arg1 = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t arg2 = ctx ? getRegU32(ctx, 6) : 0u;
        const std::string pathText = readGuestCString(rdram, path);

        // This retail helper is used here as the IOP module-load entry point.
        // HLE boot deliberately skips module execution, but only reports a
        // successful load when the requested IRX exists on the mounted CD root.
        const ps2x::iop::ParsedPs2Path parsedPath = ps2x::iop::parsePs2Path(pathText);
        const bool isCdromModule = parsedPath.device == ps2x::iop::Ps2PathDevice::Cdrom &&
                                   parsedPath.path.size() >= 4u &&
                                   parsedPath.path.substr(parsedPath.path.size() - 4u) == ".IRX";
        if (isCdromModule && runtime && ctx)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
            std::filesystem::path hostPath;
            std::error_code fileError;
            const bool resolved = runtime->vfs().resolveHostPath(pathText, mounts, hostPath);
            const bool exists = resolved && std::filesystem::is_regular_file(hostPath, fileError) && !fileError;
            if (exists)
            {
                uint32_t moduleId = 0u;
                {
                    std::lock_guard<std::mutex> lock(gHleModuleMutex);
                    const std::string key = hostPath.lexically_normal().generic_string();
                    const auto found = gHleModuleIds.find(key);
                    if (found != gHleModuleIds.end())
                    {
                        moduleId = found->second;
                    }
                    else
                    {
                        moduleId = gNextHleModuleId++;
                        gHleModuleIds.emplace(key, moduleId);
                    }
                }
                const uint32_t returnPc = getRegU32(ctx, 31);
                SET_GPR_U32(ctx, 2, moduleId);
                ctx->pc = returnPc;
                const uintmax_t imageSize = std::filesystem::file_size(hostPath, fileError);
                std::cout << "[Armageddon HLE module load] path=\"" << pathText
                          << "\" host_file_exists=true host_file_bytes=0x" << std::hex
                          << (fileError ? 0u : imageSize) << " module_id=0x" << moduleId
                          << " mode=module-execution-skipped profile=HLE-boot\n" << std::dec;
                return;
            }

            std::cerr << "[Armageddon HLE module load rejected] path=\"" << pathText
                      << "\" resolved=" << (resolved ? "true" : "false")
                      << " file_error=" << fileError.message()
                      << " reason=missing-host-image; falling-back-to-guest-request\n";
        }

        if (gOriginalGuestFileRequest)
            gOriginalGuestFileRequest(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE file request] path_ptr=0x" << std::hex << path
                  << " path=\"" << pathText << "\" arg1=0x" << arg1 << " arg2=0x" << arg2
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u) << std::dec << std::endl;
    }

    void probeSifRequestStateCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pointerValue = readGuestWord(rdram, 0x005981e4u);
        const uint32_t left = readGuestWord(rdram, 0x00653aa8u);
        const uint32_t right = readGuestWord(rdram, 0x0059810cu);
        const uint32_t pointedValue = readGuestWord(rdram, pointerValue & 0x1fffffffu);
        if (gOriginalSifRequestStateCheck)
            gOriginalSifRequestStateCheck(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE SIF request-state check]"
                  << " pc=0x" << std::hex << (ctx ? ctx->pc : 0u)
                  << " left=0x" << left
                  << " right=0x" << right
                  << " pointer_slot=0x5981e4 pointer=0x" << pointerValue
                  << " pointed_value=0x" << pointedValue
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " left_after=0x" << readGuestWord(rdram, 0x00653aa8u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u)
                  << std::dec << std::endl;
    }

    void probeAllocatorPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ctx || !gOriginalAllocatorPoll)
            return;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t caller = getRegU32(ctx, 31);
        // sub_001AD520 saves its caller's return address at entry-SP+0x20;
        // the probe runs before the callback prologue, so the saved RA is SP+0x20.
        const uint32_t outerCaller = readGuestWord(rdram, getRegU32(ctx, 29) + 0x20u);
        const uint32_t args[4] = {getRegU32(ctx, 4), getRegU32(ctx, 5),
                                  getRegU32(ctx, 6), getRegU32(ctx, 7)};
        const std::array<uint32_t, 6> before = {
            readGuestWord(rdram, gp - 0x5bf0u), readGuestWord(rdram, gp - 0x5bf4u),
            readGuestWord(rdram, gp - 0x5bacu), readGuestWord(rdram, gp - 0x5bb0u),
            readGuestWord(rdram, gp - 0x5b9cu), readGuestWord(rdram, gp - 0x5b7cu)};
        gOriginalAllocatorPoll(rdram, ctx, runtime);
        // Explicit discovery-only host GPU completion shortcut. The retail
        // callback remains executed; this advances the queue consumer only
        // after the guest has waited repeatedly without a producer completing
        // the outstanding descriptors. It must not be used as boot proof.
        if (!gHleRenderQueueDrainApplied && gAllocatorPollCalls >= 15u &&
            outerCaller == 0x126fa8u &&
            std::getenv("PS2X_ARMAGEDDON_HLE_DRAIN_RENDER_QUEUE") &&
            before[2] != before[3] && runtime)
        {
            runtime->Store32(rdram, ctx, gp - 0x5bacu, before[3]);
            gHleRenderQueueDrainApplied = true;
            std::cout << "[Armageddon HLE assumption] render-queue-consumer="
                      << std::hex << before[2] << "->" << before[3]
                      << " after_repeated_host_gpu_wait; unverified shortcut"
                      << std::dec << std::endl;
        }
        const std::array<uint32_t, 6> after = {
            readGuestWord(rdram, gp - 0x5bf0u), readGuestWord(rdram, gp - 0x5bf4u),
            readGuestWord(rdram, gp - 0x5bacu), readGuestWord(rdram, gp - 0x5bb0u),
            readGuestWord(rdram, gp - 0x5b9cu), readGuestWord(rdram, gp - 0x5b7cu)};
        ++gAllocatorPollCalls;
        const bool changed = !gHaveLastAllocatorPollState || before != gLastAllocatorPollState || after != before;
        if (gAllocatorPollCalls == 0u || changed)
        {
            if (runtime)
            {
                std::cout << "[Armageddon HLE IRQ census at allocator-poll]"
                          << " dmac1_handlers=" << runtime->eeScheduler().registeredHandlerCount(true, 1u)
                          << " dmac2_handlers=" << runtime->eeScheduler().registeredHandlerCount(true, 2u)
                          << " intc2_handlers=" << runtime->eeScheduler().registeredHandlerCount(false, 2u)
                          << std::endl;
            }
        }
        if (gAllocatorPollCalls <= 16u || changed)
        {
            std::cout << "[Armageddon HLE allocator callback] call=" << gAllocatorPollCalls
                      << " entry=0x225d50 caller=0x" << std::hex << caller
                      << " outer_caller=0x" << outerCaller
                      << " args=0x" << args[0] << ",0x" << args[1] << ",0x" << args[2] << ",0x" << args[3]
                      << " before=";
            for (const uint32_t value : before) std::cout << "0x" << value << ",";
            std::cout << " after=";
            for (const uint32_t value : after) std::cout << "0x" << value << ",";
            std::cout << " result=0x" << getRegU32(ctx, 2)
                      << " return_pc=0x" << ctx->pc << std::dec << std::endl;
        }
        gLastAllocatorPollState = after;
        gHaveLastAllocatorPollState = true;
    }

    void probeQueueService(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ctx || !gOriginalQueueService)
            return;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t entryRa = getRegU32(ctx, 31);
        const uint32_t args[4] = {getRegU32(ctx, 4), getRegU32(ctx, 5),
                                  getRegU32(ctx, 6), getRegU32(ctx, 7)};
        const std::array<uint32_t, 4> before = {
            readGuestWord(rdram, gp - 0x5bacu), readGuestWord(rdram, gp - 0x5bb0u),
            readGuestWord(rdram, gp - 0x5b9cu), readGuestWord(rdram, gp - 0x5bf0u)};
        gOriginalQueueService(rdram, ctx, runtime);
        const std::array<uint32_t, 4> after = {
            readGuestWord(rdram, gp - 0x5bacu), readGuestWord(rdram, gp - 0x5bb0u),
            readGuestWord(rdram, gp - 0x5b9cu), readGuestWord(rdram, gp - 0x5bf0u)};
        ++gQueueServiceCalls;
        const bool changed = !gHaveLastQueueServiceState || before != gLastQueueServiceState || after != before;
        if (gQueueServiceCalls <= 16u || changed)
        {
            std::cout << "[Armageddon HLE queue service] call=" << gQueueServiceCalls
                      << " entry=0x1aca60 ra=0x" << std::hex << entryRa
                      << " args=0x" << args[0] << ",0x" << args[1] << ",0x" << args[2] << ",0x" << args[3]
                      << " before=";
            for (const uint32_t value : before) std::cout << "0x" << value << ",";
            std::cout << " after=";
            for (const uint32_t value : after) std::cout << "0x" << value << ",";
            std::cout << " result=0x" << getRegU32(ctx, 2)
                      << " return_pc=0x" << ctx->pc << std::dec << std::endl;
        }
        gLastQueueServiceState = after;
        gHaveLastQueueServiceState = true;
    }

    void probeResourceManagerStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t manager = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t config = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t pathPointer = readGuestWord(rdram, config + 0x14u);
        if (gOriginalResourceManagerStart)
            gOriginalResourceManagerStart(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE manager-start result] manager=0x" << std::hex << manager
                  << " config=0x" << config
                  << " path_ptr=0x" << pathPointer
                  << " path=\"" << readGuestCString(rdram, pathPointer) << "\""
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u) << std::dec << std::endl;
    }

    void probeMainSubsystemGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arg0 = ctx ? getRegU32(ctx, 4) : 0u;
        if (gOriginalMainSubsystemGate)
            gOriginalMainSubsystemGate(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE main gate] function=0x221c90 arg0=0x" << std::hex << arg0
                  << " result=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u) << std::dec << std::endl;
    }

    void probeMainServiceGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gOriginalMainServiceGate)
            gOriginalMainServiceGate(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE main gate] function=0x171b10 result=0x"
                  << std::hex << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u) << std::dec << std::endl;
    }

    void probeMainServiceInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gOriginalMainServiceInit)
            gOriginalMainServiceInit(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE main gate] function=0x225000 result=0x"
                  << std::hex << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " return_pc=0x" << (ctx ? ctx->pc : 0u) << std::dec << std::endl;
    }

    void probeSifDmaInitializer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t entryRa = ctx ? getRegU32(ctx, 31) : 0u;
        if (gOriginalSifDmaInitializer)
            gOriginalSifDmaInitializer(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE SIF DMA initializer] entry_pc=0x" << std::hex << entryPc
                  << " entry_ra=0x" << entryRa
                  << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                  << " after_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                  << " v0=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " completed=" << (ctx && ctx->pc == getRegU32(ctx, 31) ? "true" : "false")
                  << std::dec << std::endl;
    }

    void probeStartupCoordinator(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t entryRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t entryV0 = ctx ? getRegU32(ctx, 2) : 0u;
        const uint32_t arg0 = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t arg1 = ctx ? getRegU32(ctx, 5) : 0u;
        if (gOriginalStartupCoordinator)
            gOriginalStartupCoordinator(rdram, ctx, runtime);
        if (entryPc == 0x12289cu || entryPc == 0x1228c4u ||
            entryPc == 0x1228dcu || entryPc == 0x122940u ||
            (ctx && ctx->pc == 0x122ab8u))
        {
            std::cout << "[Armageddon startup check] pc=0x" << std::hex << entryPc
                      << " returned_v0=0x" << entryV0
                      << " next_pc=0x" << (ctx ? ctx->pc : 0u)
                      << " next_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                      << std::dec << std::endl;
        }
        if (gStartupCoordinatorTraceCount < 256u)
        {
            ++gStartupCoordinatorTraceCount;
            std::cout << "[Armageddon HLE startup coordinator] call=" << gStartupCoordinatorTraceCount
                      << " entry_pc=0x" << std::hex << entryPc
                      << " entry_ra=0x" << entryRa
                      << " args=0x" << arg0 << ",0x" << arg1
                      << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                      << " after_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                      << " v0=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                      << " completed=" << (ctx && ctx->pc == getRegU32(ctx, 31) ? "true" : "false")
                      << std::dec << std::endl;
        }
    }

    void probeStartupSubsystem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t entryRa = ctx ? getRegU32(ctx, 31) : 0u;
        if (gOriginalStartupSubsystem)
            gOriginalStartupSubsystem(rdram, ctx, runtime);
        if (gStartupSubsystemTraceCount < 128u)
        {
            ++gStartupSubsystemTraceCount;
            std::cout << "[Armageddon HLE subsystem return] call=" << gStartupSubsystemTraceCount
                      << " entry_pc=0x" << std::hex << entryPc
                      << " entry_ra=0x" << entryRa
                      << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                      << " after_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                      << " v0=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                      << " completed=" << (ctx && ctx->pc == getRegU32(ctx, 31) ? "true" : "false")
                      << std::dec << std::endl;
        }
    }

    void probeNestedFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx ? ctx->pc : 0u;
        const uint32_t entryRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t a0 = ctx ? getRegU32(ctx, 4) : 0u;
        const uint32_t a1 = ctx ? getRegU32(ctx, 5) : 0u;
        const uint32_t a2 = ctx ? getRegU32(ctx, 6) : 0u;
        const uint32_t a3 = ctx ? getRegU32(ctx, 7) : 0u;
        const auto it = gNestedFunctionProbes.find(entryPc);
        if (it == gNestedFunctionProbes.end() || !it->second.original)
        {
            std::cerr << "[Armageddon HLE nested probe] no original for pc=0x" << std::hex << entryPc << std::dec << "\n";
            return;
        }
        if (entryPc == 0x0010CEE0u)
        {
            const uint32_t currentBreak = readGuestWord(rdram, 0x00598114u);
            std::cout << "[Armageddon HLE sbrk precondition] current=0x" << std::hex << currentBreak
                      << " increment=0x" << a0 << " requested_end=0x" << (currentBreak + a0)
                      << " runtime_limit=0x" << runtime->guestHeapLimit() << std::dec << std::endl;
        }
        const uint32_t gpAtEntry = ctx ? getRegU32(ctx, 28) : 0u;
        const bool isGateWriter = entryPc == 0x001B33C0u ||
                                  entryPc == 0x001B5D20u ||
                                  entryPc == 0x001CFE70u;
        const uint32_t gateValueBefore = isGateWriter
                                             ? readGuestWord(rdram, gpAtEntry - 0x5BF0u)
                                             : 0u;
        const std::array<uint32_t, 7> ad520Before = entryPc == 0x001AD520u
            ? std::array<uint32_t, 7>{
                  readGuestWord(rdram, gpAtEntry - 0x5B4Cu),
                  readGuestWord(rdram, gpAtEntry - 0x5B48u),
                  readGuestWord(rdram, gpAtEntry - 0x5B50u),
                  readGuestWord(rdram, gpAtEntry - 0x5B44u),
                  readGuestWord(rdram, gpAtEntry - 0x5B98u),
                  readGuestWord(rdram, gpAtEntry - 0x5B9Cu),
                  readGuestWord(rdram, gpAtEntry - 0x5BF0u)}
            : std::array<uint32_t, 7>{};
        if (entryPc == 0x004295B0u)
        {
            const uint32_t parent = readGuestWord(rdram, a0 + 0x14u);
            const uint32_t magic = readGuestWord(rdram, parent + 0x1cu);
            std::cout << "[Armageddon HLE allocation precondition] descriptor=0x" << std::hex << a0
                      << " parent=0x" << parent << " parent_magic=0x" << magic
                      << " magic_matches=" << (magic == 0xbeabbeabu ? "true" : "false")
                      << " parent_kind=0x" << readGuestWord(rdram, parent + 0x14u)
                      << " parent_callback=0x" << readGuestWord(rdram, parent + 0x18u)
                      << " runtime_heap_base=0x" << runtime->guestHeapBase()
                      << " runtime_heap_end=0x" << runtime->guestHeapEnd()
                      << " runtime_heap_limit=0x" << runtime->guestHeapLimit()
                      << " hle_arena_next=0x" << gHleAllocatorNext << std::dec << std::endl;
        }
        it->second.original(rdram, ctx, runtime);
        std::cout << "[Armageddon HLE nested return] function=" << it->second.name
                  << " entry_pc=0x" << std::hex << entryPc
                  << " ra=0x" << entryRa
                  << " args=0x" << a0 << ",0x" << a1 << ",0x" << a2 << ",0x" << a3
                  << " result_v0=0x" << (ctx ? getRegU32(ctx, 2) : 0u)
                  << " result_v1=0x" << (ctx ? getRegU32(ctx, 3) : 0u)
                  << " after_pc=0x" << (ctx ? ctx->pc : 0u)
                  << " after_ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                  << " returned=" << (ctx && ctx->pc == entryRa ? "true" : "false");
        if (isGateWriter)
        {
            const uint32_t gateValueAfter = readGuestWord(rdram, gpAtEntry - 0x5BF0u);
            std::cout << " gate_global=gp-0x5bf0 before=0x" << gateValueBefore
                      << " after=0x" << gateValueAfter;
        }
        if (entryPc == 0x001AD520u)
        {
            const std::array<uint32_t, 7> after = {
                readGuestWord(rdram, gpAtEntry - 0x5B4Cu),
                readGuestWord(rdram, gpAtEntry - 0x5B48u),
                readGuestWord(rdram, gpAtEntry - 0x5B50u),
                readGuestWord(rdram, gpAtEntry - 0x5B44u),
                readGuestWord(rdram, gpAtEntry - 0x5B98u),
                readGuestWord(rdram, gpAtEntry - 0x5B9Cu),
                readGuestWord(rdram, gpAtEntry - 0x5BF0u)};
            ++gAd520ProbeCalls;
            const bool changed = !gHaveLastAd520State || ad520Before != gLastAd520State || after != ad520Before;
            if (gAd520ProbeCalls > 32u && !changed)
            {
                std::cout << std::dec << std::endl;
                gLastAd520State = after;
                gHaveLastAd520State = true;
                return;
            }
            std::cout << " state_before=";
            for (const uint32_t value : ad520Before) std::cout << "0x" << value << ",";
            std::cout << " state_after=";
            for (const uint32_t value : after) std::cout << "0x" << value << ",";
            gLastAd520State = after;
            gHaveLastAd520State = true;
        }
        std::cout << std::dec << std::endl;
    }

    bool installNestedFunctionProbe(PS2Runtime &runtime, uint32_t address, const char *name)
    {
        auto original = runtime.lookupFunction(address);
        if (!original || !runtime.replaceFunction(address, probeNestedFunction))
            return false;
        gNestedFunctionProbes[address] = NestedFunctionProbe{name, original};
        return true;
    }

    bool installProbeRange(PS2Runtime &runtime,
                           uint32_t start,
                           uint32_t endExclusive,
                           PS2Runtime::RecompiledFunction wrapper,
                           PS2Runtime::RecompiledFunction &original)
    {
        if (start >= endExclusive || ((endExclusive - start) & 3u) != 0u)
            return false;
        original = runtime.lookupFunction(start);
        if (!original)
            return false;
        for (uint32_t address = start; address < endExclusive; address += 4u)
        {
            if (!runtime.replaceFunction(address, wrapper))
                return false;
        }
        return true;
    }

    bool installProbe(PS2Runtime &runtime,
                      uint32_t address,
                      PS2Runtime::RecompiledFunction wrapper,
                      PS2Runtime::RecompiledFunction &original)
    {
        original = runtime.lookupFunction(address);
        return original != nullptr && runtime.replaceFunction(address, wrapper);
    }

    void hleStartupPow(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;

        // HLE-only acceleration for Armageddon's 128x128 alpha lookup table.
        // This preserves the R5900 FPU result register and guest return PC,
        // while approximating the original guest pow implementation with libm.
        const float base = ctx->f[12];
        const float exponent = ctx->f[13];
        ctx->f[0] = std::pow(base, exponent);
        const uint32_t returnPc = getRegU32(ctx, 31);
        ctx->pc = returnPc;

        ++gHlePowCalls;
        if (gHlePowCalls == 1u)
        {
            std::cout << "[Armageddon HLE math] helper=0x122580"
                      << " implementation=host-std-pow-float"
                      << " base=" << base << " exponent=" << exponent
                      << " result=" << ctx->f[0]
                      << " return_pc=0x" << std::hex << returnPc << std::dec
                      << " mode=approximation; original guest routine bypassed\n";
        }
    }
}

int main(int argc, char **argv)
{
#if defined(PS2X_ARMAGEDDON_SUPPLEMENTAL_REGISTRATION)
    if (!registerArmageddonSupplementalFunctions())
    {
        std::cerr << "Armageddon supplemental function registration failed\n";
        return 3;
    }
    if (!registerArmageddonOwner507f60())
    {
        std::cerr << "Armageddon 0x00507F60 whole-owner registration failed\n";
        return 4;
    }
    if (!registerArmageddonOwner12a4f0())
    {
        std::cerr << "Armageddon 0x0012A4F0 whole-owner registration failed\n";
        return 5;
    }
    if (!registerArmageddonOwner12a170())
    {
        std::cerr << "Armageddon 0x0012A170 whole-owner registration failed\n";
        return 6;
    }
    if (!registerArmageddonOwner3160d0())
    {
        std::cerr << "Armageddon 0x003160D0 whole-owner registration failed\n";
        return 7;
    }
    if (!registerArmageddonEntry12b380())
    {
        std::cerr << "Armageddon 0x0012B380 entry registration failed\n";
        return 8;
    }
#endif

    if (argc != 2)
    {
        std::cerr << "Usage: ps2EntryRunner <SLUS_214.10 ELF>\n";
        return 2;
    }

    PS2Runtime runtime;
    runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
    if (!runtime.initialize("Mortal Kombat: Armageddon - HLE boot"))
    {
        std::cerr << "Failed to initialize the visible PS2 runtime\n";
        return 1;
    }

    constexpr uint32_t continuationAddresses[] = {
        0x0032D040u,
        0x0032D05Cu,
        0x0032D064u,
        0x0032D06Cu,
        0x0032D088u,
        0x0032D0A8u,
        0x0032D100u,
        0x0032D108u,
        0x0032D110u,
    };
    for (const uint32_t address : continuationAddresses)
    {
        if (!runtime.replaceFunction(address, FUN_0032d040_hle_full_0x32d040))
        {
            std::cerr << "Failed to replace FUN_0032d040 translation at 0x"
                      << std::hex << address << '\n';
            return 4;
        }
        std::cout << "[Armageddon HLE map override] address=0x"
                  << std::hex << address << std::dec << " registered=true\n";
    }

    if (!runtime.loadELF(std::string(argv[1])))
    {
        std::cerr << "Failed to load Armageddon ELF: " << argv[1] << '\n';
        return 1;
    }

    if (!registerArmageddonBulkOwnerGaps(runtime))
    {
        std::cerr << "Armageddon bulk owner gap registration or audited-target check failed\n";
        return 22;
    }

    if (!registerArmageddonObservedPointerTargets(runtime))
    {
        std::cerr << "Armageddon observed pointer-target registration failed\n";
        return 23;
    }

    for (const uint32_t entry : {0x00110fa0u, 0x00110fb8u, 0x00110fc0u, 0x00110fc8u, 0x00110fd8u})
    {
        if (!runtime.replaceFunction(entry, FUN_00110fa0_hle_full_0x110fa0))
        {
            std::cerr << "Failed to register complete FUN_00110FA0 owner at 0x"
                      << std::hex << entry << '\n';
            return 21;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00110FA0"
              << " range=[0x110fa0,0x110fe0) resumable_entries=5\n";
    uint32_t owner327b40Entries = 0u;
    for (uint32_t entry = 0x00327B40u; entry < 0x00327C60u; entry += 4u)
    {
        if (!runtime.replaceFunction(entry, FUN_00327b40_hle_0x327b40))
        {
            std::cerr << "Failed to register FUN_00327B40 body at 0x" << std::hex << entry << '\n';
            return 22;
        }
        ++owner327b40Entries;
    }
    std::cout << "[Armageddon HLE map override] HLE entry=0x00327B40 range=[0x00327B40,0x00327C60)"
              << " registered_entries=" << std::dec << owner327b40Entries
              << " indirect_fallbacks=144 unresolved_indirect_site=0x00327C24\n";
    constexpr uint32_t owner3ddcd0Entries[] = {
        0x003ddcd0u, 0x003ddd28u, 0x003dddacu, 0x003dddc4u, 0x003ddde4u,
        0x003dde1cu, 0x003dde30u, 0x003dde84u, 0x003dde9cu, 0x003ddebcu,
    };
    for (const uint32_t entry : owner3ddcd0Entries)
    {
        if (!runtime.replaceFunction(entry, FUN_003ddcd0_hle_full_0x3ddcd0))
        {
            std::cerr << "Failed to register complete FUN_003DDCD0 owner at 0x"
                      << std::hex << entry << '\n';
            return 23;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_003DDCD0"
              << " range=[0x3ddcd0,0x3ddee4) resumable_entries=10"
              << " direct_targets=0x3ddec8,0x3ddecc\n";
    if (!runtime.replaceFunction(0x002209b0u, entry_002209b0_hle_0x2209b0))
    {
        std::cerr << "Failed to register retail code thunk at 0x002209B0\n";
        return 24;
    }
    std::cout << "[Armageddon HLE code-range registration] address=0x2209b0"
              << " range=[0x2209b0,0x2209cc) source=retail-ELF thunk=true registered=true\n";
    for (const uint32_t entry : {0x0012ed10u, 0x0012ed3cu, 0x0012ed5cu})
    {
        if (!runtime.replaceFunction(entry, FUN_0012ed10_hle_full_0x12ed10))
        {
            std::cerr << "Failed to register complete FUN_0012ED10 owner at 0x"
                      << std::hex << entry << '\n';
            return 14;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_0012ED10"
              << " range=[0x12ed10,0x12eda0) entries=0x12ed10,0x12ed3c,0x12ed5c\n";

    for (const uint32_t entry : {0x002404a0u, 0x002404b0u, 0x002404f8u, 0x0024051cu,
                                 0x00240590u, 0x00240598u, 0x00240604u,
                                 0x00240620u})
    {
        if (!runtime.replaceFunction(entry, FUN_002404a0_hle_full_0x2404a0))
        {
            std::cerr << "Failed to register full FUN_002404A0 owner at 0x"
                      << std::hex << entry << '\n';
            return 15;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_002404A0"
              << " range=[0x2404a0,0x240680) entry_and_resumable_entries=8\n";

    if (!runtime.replaceFunction(0x0022031cu, entry_0022031c_0x22031c))
    {
        std::cerr << "Failed to register owner-confirmed continuation 0x0022031C\n";
        return 13;
    }
    std::cout << "[Armageddon HLE owner continuation] owner=FUN_002202A0"
              << " entry=0x22031c bytes=16 source=retail-ELF-recompile registered=true\n";

    if (!runtime.replaceFunction(0x002e1ea0u, entry_002e1ea0_0x2e1ea0))
    {
        std::cerr << "Failed to register owner-confirmed continuation 0x002E1EA0\n";
        return 16;
    }
    std::cout << "[Armageddon HLE owner continuation] owner=FUN_002EBB20"
              << " entry=0x2e1ea0 bytes=16 source=retail-ELF-recompile registered=true\n";

    for (const uint32_t entry : {0x002811f0u, 0x0028126cu})
    {
        if (!runtime.replaceFunction(entry, FUN_002811f0_hle_full_0x2811f0))
        {
            std::cerr << "Failed to register complete FUN_002811F0 owner at 0x"
                      << std::hex << entry << '\n';
            return 17;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_002811F0"
              << " range=[0x2811f0,0x281310) resumable_entries=2\n";

    for (const uint32_t entry : {0x0019c550u, 0x0019c5acu, 0x0019c628u,
                                 0x0019c648u, 0x0019c6a0u, 0x0019c6c8u,
                                 0x0019c6dcu, 0x0019c704u, 0x0019c728u})
    {
        if (!runtime.replaceFunction(entry, FUN_0019c550_hle_full_0x19c550))
        {
            std::cerr << "Failed to register complete FUN_0019C550 owner at 0x"
                      << std::hex << entry << '\n';
            return 18;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_0019C550"
              << " range=[0x19c550,0x19c770) resumable_entries=9\n";

    for (const uint32_t entry : {0x002e0370u, 0x002e03ccu, 0x002e03ecu,
                                 0x002e041cu, 0x002e04a4u, 0x002e04b4u,
                                 0x002e04ccu})
    {
        if (!runtime.replaceFunction(entry, FUN_002e0370_hle_full_0x2e0370))
        {
            std::cerr << "Failed to register complete FUN_002E0370 owner at 0x"
                      << std::hex << entry << '\n';
            return 19;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_002E0370"
              << " range=[0x2e0370,0x2e05a0) resumable_entries=7\n";

    for (const uint32_t entry : {0x002193e0u, 0x00219404u, 0x00219418u,
                                 0x00219434u, 0x0021944cu, 0x00219474u,
                                 0x002194acu, 0x002194ccu, 0x0021952cu,
                                 0x0021954cu, 0x00219568u, 0x00219578u,
                                 0x00219588u, 0x002195bcu, 0x002195e8u})
    {
        if (!runtime.replaceFunction(entry, FUN_002193e0_hle_full_0x2193e0))
        {
            std::cerr << "Failed to register complete FUN_002193E0 owner at 0x"
                      << std::hex << entry << '\n';
            return 20;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_002193E0"
              << " range=[0x2193e0,0x21961c) resumable_entries=15\n";

    for (const uint32_t entry : {0x001df1c0u, 0x001df1c8u, 0x001df24cu,
                                 0x001df2e0u, 0x001df340u})
    {
        if (!runtime.replaceFunction(entry, FUN_001df1c0_hle_full_0x1df1c0))
        {
            std::cerr << "Failed to register complete FUN_001DF1C0 owner at 0x"
                      << std::hex << entry << '\n';
            return 21;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_001DF1C0"
              << " range=[0x1df1c0,0x1df378) resumable_entries=5\n";

    for (const uint32_t entry : {0x00118cb0u, 0x00118ce0u, 0x00118ce8u,
                                 0x00118d1cu, 0x00118d38u, 0x00118d40u,
                                 0x00118d70u, 0x00118d84u, 0x00118dacu,
                                 0x00118dc8u, 0x00118dd8u})
    {
        if (!runtime.replaceFunction(entry, FUN_00118cb0_hle_full_0x118cb0))
        {
            std::cerr << "Failed to register complete FUN_00118CB0 owner at 0x"
                      << std::hex << entry << '\n';
            return 22;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00118CB0"
              << " range=[0x118cb0,0x118dfc) resumable_entries=11\n";

    if (!runtime.replaceFunction(0x003293c0u, entry_003293c0_hle_0x3293c0))
    {
        std::cerr << "Failed to register verified secondary code range 0x003293C0\n";
        return 6;
    }
    std::cout << "[Armageddon HLE code-range registration] address=0x3293c0"
              << " source=retail-ELF bytes=[0x3293c0,0x3293f0) registered=true\n";

    for (const uint32_t entry : {0x003287a0u, 0x003287acu, 0x003287c0u, 0x003287e4u, 0x00328808u, 0x00328810u, 0x00328818u})
    {
        if (!runtime.replaceFunction(entry, FUN_00328750_hle_full_0x328750)) { std::cerr << "unregistered FUN_00328750 tail\n"; return 7; }
    }
    for (const uint32_t entry : {0x00328850u, 0x003288a0u, 0x003288acu, 0x003288c0u, 0x003288e4u, 0x00328908u, 0x00328910u, 0x00328918u})
    {
        if (!runtime.replaceFunction(entry, FUN_00328850_hle_full_0x328850)) { std::cerr << "unregistered FUN_00328850 body\n"; return 9; }
    }
    if (!runtime.replaceFunction(0x0032d8d0u, entry_0032d8d0_hle_0x32d8d0)) { std::cerr << "unregistered exact callback 0x32d8d0\n"; return 10; }
    std::cout << "[Armageddon HLE verified target] 0x32d8d0=retail jr ra + addiu v0,a0,0x3c\n";
    for (uint32_t entry = 0x00327c60u; entry < 0x00327db0u; entry += 4u)
    {
        if (!runtime.replaceFunction(entry, FUN_00327c60_hle_full_0x327c60))
        {
            std::cerr << "Failed to register contiguous original owner entry 0x" << std::hex << entry << "\n";
            return 11;
        }
    }
    std::cout << "[Armageddon HLE owner-map registration] owner=FUN_00327c60 entries=84 body=[0x327c60,0x327db0) reason=unresolved-indirect-entry-resumption\n";
    for (const uint32_t entry : {0x0042b8b0u, 0x0042b8c4u, 0x0042b8d4u, 0x0042b8f0u, 0x0042b900u, 0x0042b908u, 0x0042b938u, 0x0042b940u, 0x0042b944u, 0x0042b958u, 0x0042b960u, 0x0042b96cu})
    {
        if (!runtime.replaceFunction(entry, FUN_0042b8b0_hle_full_0x42b8b0)) { std::cerr << "Failed to register FUN_0042b8b0 body entry\n"; return 12; }
    }
    std::cout << "[Armageddon HLE owner-map registration] owner=FUN_0042b8b0 body=[0x42b8b0,0x42b980) entries=12\n";
    for (const uint32_t entry : {0x00428e00u, 0x00428e48u, 0x00428e50u,
                                 0x00428e68u, 0x00428e70u, 0x00428e80u,
                                 0x00428e88u, 0x00428eb8u, 0x00428eecu,
                                 0x00428ef8u})
    {
        if (!runtime.replaceFunction(entry, FUN_00428e00_hle_full_0x428e00))
        {
            std::cerr << "Failed to register complete FUN_00428E00 owner at 0x"
                      << std::hex << entry << '\n';
            return 25;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00428E00"
              << " range=[0x428e00,0x428f10) resumable_entries=10\n";
    for (const uint32_t entry : {0x0019c770u, 0x0019c7acu, 0x0019c7f0u,
                                 0x0019c804u, 0x0019c85cu, 0x0019c868u})
    {
        if (!runtime.replaceFunction(entry, FUN_0019c770_hle_full_0x19c770))
        {
            std::cerr << "Failed to register complete FUN_0019C770 owner at 0x"
                      << std::hex << entry << '\n';
            return 26;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_0019C770"
              << " range=[0x19c770,0x19c8b0) resumable_entries=6\n";
    for (uint32_t entry = 0x001da240u; entry < 0x001da608u; entry += 4u)
    {
        if (!runtime.replaceFunction(entry, FUN_001da240_hle_full_0x1da240))
        {
            std::cerr << "Failed to register complete FUN_001DA240 owner at 0x"
                      << std::hex << entry << '\n';
            return 28;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_001DA240"
              << " range=[0x1da240,0x1da608) resumable_entries=242"
              << " indirect_fallbacks=2\n";
    for (const uint32_t entry : {0x001de6a8u, 0x001de6e8u, 0x001de708u})
    {
        if (!runtime.replaceFunction(entry, FUN_001de6a8_hle_0x1de6a8))
        {
            std::cerr << "Failed to register original FUN_001DE6A8 at 0x"
                      << std::hex << entry << '\n';
            return 29;
        }
    }
    std::cout << "[Armageddon HLE ELF function registration]"
              << " range=[0x1de6a8,0x1de728) resumable_entries=3\n";
    for (const uint32_t entry : {0x00524350u, 0x005243a0u, 0x005243c4u,
                                 0x00524420u, 0x00524438u, 0x00524450u,
                                 0x00524470u, 0x00524478u, 0x00524488u,
                                 0x005244acu})
    {
        if (!runtime.replaceFunction(entry, FUN_00524350_hle_full_0x524350))
        {
            std::cerr << "Failed to register complete FUN_00524350 at 0x"
                      << std::hex << entry << '\n';
            return 30;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00524350"
              << " range=[0x524350,0x5244e8) resumable_entries=10\n";
    for (const uint32_t entry : {0x00523708u, 0x00523728u, 0x00523738u,
                                 0x00523740u, 0x00523750u, 0x00523758u,
                                 0x00523778u, 0x00523790u})
    {
        if (!runtime.replaceFunction(entry, FUN_00523708_hle_full_0x523708))
        {
            std::cerr << "Failed to register complete FUN_00523708 at 0x"
                      << std::hex << entry << '\n';
            return 31;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00523708"
              << " range=[0x523708,0x5237d0) resumable_entries=8\n";
    for (const uint32_t entry : {0x00523600u, 0x00523628u, 0x00523674u,
                                 0x00523690u, 0x005236b0u, 0x005236b8u,
                                 0x005236d0u, 0x005236dcu, 0x005236e8u})
    {
        if (!runtime.replaceFunction(entry, FUN_00523600_hle_full_0x523600))
        {
            std::cerr << "Failed to register complete FUN_00523600 at 0x"
                      << std::hex << entry << '\n';
            return 32;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00523600"
              << " range=[0x523600,0x523708) resumable_entries=9\n";
    for (const uint32_t entry : {0x00524af0u, 0x00524b18u})
    {
        if (!runtime.replaceFunction(entry, FUN_00524af0_hle_full_0x524af0))
        {
            std::cerr << "Failed to register complete FUN_00524AF0 at 0x"
                      << std::hex << entry << '\n';
            return 33;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00524AF0"
              << " range=[0x524af0,0x524b70) resumable_entries=2\n";
    for (uint32_t entry = 0x001a1180u; entry < 0x001a1340u; entry += 4u)
    {
        if (!runtime.replaceFunction(entry, FUN_001a1180_hle_full_0x1a1180))
        {
            std::cerr << "Failed to register complete FUN_001A1180 owner at 0x"
                      << std::hex << entry << '\n';
            return 34;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_001A1180"
              << " range=[0x1a1180,0x1a1340) resumable_entries=112"
              << " indirect_fallbacks=2\n";
    if (!runtime.replaceFunction(0x001ad1b0u, FUN_001ad1b0_tail_0x1ad1b0))
    {
        std::cerr << "Failed to register ELF continuation FUN_00225D40 at 0x001AD1B0\n";
        return 35;
    }
    std::cout << "[Armageddon HLE ELF continuation] owner=FUN_00225D40"
              << " target=0x1ad1b0 range=[0x1ad1b0,0x1ad1c0)\n";
    const struct { uint32_t address; PS2Runtime::RecompiledFunction function; } recoveredWrappers[] = {
        {0x00220950u, gap_wrapper_00220950_0x220950},
        {0x00220970u, gap_wrapper_00220970_0x220970},
        {0x002209a0u, gap_wrapper_002209A0_0x2209a0},
        {0x002209b0u, gap_wrapper_002209B0_0x2209b0},
    };
    for (const auto &entry : recoveredWrappers)
    {
        if (!runtime.replaceFunction(entry.address, entry.function))
        {
            std::cerr << "Failed to register ELF callback wrapper at 0x"
                      << std::hex << entry.address << '\n';
            return 36;
        }
    }
    std::cout << "[Armageddon HLE ELF callback wrappers]"
              << " table=0x220900 targets=4 source=verified-retail-ELF\n";
    for (const uint32_t entry : {0x00429a90u, 0x00429ad0u, 0x00429ae0u,
                                 0x00429afcu, 0x00429b20u, 0x00429b60u,
                                 0x00429b98u, 0x00429ba0u, 0x00429bb0u,
                                 0x00429bd8u, 0x00429bf0u, 0x00429c04u,
                                 0x00429c24u})
    {
        if (!runtime.replaceFunction(entry, FUN_00429a90_hle_full_0x429a90))
        {
            std::cerr << "Failed to register complete FUN_00429A90 owner at 0x"
                      << std::hex << entry << '\n';
            return 37;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=FUN_00429A90"
              << " range=[0x429a90,0x429c44) resumable_entries=13\n";
    constexpr uint32_t owner1b9cf0Entries[] = {
        0x001b9cf0u, 0x001b9d2cu, 0x001b9ebcu, 0x001b9ec4u,
        0x001b9ec8u, 0x001b9ef4u, 0x001b9efcu, 0x001b9f14u,
        0x001b9f24u, 0x001b9f48u, 0x001b9f64u, 0x001b9f6cu,
        0x001b9f80u, 0x001b9f94u, 0x001b9fa8u, 0x001b9fccu,
        0x001ba068u, 0x001ba070u, 0x001ba078u, 0x001ba0c0u,
        0x001ba0d8u, 0x001ba0f0u, 0x001ba0f4u,
    };
    for (const uint32_t entry : owner1b9cf0Entries)
    {
        if (!runtime.replaceFunction(entry, FUN_001b9cf0_hle_full_0x1b9cf0))
        {
            std::cerr << "Failed to register full ELF-backed owner 0x1b9cf0 entry at 0x"
                      << std::hex << entry << '\n';
            return 25;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=0x1b9cf0"
              << " range=[0x1b9cf0,0x1ba108) resumable_entries=23"
              << " evidence=Ghidra-body-chunks+observed-indirect-targets\n";
    constexpr uint32_t owner1b7370Entries[] = {
        0x001b7370u, 0x001b7450u, 0x001b74e8u, 0x001b7528u, 0x001b75d4u,
        0x001b7614u, 0x001b7670u, 0x001b7688u, 0x001b76a0u, 0x001b76b8u,
    };
    for (const uint32_t entry : owner1b7370Entries)
    {
        if (!runtime.replaceFunction(entry, FUN_001b7370_hle_full_0x1b7370))
        {
            std::cerr << "Failed to register full ELF-backed owner 0x1b7370 entry at 0x"
                      << std::hex << entry << '\n';
            return 42;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=0x1b7370"
              << " range=[0x1b7370,0x1b7710) resumable_entries=10"
              << " evidence=Ghidra-body-chunks+observed-direct-jump\n";
    constexpr uint32_t owner11ea20Entries[] = {
        0x0011ea20u, 0x0011eaa0u, 0x0011eaa4u, 0x0011eaccu,
        0x0011ead0u, 0x0011eb44u, 0x0011ebacu, 0x0011ebc8u,
        0x0011ec14u, 0x0011ecdcu, 0x0011ed88u, 0x0011ed8cu,
        0x0011ed98u, 0x0011ed9cu, 0x0011ef44u, 0x0011ef98u,
    };
    for (const uint32_t entry : owner11ea20Entries)
    {
        if (!runtime.replaceFunction(entry, FUN_0011ea20_hle_full_0x11ea20))
        {
            std::cerr << "Failed to register full ELF-backed owner 0x11ea20 entry at 0x"
                      << std::hex << entry << '\n';
            return 43;
        }
    }
    std::cout << "[Armageddon HLE full-owner registration] owner=0x11ea20"
              << " range=[0x11ea20,0x11f21c) resumable_entries=16"
              << " evidence=Ghidra-body-chunks+observed-direct-jump\n";
    constexpr uint32_t entry1b7060[] = {
        0x001b7060u, 0x001b70a8u, 0x001b70f4u, 0x001b711cu,
    };
    for (const uint32_t entry : entry1b7060)
    {
        if (!runtime.replaceFunction(entry, FUN_001b7060_hle_0x1b7060))
        {
            std::cerr << "Failed to register ELF-backed direct JAL target 0x1b7060 at 0x"
                      << std::hex << entry << '\n';
            return 44;
        }
    }
    std::cout << "[Armageddon HLE direct-JAL registration] entry=0x1b7060"
              << " provisional-range=[0x1b7060,0x1b7160) entries=4"
              << " evidence=retail-JAL-target; end=next-active-map-entry\n";
    const struct { uint32_t address; PS2Runtime::RecompiledFunction function; } wrapperEntries[] = {
        {0x0033f060u, entry_0033f060_hle_0x33f060},
        {0x0033f070u, entry_0033f070_hle_0x33f070},
        {0x0033f090u, entry_0033f090_hle_0x33f090},
        {0x0033f0b0u, entry_0033f0b0_hle_0x33f0b0},
        {0x0033f0c0u, entry_0033f0c0_hle_0x33f0c0},
        {0x0033f0e0u, entry_0033f0e0_hle_0x33f0e0},
        {0x0033f100u, entry_0033f100_hle_0x33f100},
        {0x0033f110u, entry_0033f110_hle_0x33f110},
        {0x0033f130u, entry_0033f130_hle_0x33f130},
    };
    for (const auto &entry : wrapperEntries)
    {
        if (!runtime.replaceFunction(entry.address, entry.function))
        {
            std::cerr << "Failed to register ELF jump wrapper at 0x"
                      << std::hex << entry.address << '\n';
            return 27;
        }
    }
    std::cout << "[Armageddon HLE ELF wrapper registration]"
              << " range=[0x33f060,0x33f150) entries=9\n";
    const bool heapProbeInstalled =
        installProbe(runtime, 0x003343E0u, probeGlobalConstructors, gOriginalGlobalConstructors) &&
        installProbe(runtime, 0x00310690u, probeHeapInitialization, gOriginalHeapInitialization) &&
        installProbe(runtime, 0x00429810u, probeHeapCreate, gOriginalHeapCreate) &&
        installProbe(runtime, 0x00428FD0u, probeHeapRegistry, gOriginalHeapRegistry) &&
        installProbe(runtime, 0x002DF470u, probeGameManagerInit, gOriginalGameManagerInit) &&
        installProbe(runtime, 0x00429D20u, probeFirstManagerAllocation, gOriginalFirstManagerAllocation) &&
        installProbe(runtime, 0x002DF5B0u, hleGuestHeapAllocation, gOriginalGuestHeapAllocation) &&
        installProbe(runtime, 0x00115B68u, probeGuestFileRequest, gOriginalGuestFileRequest) &&
        installProbe(runtime, 0x0032C970u, probeResourceManagerStart, gOriginalResourceManagerStart) &&
        installProbe(runtime, 0x00115268u, probeSifRequestStateCheck, gOriginalSifRequestStateCheck) &&
        installProbe(runtime, 0x00221C90u, probeMainSubsystemGate, gOriginalMainSubsystemGate) &&
        installProbe(runtime, 0x00171B10u, probeMainServiceGate, gOriginalMainServiceGate) &&
        installProbe(runtime, 0x00225000u, probeMainServiceInit, gOriginalMainServiceInit) &&
        installProbe(runtime, 0x00225DB0u, probeSifDmaInitializer, gOriginalSifDmaInitializer) &&
      installProbe(runtime, 0x00225D50u, probeAllocatorPoll, gOriginalAllocatorPoll) &&
        installProbe(runtime, 0x001ACA60u, probeQueueService, gOriginalQueueService) &&
        installProbeRange(runtime, 0x00122740u, 0x00122AC8u, probeStartupCoordinator, gOriginalStartupCoordinator) &&
        installProbeRange(runtime, 0x00127260u, 0x00127364u, probeStartupSubsystem, gOriginalStartupSubsystem);
    const bool initializerProbesInstalled =
        installNestedFunctionProbe(runtime, 0x001A6DD0u, "FUN_001A6DD0") &&
        installNestedFunctionProbe(runtime, 0x0019F5B0u, "FUN_0019F5B0") &&
        installNestedFunctionProbe(runtime, 0x001A40B0u, "FUN_001A40B0") &&
        installNestedFunctionProbe(runtime, 0x001A1890u, "FUN_001A1890") &&
        installNestedFunctionProbe(runtime, 0x001A2F40u, "FUN_001A2F40") &&
        installNestedFunctionProbe(runtime, 0x001A66A0u, "FUN_001A66A0") &&
        installNestedFunctionProbe(runtime, 0x001B72B0u, "FUN_001B72B0") &&
        installNestedFunctionProbe(runtime, 0x001A2FD0u, "FUN_001A2FD0") &&
        installNestedFunctionProbe(runtime, 0x0019F670u, "FUN_0019F670") &&
        installNestedFunctionProbe(runtime, 0x001A19B0u, "FUN_001A19B0") &&
        installNestedFunctionProbe(runtime, 0x001A41D0u, "FUN_001A41D0") &&
        installNestedFunctionProbe(runtime, 0x0010CEE0u, "FUN_0010CEE0") &&
        installNestedFunctionProbe(runtime, 0x001DADC0u, "FUN_001DADC0") &&
        installNestedFunctionProbe(runtime, 0x0042A2A0u, "FUN_0042A2A0") &&
        installNestedFunctionProbe(runtime, 0x0042A200u, "FUN_0042A200") &&
        installNestedFunctionProbe(runtime, 0x0042B3B0u, "FUN_0042B3B0") &&
        installNestedFunctionProbe(runtime, 0x0042B3A0u, "FUN_0042B3A0") &&
        installNestedFunctionProbe(runtime, 0x0042B400u, "FUN_0042B400") &&
        installNestedFunctionProbe(runtime, 0x001DAD80u, "FUN_001DAD80") &&
        installNestedFunctionProbe(runtime, 0x001DADA0u, "FUN_001DADA0") &&
        installNestedFunctionProbe(runtime, 0x004295B0u, "FUN_004295B0") &&
        installNestedFunctionProbe(runtime, 0x004288F0u, "FUN_004288F0") &&
        installNestedFunctionProbe(runtime, 0x001B33C0u, "FUN_001B33C0_gate_writer") &&
        installNestedFunctionProbe(runtime, 0x001B5D20u, "FUN_001B5D20_gate_writer") &&
        installNestedFunctionProbe(runtime, 0x001CFE70u, "FUN_001CFE70_gate_writer") &&
        installNestedFunctionProbe(runtime, 0x001AEA70u, "FUN_001AEA70_display_irq_init") &&
        installNestedFunctionProbe(runtime, 0x001AD520u, "FUN_001AD520_wait_path");
    std::cout << "[Armageddon HLE heap probe] installed="
              << (heapProbeInstalled ? "true" : "false")
              << " nested_initializer_probes=" << (initializerProbesInstalled ? "true" : "false") << "\n";
    if (!heapProbeInstalled || !initializerProbesInstalled)
    {
        std::cerr << "Failed to install a startup heap probe; refusing this diagnostic run\n";
        return 5;
    }

    const char *fastMathSetting = std::getenv("PS2X_ARMAGEDDON_HLE_FAST_MATH");
    if (fastMathSetting && std::string(fastMathSetting) == "1")
    {
        if (runtime.lookupFunction(0x00122580u) == nullptr ||
            !runtime.replaceFunction(0x00122580u, hleStartupPow))
        {
            std::cerr << "Failed to install HLE startup math override at 0x00122580\n";
            return 7;
        }
        std::cout << "[Armageddon HLE math] installed helper=0x122580"
                  << " mode=host-std-pow-float approximation\n";
    }

    std::cout << "[Armageddon HLE GUI] profile=HLE-boot"
              << " graphics=runtime-GS-Raylib"
              << " missing_function_policy=Stop"
              << "\n";
    runtime.run();
    return 0;
}

