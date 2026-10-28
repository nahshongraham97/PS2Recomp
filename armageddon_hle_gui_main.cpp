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

void FUN_0032d040_hle_full_0x32d040(uint8_t *, R5900Context *, PS2Runtime *);
void entry_003293c0_hle_0x3293c0(uint8_t *, R5900Context *, PS2Runtime *);

#if defined(PS2X_ARMAGEDDON_SUPPLEMENTAL_REGISTRATION)
bool registerArmageddonSupplementalFunctions();
#endif
bool registerArmageddonBulkOwnerGaps(PS2Runtime &runtime);

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
    std::mutex gHleAllocatorMutex;
    std::mutex gHleModuleMutex;
    std::unordered_map<std::string, uint32_t> gHleModuleIds;
    uint32_t gNextHleModuleId = 0x12u;
    uint32_t gHleAllocatorNext = 0x00A00000u;
    uint64_t gHleAllocatorCalls = 0u;
    uint64_t gHlePowCalls = 0u;

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
        const uint32_t backingHeap = descriptor[0];
        const uint32_t backingHeapType = readGuestWord(rdram, backingHeap + 0x14u);
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
        if (isTargetAllocation)
        {
            dumpStartupHeapState("manager-allocation-before", rdram, ctx);
            std::cout << "[Armageddon HLE allocation request] pool=0x" << std::hex
                      << getRegU32(ctx, 4) << " size=0x" << getRegU32(ctx, 5)
                      << " alignment=0x" << getRegU32(ctx, 6) << "\n" << std::dec;
        }
        if (gOriginalFirstManagerAllocation)
            gOriginalFirstManagerAllocation(rdram, ctx, runtime);
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
        // The guest calls the original power helper once per table element;
        // use the host's single-precision libm implementation, preserve the
        // R5900 FPU result register, and return to the guest caller.
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
        std::cerr << "Armageddon bulk owner-gap registration failed\n";
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


    if (!runtime.replaceFunction(0x003293c0u, entry_003293c0_hle_0x3293c0))
    {
        std::cerr << "Failed to register verified secondary code range 0x003293C0\n";
        return 6;
    }
    std::cout << "[Armageddon HLE code-range registration] address=0x3293c0"
              << " source=retail-ELF bytes=[0x3293c0,0x3293f0) registered=true\n";

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
        installProbe(runtime, 0x00115268u, probeSifRequestStateCheck, gOriginalSifRequestStateCheck);
    std::cout << "[Armageddon HLE heap probe] installed="
              << (heapProbeInstalled ? "true" : "false") << "\n";
    if (!heapProbeInstalled)
    {
        std::cerr << "Failed to install a startup heap probe; refusing this diagnostic run\n";
        return 5;
    }

    std::cout << "[Armageddon HLE GUI] profile=HLE-boot"
              << " graphics=runtime-GS-Raylib"
              << " missing_function_policy=Stop"
              << "\n";
    runtime.run();
    return 0;
}
