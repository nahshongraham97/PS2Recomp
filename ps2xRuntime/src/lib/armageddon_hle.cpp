#include "armageddon_hle.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vfs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace armageddon_hle
{
void populatePadBuffer(PadDataNew &buf, uint32_t frameVal, uint32_t counterVal, uint8_t state, uint8_t reqState)
{
    std::memset(&buf, 0, sizeof(PadDataNew));
    buf.frame = frameVal;
    buf.modeTable[0] = 7u; // DualShock mode ID
    buf.modeTable[1] = 4u; // Digital mode ID
    buf.frameCounter = counterVal;
    buf.data[0] = 0x00u;
    buf.data[1] = 0x79u; // DualShock 2 ID
    buf.data[2] = 0xFFu; // Buttons unpressed
    buf.data[3] = 0xFFu;
    buf.data[4] = 0x7Fu; // Analogs centered
    buf.data[5] = 0x7Fu;
    buf.data[6] = 0x7Fu;
    buf.data[7] = 0x7Fu;
    buf.modeConfig = 2u;   // 2 supported modes
    buf.modeCurId = 0x79u; // DualShock 2 type
    buf.mode = 7u;         // DualShock mode
    buf.lock = 3u;         // Locked in analog mode
    buf.state = state;
    buf.reqState = reqState;
    buf.currentTask = 1u;
    buf.length = 32u;
}
    namespace
    {
        std::mutex gStdioHandleMutex;
        std::deque<int32_t> gPendingStdioDescriptors;
        std::unordered_map<uint32_t, int32_t> gGuestStdioDescriptors;

        bool equalsIgnoreCase(const std::string &left, const char *right)
        {
            if (!right || left.size() != std::strlen(right))
                return false;
            for (size_t i = 0; i < left.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(left[i])) !=
                    std::tolower(static_cast<unsigned char>(right[i])))
                    return false;
            }
            return true;
        }

        void logProfileOnce()
        {
            static std::once_flag once;
            std::call_once(once, []
            {
                const bool rpcDiscovery = std::getenv("PS2X_ARMAGEDDON_DISCOVERY_RPC") != nullptr;
                std::cout << "[Armageddon HLE] profile=HLE-boot"
                          << " iop_reset=skipped"
                          << " flags=assumed-SIFINIT|CMDINIT|BOOTEND"
                          << " sreg0=assumed-1"
                          << " known_fileio=host-vfs unknown_rpc="
                          << (rpcDiscovery ? "discovery-only-zero-reply-up-to-4-bytes"
                                           : "fail-stop")
                          << std::endl;
            });
        }

        bool readRpcPayload(PS2Runtime *runtime,
                            uint8_t *rdram,
                            const ps2x::iop::RpcRequest &request,
                            std::vector<uint8_t> &payload)
        {
            constexpr uint32_t kMaxFileIoRpcBytes = 0x10000u;
            if (request.send.size > kMaxFileIoRpcBytes)
                return false;
            payload.resize(request.send.size);
            if (request.send.size == 0u)
                return true;
            if (runtime->isIopMemoryRange(request.send.address, request.send.size))
                return runtime->readIopMemory(request.send.address, payload.data(), request.send.size);

            const uint64_t end = static_cast<uint64_t>(request.send.address) + request.send.size;
            if (end > UINT32_MAX + 1ull)
                return false;
            const uint8_t *const source = getConstMemPtr(rdram, request.send.address);
            const uint8_t *const last = getConstMemPtr(rdram, static_cast<uint32_t>(end - 1u));
            if (!source || !last || last != source + request.send.size - 1u)
                return false;
            std::memcpy(payload.data(), source, request.send.size);
            return true;
        }

        uint32_t readWord(const std::vector<uint8_t> &bytes, size_t offset)
        {
            uint32_t value = 0u;
            if (offset + sizeof(value) <= bytes.size())
                std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        bool writeRpcReceive(uint8_t *rdram,
                             const ps2x::iop::RpcRequest &request,
                             const void *bytes,
                             uint32_t size)
        {
            if (size > request.receive.size || request.receive.address == 0u)
                return false;
            uint8_t *const destination = getMemPtr(rdram, request.receive.address);
            if (!destination)
                return false;
            std::memcpy(destination, bytes, size);
            return true;
        }

        bool clearGuestRange(uint8_t *rdram, uint32_t address, uint32_t size)
        {
            if (size == 0u)
                return true;
            if (size > PS2_RAM_SIZE || address == 0u)
                return false;
            const uint64_t endAddress = static_cast<uint64_t>(address) + size - 1u;
            uint8_t *const destination = getMemPtr(rdram, address);
            const uint8_t *const last = endAddress <= UINT32_MAX
                                            ? getConstMemPtr(rdram, static_cast<uint32_t>(endAddress))
                                            : nullptr;
            if (!destination || !last || last != destination + size - 1u)
                return false;
            std::memset(destination, 0, size);
            return true;
        }

        ps2x::iop::RpcResult finishHostRpc(const ps2x::iop::RpcRequest &request)
        {
            ps2x::iop::RpcResult result{};
            result.handled = true;
            result.resultAddress = request.receive.address;
            result.signalCompletion = true;
            result.signalNowaitCompletion = true;
            result.callbackPolicy = ps2x::iop::CallbackPolicy::Suppress;
            result.serverDispatchPolicy = ps2x::iop::ServerDispatchPolicy::Suppress;
            return result;
        }

        int signalStdioOperationCompletion(PS2Runtime *runtime,
                                           const std::vector<uint8_t> &payload,
                                           uint32_t function)
        {
            if (!runtime || function == 0xFFu || payload.size() < sizeof(uint32_t))
                return -1;

            // The guest request starts with its per-operation completion
            // semaphore. For rpc=0 the captured request word 0x19 matches the
            // semaphore created by FUN_00111CF0 immediately before submission.
            const uint32_t semaphoreId = readWord(payload, 0u);
            if (semaphoreId == 0u || semaphoreId > static_cast<uint32_t>(INT32_MAX))
                return -1;

            // A host-side SceStdio operation completes synchronously in this
            // HLE profile, so publish the guest operation completion now.
            return runtime->eeScheduler().signalSemaphore(static_cast<int>(semaphoreId), true);
        }

        bool writeStdioOperationResult(uint8_t *rdram,
                                       const std::vector<uint8_t> &payload,
                                       int32_t operationResult,
                                       uint32_t &resultAddress,
                                       uint32_t &resultSize)
        {
            // The SceStdio request carries its own result pointer and size at
            // words 1 and 2. The EE wrapper returns this stack result after
            // WaitSema; writing only the separate SIF RPC receive buffer leaves
            // open/seek/read wrappers with stale return values.
            resultAddress = readWord(payload, sizeof(uint32_t));
            resultSize = readWord(payload, 2u * sizeof(uint32_t));
            if (!rdram || resultAddress == 0u || resultSize < sizeof(operationResult))
                return false;
            uint8_t *const destination = getMemPtr(rdram, resultAddress);
            if (!destination)
                return false;
            std::memcpy(destination, &operationResult, sizeof(operationResult));
            return true;
        }

        void rememberStdioDescriptor(int32_t descriptor)
        {
            if (descriptor < 0)
                return;
            std::lock_guard<std::mutex> lock(gStdioHandleMutex);
            gPendingStdioDescriptors.push_back(descriptor);
        }

        int32_t resolveStdioDescriptor(uint32_t guestHandle)
        {
            constexpr uint32_t kOpaqueGuestHandleFloor = 0x10000u;
            if (guestHandle != 0u && guestHandle < kOpaqueGuestHandleFloor)
                return static_cast<int32_t>(guestHandle);

            std::lock_guard<std::mutex> lock(gStdioHandleMutex);
            const auto found = gGuestStdioDescriptors.find(guestHandle);
            if (found != gGuestStdioDescriptors.end())
                return found->second;
            if (gPendingStdioDescriptors.empty())
                return guestHandle == 0u ? 0 : -1;
            const int32_t descriptor = gPendingStdioDescriptors.front();
            gPendingStdioDescriptors.pop_front();
            gGuestStdioDescriptors.emplace(guestHandle, descriptor);
            std::cout << "[Armageddon HLE FILEIO handle-map] guest=0x" << std::hex << guestHandle
                      << " host_fd=" << std::dec << descriptor
                      << " policy=" << (guestHandle == 0u ? "zero-token-binds-oldest-open" : "opaque-token-binds-oldest-open")
                      << std::endl;
            return descriptor;
        }

        void forgetStdioDescriptor(uint32_t guestHandle)
        {
            std::lock_guard<std::mutex> lock(gStdioHandleMutex);
            gGuestStdioDescriptors.erase(guestHandle);
        }

        bool isBoundedString(const std::vector<uint8_t> &bytes, size_t offset, std::string &out)
        {
            out.clear();
            if (offset >= bytes.size())
                return false;
            const size_t limit = std::min(bytes.size(), offset + 1024u);
            for (size_t i = offset; i < limit; ++i)
            {
                const char ch = static_cast<char>(bytes[i]);
                if (ch == '\0')
                    return !out.empty();
                out.push_back(ch);
            }
            return false;
        }

        ps2x::iop::RpcResult handleStdioRpc(PS2Runtime *runtime,
                                            uint8_t *rdram,
                                            const ps2x::iop::RpcRequest &request)
        {
            std::vector<uint8_t> payload;
            if (!readRpcPayload(runtime, rdram, request, payload))
            {
                std::cerr << "[Armageddon HLE FILEIO] rejected payload range"
                          << " rpc=0x" << std::hex << request.function
                          << " address=0x" << request.send.address
                          << " size=0x" << request.send.size << std::dec << std::endl;
                return {};
            }

            if (request.function == 0xFFu)
            {
                // The EE's SceStdio guard compares the returned token against
                // the word stored at 0x0059810C. The ELF bytes there are ASCII
                // "3100" in little-endian order (0x30303133). Return that
                // embedded word, not its address. Mode 2 remains an explicit
                // HLE profile assumption rather than a recovered IOP reply.
                const uint32_t versionToken = 0x30303133u;
                const uint32_t mode = 2u;
                const uint32_t reply[2] = {versionToken, mode};
                if (request.receive.size < sizeof(reply) ||
                    !writeRpcReceive(rdram, request, reply, sizeof(reply)))
                    return {};
                std::cout << "[Armageddon HLE FILEIO] rpc=0xff version_token=0x30303133"
                          << " mode=2 assumption=guest-embedded-3100-profile-token" << std::endl;
                return finishHostRpc(request);
            }

            if (request.function == 0u)
            {
                constexpr size_t kOpenPathOffset = 0x14u;
                constexpr size_t kOpenFlagsOffset = 0x0Cu;
                std::string path;
                int32_t descriptor = -1;
                const uint32_t flags = readWord(payload, kOpenFlagsOffset);
                if (request.receive.size >= sizeof(uint32_t) &&
                    isBoundedString(payload, kOpenPathOffset, path))
                {
                    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
                    const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
                    descriptor = runtime->vfs().open(path, flags, mounts, runtime->romDevice());
                }
                const uint32_t reply = static_cast<uint32_t>(descriptor);
                if (request.receive.size < sizeof(reply) ||
                    !writeRpcReceive(rdram, request, &reply, sizeof(reply)))
                    return {};
                uint32_t operationResultAddress = 0u;
                uint32_t operationResultSize = 0u;
                if (!writeStdioOperationResult(rdram, payload, descriptor,
                                               operationResultAddress, operationResultSize))
                {
                    std::cerr << "[Armageddon HLE FILEIO] missing-operation-result-buffer"
                              << " rpc=0 address=0x" << std::hex << operationResultAddress
                              << " size=0x" << operationResultSize << std::dec << std::endl;
                    return {};
                }
                rememberStdioDescriptor(descriptor);
                const int completionResult = signalStdioOperationCompletion(runtime, payload, request.function);
                std::cout << "[Armageddon HLE FILEIO] rpc=0 open path=\"" << path
                          << "\" flags=0x" << std::hex << flags << std::dec
                          << " fd=" << descriptor << " source=host-vfs"
                          << " operation_result=0x" << std::hex << operationResultAddress
                          << "/0x" << operationResultSize << std::dec
                          << " completion_sema=" << readWord(payload, 0u)
                          << " completion_result=" << completionResult
                          << " completion=host-synchronous-HLE" << std::endl;
                return finishHostRpc(request);
            }

            if (request.function == 1u || request.function == 2u ||
                request.function == 3u || request.function == 4u)
            {
                constexpr size_t kFdOffset = 0x0Cu;
                const uint32_t guestHandle = readWord(payload, kFdOffset);
                const int32_t descriptor = resolveStdioDescriptor(guestHandle);
                int32_t operationResult = -1;
                uint32_t bufferAddress = 0u;
                uint32_t count = 0u;
                int32_t offset = 0;
                int32_t whence = 0;
                if (request.function == 2u && payload.size() >= 0x18u)
                {
                    bufferAddress = readWord(payload, 0x10u);
                    count = readWord(payload, 0x14u);
                    constexpr uint32_t kMaxReadBytes = 16u * 1024u * 1024u;
                    uint8_t *const destination = getMemPtr(rdram, bufferAddress);
                    if (count <= kMaxReadBytes && (count == 0u || destination))
                    {
                        const int64_t bytesRead = runtime->vfs().read(descriptor, destination, count);
                        if (bytesRead >= 0 && bytesRead <= INT32_MAX)
                            operationResult = static_cast<int32_t>(bytesRead);
                    }
                    std::cout << "[Armageddon HLE FILEIO] rpc=2 read fd=" << descriptor
                              << " guest_dst=0x" << std::hex << bufferAddress
                              << " bytes=0x" << count << std::dec
                              << " result=" << operationResult << std::endl;
                }
                else if (request.function == 1u || request.function == 3u)
                {
                    operationResult = runtime->vfs().close(descriptor);
                    std::cout << "[Armageddon HLE FILEIO] rpc=0x" << std::hex << request.function
                              << " close guest_handle=0x" << guestHandle << std::dec
                              << " fd=" << descriptor
                              << " result=" << operationResult << std::endl;
                    forgetStdioDescriptor(guestHandle);
                }
                else if (payload.size() >= 0x18u)
                {
                    offset = static_cast<int32_t>(readWord(payload, 0x10u));
                    whence = static_cast<int32_t>(readWord(payload, 0x14u));
                    if (whence == 0 || whence == 1 || whence == 2)
                    {
                        const int64_t position = runtime->vfs().seek(descriptor, offset, whence);
                        if (position >= 0 && position <= INT32_MAX)
                            operationResult = static_cast<int32_t>(position);
                    }
                    std::cout << "[Armageddon HLE FILEIO] rpc=4 seek fd=" << descriptor
                              << " offset=" << offset << " whence=" << whence
                              << " result=" << operationResult << std::endl;
                }
                const uint32_t reply = static_cast<uint32_t>(operationResult);
                if (request.receive.size < sizeof(reply) ||
                    !writeRpcReceive(rdram, request, &reply, sizeof(reply)))
                    return {};
                uint32_t operationResultAddress = 0u;
                uint32_t operationResultSize = 0u;
                if (!writeStdioOperationResult(rdram, payload, operationResult,
                                               operationResultAddress, operationResultSize))
                {
                    std::cerr << "[Armageddon HLE FILEIO] missing-operation-result-buffer"
                              << " rpc=0x" << std::hex << request.function
                              << " address=0x" << operationResultAddress
                              << " size=0x" << operationResultSize << std::dec << std::endl;
                    return {};
                }
                const int completionResult = signalStdioOperationCompletion(runtime, payload, request.function);
                std::cout << "[Armageddon HLE FILEIO completion] rpc=0x" << std::hex << request.function
                          << " guest_handle=0x" << guestHandle << " host_fd=" << std::dec << descriptor
                          << " operation_result=" << operationResult
                          << " result_buffer=0x" << std::hex << operationResultAddress
                          << "/0x" << operationResultSize << std::dec
                          << " completion_sema=" << readWord(payload, 0u)
                          << " completion_result=" << completionResult << std::endl;
                return finishHostRpc(request);
            }

            if (request.function == 0xFDu || request.function == 0xFEu)
            {
                const uint32_t reply = 0u;
                if (request.receive.size < sizeof(reply) ||
                    !writeRpcReceive(rdram, request, &reply, sizeof(reply)))
                    return {};
                std::cout << "[Armageddon HLE FILEIO] rpc=0x" << std::hex << request.function
                          << " documented-default=success-zero" << std::dec << std::endl;
                return finishHostRpc(request);
            }

            std::cerr << "[Armageddon HLE FILEIO] unknown function=0x" << std::hex
                      << request.function << " send=0x" << request.send.size
                      << " receive=0x" << request.receive.size
                      << " documented-default=zero" << std::dec << std::endl;
            return finishHostRpc(request);
        }
    }

    bool enabled(const PS2Runtime *runtime) noexcept
    {
        const char *const profile = std::getenv("PS2X_ARMAGEDDON_HLE_BOOT");
        if (!profile || std::strcmp(profile, "1") != 0 || !runtime)
            return false;
        return equalsIgnoreCase(PS2Runtime::getIoPaths().elfPath.filename().string(), "SLUS_214.10");
    }

    bool readSifRegister(const PS2Runtime *runtime, uint32_t index, uint32_t &value)
    {
        if (!enabled(runtime) || index != 4u)
            return false;
        value = 0x00070000u; // HLE assumptions: SIFINIT | CMDINIT | BOOTEND.
        logProfileOnce();
        static std::once_flag once;
        std::call_once(once, []
        {
            std::cout << "[Armageddon HLE assumption] sceSifGetReg(4)=0x00070000"
                      << " publisher=host profile=HLE-boot" << std::endl;
        });
        return true;
    }

    bool readSifSreg(const PS2Runtime *runtime, uint8_t *rdram, uint32_t index, uint32_t &value)
    {
        if (!enabled(runtime) || index != 0u)
            return false;
        value = 1u; // HLE assumption: host answers the EE's SREG[0] wait.
        logProfileOnce();
        if (rdram)
        {
            if (uint8_t *const slot = getMemPtr(rdram, 0x006505C0u))
                std::memcpy(slot, &value, sizeof(value));
        }
        static std::once_flag once;
        std::call_once(once, []
        {
            std::cout << "[Armageddon HLE assumption] sceSifGetSreg(0)=1"
                      << " guest_slot=0x006505C0 publisher=host profile=HLE-boot" << std::endl;
        });
        return true;
    }

    bool skipIopReset(const PS2Runtime *runtime,
                      const R5900Context *context,
                      const char *operation)
    {
        if (!enabled(runtime))
            return false;
        static std::mutex mutex;
        static std::set<std::string> logged;
        const std::string name = operation ? operation : "unknown";
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.emplace(name).second)
        {
            std::cout << "[Armageddon HLE] skipped=" << name
                      << " pc=0x" << std::hex << (context ? context->pc : 0u) << std::dec
                      << " reason=IOP-startup-bypassed" << std::endl;
        }
        return true;
    }

    bool canBindRpc(const PS2Runtime *runtime, uint32_t sid)
    {
        if (!enabled(runtime))
            return false;
        logProfileOnce();
        static std::mutex mutex;
        static std::array<uint32_t, 64u> logged{};
        static size_t loggedCount = 0u;
        std::lock_guard<std::mutex> lock(mutex);
        const auto end = logged.begin() + static_cast<std::ptrdiff_t>(loggedCount);
        if (std::find(logged.begin(), end, sid) == end)
        {
            if (loggedCount < logged.size())
                logged[loggedCount++] = sid;
            std::cout << "[Armageddon HLE RPC bind] sid=0x" << std::hex << sid
                      << " server=host-default" << std::dec << std::endl;
        }
        return true;
    }

    ps2x::iop::RpcResult handleRpc(const PS2Runtime *runtime,
                                   uint8_t *rdram,
                                   const ps2x::iop::RpcRequest &request)
    {
        ps2x::iop::RpcResult result{};
        if (!enabled(runtime))
            return result;

        const char *service = "unknown";
        if (request.sid == 0x80000003u)
            service = "FILEIO-HLE-pending-protocol";
        else if (request.sid == 0x80000001u)
            service = "SceStdio-FILEIO-HLE";
        else if (request.sid == 0x80000592u)
            service = "CDVDFSV-HLE";
        else if (request.sid == 0x80000593u)
            service = "CDVDFSV-SCMD";
        else if (request.sid == 0x8000059Cu)
            service = "CDVDFSV-DiskReady";
        else if (request.sid == 0x80000006u)
            service = "LOADCORE-HLE";
        else if (request.sid == 0x80000400u || request.sid == 0x80000480u)
            service = "memory-card-HLE-no-card";
        else if (request.sid == 0x80000701u)
            service = "sound-HLE-default";
        else if (request.sid == 0x80000100u || request.sid == 0x80000101u)
            service = "PADMAN-HLE-command-contract";
        else if (request.sid == 0x61766973u)
            service = "lgkbm-compatible-keyboard-rpc-candidate";

        const bool observedRequestSupported =
            (request.sid == 0x80000001u &&
             (request.function == 0xFFu || request.function <= 4u ||
              request.function == 0xFDu || request.function == 0xFEu)) ||
            (request.sid == 0x80000592u && request.function == 0u) ||
            (request.sid == 0x80000593u &&
             (request.function == 0x03u || request.function == 0x22u)) ||
            (request.sid == 0x8000059Cu && request.function == 0u) ||
            (request.sid == 0x80000006u &&
             (request.function == 0xFFu || request.function == 0x09u)) ||
            (request.sid == 0x000F0001u && request.function == 1u) ||
            ((request.sid == 0x80000400u || request.sid == 0x80000480u) &&
             request.function == 0xFEu) ||
            ((request.sid == 0x80000100u || request.sid == 0x80000101u) &&
             request.function == 1u) ||
            (request.sid == 0x61766973u &&
             (request.function == 1u || request.function == 2u || request.function == 3u));

        static std::mutex mutex;
        static std::set<std::tuple<uint32_t, uint32_t>> logged;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (logged.emplace(request.sid, request.function).second)
            {
                std::ostringstream preview;
                const uint32_t previewBytes = std::min<uint32_t>(request.send.size, 32u);
                std::array<uint8_t, 32u> iopPreview{};
                const bool sendIsIopMemory = previewBytes != 0u && runtime->isIopMemoryRange(request.send.address, previewBytes) &&
                                             runtime->readIopMemory(request.send.address, iopPreview.data(), previewBytes);
                for (uint32_t i = 0u; i < previewBytes; ++i)
                {
                    const uint8_t *byte = nullptr;
                    if (sendIsIopMemory)
                        byte = &iopPreview[i];
                    else
                        byte = getConstMemPtr(rdram, request.send.address + i);
                    if (!byte) break;
                    if (i != 0u)
                        preview << ' ';
                    preview << std::hex << std::uppercase << std::setw(2)
                            << std::setfill('0') << static_cast<unsigned>(*byte);
                }
                std::cout << "[Armageddon HLE RPC call] sid=0x" << std::hex << request.sid
                          << " rpc=0x" << request.function
                          << " send=0x" << request.send.address << "/0x" << request.send.size
                          << " recv=0x" << request.receive.address << "/0x" << request.receive.size
                          << " sendBytes=[" << preview.str() << "]"
                          << " service=" << service
                          << " default=" << (observedRequestSupported
                                                   ? "documented-or-explicit-hle-contract"
                                                   : "unimplemented-fail-stop")
                          << std::dec << std::endl;
            }
        }

        const bool rpcDiscovery = std::getenv("PS2X_ARMAGEDDON_DISCOVERY_RPC") != nullptr;
        if (!observedRequestSupported && !rpcDiscovery)
        {
            std::cerr << "[Armageddon HLE RPC unimplemented] sid=0x" << std::hex
                      << request.sid << " rpc=0x" << request.function
                      << " send=0x" << request.send.address << "/0x" << request.send.size
                      << " reply=0x" << request.receive.address << "/0x" << request.receive.size
                      << " action=fail-stop-no-synthetic-reply" << std::dec << std::endl;
            return result;
        }

        if (request.receive.size != 0u && request.receive.address != 0u)
        {
            uint8_t *const destination = getMemPtr(rdram, request.receive.address);
            const uint64_t endAddress = static_cast<uint64_t>(request.receive.address) +
                                        static_cast<uint64_t>(request.receive.size) - 1u;
            const uint8_t *const lastByte = endAddress <= UINT32_MAX
                                                ? getConstMemPtr(rdram, static_cast<uint32_t>(endAddress))
                                                : nullptr;
            if (request.receive.size > PS2_RAM_SIZE || !destination || !lastByte ||
                lastByte != destination + request.receive.size - 1u)
            {
                std::cerr << "[Armageddon HLE RPC reject] invalid-receive-range sid=0x"
                          << std::hex << request.sid << " address=0x" << request.receive.address
                          << " size=0x" << request.receive.size << std::dec << std::endl;
                return result;
            }
            std::memset(destination, 0, request.receive.size);
        }

        if (!observedRequestSupported)
        {
            // Discovery-only continuation is deliberately constrained to a
            // four-byte reply. It is a synthetic zero used only to reveal
            // later RPC classes; it is never a supported/final HLE service.
            // Larger or nonstandard replies remain fail-stop to avoid writing
            // an unvalidated structure into guest memory.
            if (request.receive.size > sizeof(uint32_t) ||
                (request.receive.size != 0u && request.receive.address == 0u))
            {
                std::cerr << "[Armageddon HLE RPC discovery] sid=0x" << std::hex
                          << request.sid << " rpc=0x" << request.function
                          << " reply_size=0x" << request.receive.size
                          << " action=fail-stop-unsafe-discovery-reply" << std::dec << std::endl;
                return result;
            }
            std::cerr << "[Armageddon HLE RPC discovery] sid=0x" << std::hex
                      << request.sid << " rpc=0x" << request.function
                      << " send_size=0x" << request.send.size
                      << " reply_size=0x" << request.receive.size
                      << " action=synthetic-zero-only-for-discovery-not-valid-service-result"
                      << std::dec << std::endl;
            return finishHostRpc(request);
        }

        if (request.sid == 0x80000001u)
            return handleStdioRpc(const_cast<PS2Runtime *>(runtime), rdram, request);

        // PS2SDK LOADFILE RPC 9 is LF_F_SEARCH_MOD_BY_NAME. Its request is
        // the 0x200-byte _lf_search_module_by_name_arg (name begins at +8),
        // and the reply is the module id. This HLE profile skips IOP module
        // execution, so a query for USB_driver must report the SDK's
        // KE_UNKNOWN_MODULE (-202), not module id 0 from discovery zero-fill.
        if (request.sid == 0x80000006u && request.function == 0x09u)
        {
            std::vector<uint8_t> payload;
            std::string moduleName;
            if (request.send.size != 0x200u || request.receive.size < sizeof(uint32_t) ||
                request.receive.address == 0u ||
                !readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload) ||
                !isBoundedString(payload, 8u, moduleName))
            {
                std::cerr << "[Armageddon HLE LOADFILE] reject=invalid-search-module-packet"
                          << " send=0x" << std::hex << request.send.size
                          << " reply=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            constexpr int32_t kUnknownModule = -202; // PS2SDK KE_UNKNOWN_MODULE.
            if (!writeRpcReceive(rdram, request, &kUnknownModule, sizeof(kUnknownModule)))
                return {};
            std::cout << "[Armageddon HLE LOADFILE] rpc=0x9 name=\"" << moduleName
                      << "\" result=" << kUnknownModule
                      << " reason=IOP-module-execution-skipped"
                      << " source=PS2SDK-LF_F_SEARCH_MOD_BY_NAME-and-KE_UNKNOWN_MODULE"
                      << std::endl;
            return finishHostRpc(request);
        }

        // Armageddon's mwFile startup sends a 20-byte MWF_RPC_INIT request
        // and continues only when the four-byte result is zero. In this HLE
        // profile, zero means the host VFS can serve the mounted ISO; it does
        // not claim that mwfilefr.irx executed or that later MWF commands are
        // implemented. Later unknown methods remain fail-stop.
        if (request.sid == 0x000F0001u && request.function == 1u)
        {
            std::vector<uint8_t> payload;
            const auto &paths = PS2Runtime::getIoPaths();
            std::error_code fileStatusError;
            const bool imageAvailable = !paths.cdImage.empty() &&
                                        std::filesystem::is_regular_file(paths.cdImage, fileStatusError) &&
                                        !fileStatusError;
            if (request.send.size != 0x14u || request.receive.size < sizeof(uint32_t) ||
                request.receive.address == 0u ||
                !readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload))
            {
                std::cerr << "[Armageddon HLE mwFile] reject=invalid-init-packet"
                          << " send=0x" << std::hex << request.send.size
                          << " reply=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            const int32_t resultCode = imageAvailable ? 0 : -1;
            if (!writeRpcReceive(rdram, request, &resultCode, sizeof(resultCode)))
                return {};
            std::cout << "[Armageddon HLE mwFile] rpc=0x1 result=" << resultCode
                      << " image=\"" << paths.cdImage.string() << "\""
                      << " image_available=" << (imageAvailable ? "true" : "false")
                      << " source=host-vfs-init-contract"
                      << " limitation=MWF-file-operations-not-yet-implemented"
                      << std::endl;
            return finishHostRpc(request);
        }

        // Play! (BSD-3-Clause), Iop_Cdvdfsv.cpp::Invoke592, method 0 provides
        // the concrete CDVDFSV Init reply contract: a 16-byte reply whose
        // fourth word is 0xFF; its version words are left at zero. Adapt that
        // contract to this runner's SIF RPC packet instead of zero-filling the
        // entire reply. This is an HLE service response, not IRX execution.
        if (request.sid == 0x80000592u && request.function == 0u)
        {
            std::vector<uint8_t> payload;
            if (!readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload) ||
                payload.size() < sizeof(uint32_t) || request.receive.size < 4u * sizeof(uint32_t) ||
                request.receive.address == 0u)
            {
                std::cerr << "[Armageddon HLE CDVDFSV] reject=invalid-init-packet"
                          << " send=0x" << std::hex << request.send.address << "/0x" << request.send.size
                          << " recv=0x" << request.receive.address << "/0x" << request.receive.size
                          << std::dec << std::endl;
                return {};
            }
            const std::array<uint32_t, 4u> reply = {0u, 0u, 0u, 0xFFu};
            if (!writeRpcReceive(rdram, request, reply.data(), sizeof(reply)))
                return {};
            std::cout << "[Armageddon HLE CDVDFSV] rpc=0x0 init_mode=0x"
                      << std::hex << readWord(payload, 0u) << " reply_words=0,0,0,0xff"
                      << " source=Play!-BSD-3-Clause-Invoke592-contract"
                      << std::dec << std::endl;
            return finishHostRpc(request);
        }

        // Play! (BSD-3-Clause), Iop_Cdvdfsv.cpp::Invoke59C method 0 returns
        // 2 when the mounted disc is ready (6 when it is not). This HLE
        // profile has a mounted ISO, so return the ready status only for the
        // observed four-byte request/reply contract.
        if (request.sid == 0x8000059Cu && request.function == 0u)
        {
            std::vector<uint8_t> payload;
            if (!readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload) ||
                payload.size() < sizeof(uint32_t) || request.receive.size < sizeof(uint32_t) ||
                request.receive.address == 0u)
            {
                std::cerr << "[Armageddon HLE CDVDFSV] reject=invalid-disk-ready-packet"
                          << " send=0x" << std::hex << request.send.address << "/0x" << request.send.size
                          << " recv=0x" << request.receive.address << "/0x" << request.receive.size
                          << std::dec << std::endl;
                return {};
            }
            const uint32_t ready = 2u;
            if (!writeRpcReceive(rdram, request, &ready, sizeof(ready)))
                return {};
            std::cout << "[Armageddon HLE CDVDFSV] sid=0x8000059c rpc=0x0 mode=0x"
                      << std::hex << readWord(payload, 0u)
                      << " result=2 meaning=disk-ready source=Play!-BSD-3-Clause-Invoke59C-contract"
                      << std::dec << std::endl;
            return finishHostRpc(request);
        }

        // This is the observed 0-byte/4-byte LOADCORE init request; it does
        // not claim that LOADCORE.IRX was executed. Play!'s generic default
        // token is '....' (0x2E2E2E2E), but Armageddon's translated caller
        // compares the response with the retail ELF word "3100" at 0x59810C
        // (sub_00115268 -> memcmp size 4; sub_001157D0 branches on equality).
        // Use that guest-required token only for the explicitly selected
        // Armageddon HLE profile; retain Play!'s default for other callers.
        if (request.sid == 0x80000006u && request.function == 0xFFu)
        {
            const bool armageddonProfile = enabled(runtime);
            const uint32_t versionToken = armageddonProfile ? 0x30303133u : 0x2E2E2E2Eu;
            if (request.send.size != 0u || request.receive.size < sizeof(versionToken) ||
                !writeRpcReceive(rdram, request, &versionToken, sizeof(versionToken)))
            {
                std::cerr << "[Armageddon HLE LOADCORE] reject=invalid-init-packet"
                          << " send_size=0x" << std::hex << request.send.size
                          << " reply_size=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            std::cout << "[Armageddon HLE LOADCORE] rpc=0xff version_token=0x"
                      << std::hex << versionToken << std::dec
                      << " source=" << (armageddonProfile
                          ? "Armageddon-guest-compare-at-0x001157D0-HLE-profile"
                          : "Play!-BSD-3-Clause-Iop_Loadcore-Initialize") << std::endl;
            return finishHostRpc(request);
        }

        // KillzoneRecomp's GPL-3.0 findings document the keyboard service
        // handshake for SID 0x61766973, method 3, with a 0x20-byte request and
        // reply. Armageddon's guest check at 0x005230D0-0x005230E0 compares the
        // reply word at +0x04 with the unaligned word at +0x0A. Preserve the
        // request's version in both fields; the latter is the donor-documented
        // echo. This is a protocol-field adaptation, not copied donor code.
        if (request.sid == 0x61766973u && request.function == 3u)
        {
            if (request.send.size != 0x20u || request.receive.size != 0x20u ||
                request.receive.address == 0u)
            {
                std::cerr << "[Armageddon HLE lgkbm-candidate] reject=unexpected-handshake-size"
                          << " send=0x" << std::hex << request.send.size
                          << " reply=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            std::vector<uint8_t> payload;
            if (!readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload) ||
                payload.size() != 0x20u)
                return {};
            uint32_t version = 0u;
            std::memcpy(&version, payload.data() + 4u, sizeof(version));
            std::array<uint8_t, 0x20u> reply{};
            std::memcpy(reply.data() + 0x04u, &version, sizeof(version));
            std::memcpy(reply.data() + 0x0Au, &version, sizeof(version));
            const uint16_t acceptedEndpoints = 1u;
            std::memcpy(reply.data() + 0x0Eu, &acceptedEndpoints, sizeof(acceptedEndpoints));
            const uint32_t noError = 0u;
            std::memcpy(reply.data() + 0x10u, &noError, sizeof(noError));
            if (!writeRpcReceive(rdram, request, reply.data(), reply.size()))
                return {};
            std::cout << "[Armageddon HLE lgkbm-candidate] sid=0x61766973 rpc=0x3"
                      << " request_size=0x20 reply_size=0x20 version=0x" << std::hex << version
                      << " accepted_endpoints=1 error=0"
                      << " guest_compare=reply[0x04]==reply[0x0A]"
                      << " source=KillzoneRecomp-GPL-3.0-findings-protocol-only"
                      << " client=Armageddon-overlay-RPC-observed" << std::dec << std::endl;
            return finishHostRpc(request);
        }

        // The guest follows method 3 with method 1 to enumerate USB devices.
        // The protocol body has a u16 count at +0x02 and 0x10-byte records at
        // +0x04. The Armageddon worker copies reply byte +0 into its
        // device-state field and retries enumeration while that state is zero
        // (guest code at 0x0052324c-0x005232f0). Expose one active but inert
        // host endpoint so startup can leave that loop. This active-state
        // value is an explicit HLE assumption, not a retail-device observation.
        if (request.sid == 0x61766973u && request.function == 1u)
        {
            if (request.send.size != 0x90u || request.receive.size != 0x90u ||
                request.receive.address == 0u)
            {
                std::cerr << "[Armageddon HLE lgkbm] reject=unexpected-device-list-size"
                          << " send=0x" << std::hex << request.send.size
                          << " reply=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            std::array<uint8_t, 0x90u> reply{};
            reply[0] = 1u; // HLE endpoint active; guest retries while this is zero.
            const uint16_t deviceCount = 1u;
            const uint16_t keyboardType = 1u;
            const uint16_t keyboardId = 1u;
            std::memcpy(reply.data() + 0x06u, &deviceCount, sizeof(deviceCount));
            std::memcpy(reply.data() + 0x0Au, &keyboardType, sizeof(keyboardType));
            std::memcpy(reply.data() + 0x0Cu, &keyboardId, sizeof(keyboardId));
            if (!writeRpcReceive(rdram, request, reply.data(), reply.size()))
                return {};
            std::cout << "[Armageddon HLE lgkbm] sid=0x61766973 rpc=0x1"
                      << " request_size=0x90 reply_size=0x90 device_count=1"
                      << " device_type=keyboard device_id=1"
                      << " endpoint_state=1"
                      << " assumption=inert-virtual-keyboard-no-events"
                      << " source=KillzoneRecomp-GPL-3.0-findings-protocol-only"
                      << std::endl;
            return finishHostRpc(request);
        }

        // Method 2 polls the endpoint's event queue. The Armageddon guest
        // calls it with 0x1D0-byte request/reply buffers and proceeds through
        // its no-event branch when the first result byte is zero. The donor
        // protocol notes place the event count at body +0x02 and entries at
        // body +0x04; an all-zero body therefore represents an empty queue.
        // This is the explicit behavior of our inert, no-input HLE endpoint.
        if (request.sid == 0x61766973u && request.function == 2u)
        {
            if (request.send.size != 0x1D0u || request.receive.size != 0x1D0u ||
                request.receive.address == 0u)
            {
                std::cerr << "[Armageddon HLE lgkbm] reject=unexpected-event-poll-size"
                          << " send=0x" << std::hex << request.send.size
                          << " reply=0x" << request.receive.size << std::dec << std::endl;
                return {};
            }
            std::array<uint8_t, 0x1D0u> reply{};
            reply[0] = 1u; // Endpoint state matching RPC 1 to prevent renegotiation loop
            if (!writeRpcReceive(rdram, request, reply.data(), reply.size()))
                return {};
            static std::atomic<uint64_t> eventPollCount{0u};
            const uint64_t poll = eventPollCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (poll == 1u || (poll % 1000u) == 0u)
            {
                std::cout << "[Armageddon HLE lgkbm] sid=0x61766973 rpc=0x2"
                          << " request_size=0x1d0 reply_size=0x1d0 event_count=0"
                          << " poll_count=" << poll
                          << " assumption=inert-virtual-keyboard-no-key-events"
                          << " source=KillzoneRecomp-GPL-3.0-findings-protocol-only"
                          << std::endl;
            }
            return finishHostRpc(request);
        }

        // This profile deliberately models an absent card. Service init
        // succeeds and reports protocol versions, while card operations
        // report the documented card-detect error and GetInfo returns the
        // explicit NoCard type with zero capacity/format fields.
        if (request.sid == 0x80000400u || request.sid == 0x80000480u)
        {
            constexpr int32_t kMcResultFailDetect = -12; // sceMcResFailDetect
            // The retail startup gate at 0x0011BFA0 rejects versions below
            // 0x020A (MCSERV) and 0x020E (MCMAN). Report those minimum
            // compatible service versions; card presence is reported only by
            // later card-query RPCs and remains absent in this HLE profile.
            constexpr uint32_t kMcservVersion = 0x020Au;
            constexpr uint32_t kMcmanVersion = 0x020Eu;
            std::vector<uint8_t> payload;
            if (!readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload))
            {
                std::cerr << "[Armageddon HLE MCSERV] rejected-payload rpc=0x"
                          << std::hex << request.function << std::dec << std::endl;
                return {};
            }

            if (request.function == 0xFEu)
            {
                const std::array<uint32_t, 3> reply = {0u, kMcservVersion, kMcmanVersion};
                const uint32_t bytes = std::min<uint32_t>(request.receive.size, sizeof(reply));
                if (bytes != 0u && !writeRpcReceive(rdram, request, reply.data(), bytes))
                    return {};
                std::cout << "[Armageddon HLE MCSERV] rpc=0xfe init_result=0"
                          << " mcserv=0x" << std::hex << kMcservVersion
                          << " mcman=0x" << kMcmanVersion
                          << " assumption=service-available-card-absent" << std::dec << std::endl;
                return finishHostRpc(request);
            }

            // New XMCSERV GetInfo uses a 48-byte descriptor and a 192-byte
            // output record; the type/free/formatted values are at 0, 4, 144.
            if (request.function == 0x01u && payload.size() >= 48u)
            {
                const uint32_t output = readWord(payload, 28u);
                if (output != 0u && !clearGuestRange(rdram, output, 192u))
                    return {};
            }
            else if (request.function == 0x05u && payload.size() >= 48u)
            {
                const uint32_t output = readWord(payload, 28u);
                if (output != 0u && !clearGuestRange(rdram, output, 192u))
                    return {};
            }

            const uint32_t error = static_cast<uint32_t>(kMcResultFailDetect);
            if (request.receive.size >= sizeof(error) &&
                !writeRpcReceive(rdram, request, &error, sizeof(error)))
                return {};
            std::cout << "[Armageddon HLE MCSERV] sid=0x" << std::hex << request.sid
                      << " rpc=0x" << request.function << " result=" << std::dec
                      << kMcResultFailDetect
                      << " card_type=0 free=0 formatted=0 assumption=no-card-present" << std::endl;
            return finishHostRpc(request);
        }

        // Pad's EE library sends a command word inside the RPC payload. Its
        // response union stores padResult.result at byte offset 0x0c, after
        // three reserved words. The game requires a version whose high byte
        // is 4; the SDK 3.1.0-compatible PADMAN version 0x0422 is the HLE
        // assumption used here. INIT (0x10) must return 1 for padPortInit.
        // This does not claim that PADMAN.IRX executed.
        if ((request.sid == 0x80000100u || request.sid == 0x80000101u) &&
            request.function == 1u && request.receive.size >= 4u * sizeof(uint32_t) &&
            request.receive.address != 0u)
        {
            std::vector<uint8_t> payload;
            if (!readRpcPayload(const_cast<PS2Runtime *>(runtime), rdram, request, payload) ||
                payload.size() < sizeof(uint32_t))
            {
                std::cerr << "[Armageddon HLE PAD] rejected-request-payload"
                          << " sid=0x" << std::hex << request.sid
                          << " address=0x" << request.send.address
                          << " size=0x" << request.send.size << std::dec << std::endl;
                return {};
            }

            const uint32_t command = readWord(payload, 0u);
            uint32_t reply = 0u;
            const char *assumption = "unknown-command-zero-default";
            switch (command)
            {
            case 0x01u: // PAD_RPCCMD_OPEN_NEW
                reply = 1u;
                assumption = "PAD_OPEN-success-disconnected-controller-state";
                {
                    // The game passes a 256-byte double-buffered pad_data_new
                    // region in padOpenArgs.padArea (word 4). It polls each
                    // record's state byte at 0x70/0xf0 and explicitly treats
                    // FINDCTP1 (2) as a nonfatal no-controller result during
                    // startup. Publish that disconnected state in both
                    // buffers so the guest's own padGetState path can proceed.
                    const uint32_t padArea = readWord(payload, 4u * sizeof(uint32_t));
                    const uint64_t padAreaEnd = static_cast<uint64_t>(padArea) + 0xFFu;
                    uint8_t *const padData = padArea != 0u && padAreaEnd <= UINT32_MAX
                                                 ? getMemPtr(rdram, padArea)
                                                 : nullptr;
                    const uint8_t *const padDataLast = padArea != 0u && padAreaEnd <= UINT32_MAX
                                                           ? getConstMemPtr(rdram, static_cast<uint32_t>(padAreaEnd))
                                                           : nullptr;
                    if (!padData || !padDataLast || padDataLast != padData + 0xFFu)
                    {
                        std::cerr << "[Armageddon HLE PAD] rejected-open-area"
                                  << " address=0x" << std::hex << padArea
                                  << " bytes=0x100" << std::dec << std::endl;
                        return {};
                    }
                    PadAreaNew &area = *reinterpret_cast<PadAreaNew *>(padData);
                    populatePadBuffer(area.buffers[0], 1u, 1u, PAD_STATE_STABLE, PAD_RSTAT_COMPLETE);
                    populatePadBuffer(area.buffers[1], 0u, 0u, PAD_STATE_STABLE, PAD_RSTAT_COMPLETE);

                    std::cout << "[Armageddon HLE PAD state] open_area=0x" << std::hex << padArea
                              << " buffer0_state=STABLE buffer1_state=STABLE"
                              << " request_state=COMPLETE frame0=1 frame1=0 len=32" << std::dec << std::endl;
                }
                break;
            case 0x10u: // PAD_RPCCMD_INIT
                reply = 1u;
                assumption = "PAD_INIT-success";
                break;
            case 0x12u: // PAD_RPCCMD_GET_MODVER
                reply = 0x0422u;
                assumption = "SDK-3.1.0-compatible-PADMAN-version";
                break;
            case 0x0Cu: // PAD_RPCCMD_GET_PORTMAX_NEW
                reply = 2u;
                assumption = "two-EE-pad-ports";
                break;
            case 0x0Du: // PAD_RPCCMD_GET_SLOTMAX_NEW
                reply = 1u;
                assumption = "one-pad-slot-per-port";
                break;
            case 0x0Eu: // PAD_RPCCMD_CLOSE_NEW
                reply = 1u;
                assumption = "PAD_CLOSE-success";
                break;
            default:
                std::cerr << "[Armageddon HLE PAD] unimplemented-command=0x" << std::hex
                          << command << " action=fail-stop-no-synthetic-reply" << std::dec << std::endl;
                return {};
            }

            ps2x::iop::RpcRequest resultField = request;
            resultField.receive.address += 3u * sizeof(uint32_t);
            resultField.receive.size -= 3u * sizeof(uint32_t);
            if (!writeRpcReceive(rdram, resultField, &reply, sizeof(reply)))
                return {};
            std::cout << "[Armageddon HLE PAD] sid=0x" << std::hex << request.sid
                      << " rpc=0x" << request.function << " command=0x" << command
                      << " result=" << std::dec << reply
                      << " assumption=" << assumption << std::endl;
            return finishHostRpc(request);
        }

        // The trace reaches sceCdMmode during startup: CDVDFSV S-command
        // 0x22 receives one media-mode word and returns a 32-bit status.
        // Use the API's success convention for valid CD/DVD modes, and report
        // the inserted image as a PS2 DVD for the corresponding S-command.
        if (request.sid == 0x80000593u && request.receive.size >= sizeof(uint32_t) &&
            request.receive.address != 0u)
        {
            uint32_t value = 0u;
            uint32_t inputMedia = 0u;
            bool handledSpecific = false;
            if (request.function == 0x22u && request.send.size >= sizeof(uint32_t) &&
                runtime->isIopMemoryRange(request.send.address, sizeof(uint32_t)) &&
                runtime->readIopMemory(request.send.address, &inputMedia, sizeof(inputMedia)))
            {
                value = (inputMedia == 1u || inputMedia == 2u) ? 1u : 0u;
                handledSpecific = true;
            }
            else if (request.function == 0x03u && request.send.size == 0u)
            {
                value = 0x14u; // SCECdPS2DVD, matching the mounted DVD image.
                handledSpecific = true;
            }

            if (handledSpecific)
            {
                uint8_t *const output = getMemPtr(rdram, request.receive.address);
                std::memcpy(output, &value, sizeof(value));
                std::cout << "[Armageddon HLE CDVDFSV] rpc=0x" << std::hex << request.function
                          << " input=0x" << (request.function == 0x22u ? inputMedia : 0u)
                          << " output=0x" << value
                          << " assumption=" << (request.function == 0x03u ? "mounted-PS2-DVD" : "valid-media-mode")
                          << std::dec << std::endl;
                return finishHostRpc(request);
            }
        }

        std::cerr << "[Armageddon HLE RPC unimplemented] sid=0x" << std::hex
                  << request.sid << " rpc=0x" << request.function
                  << " action=fail-stop-no-synthetic-reply" << std::dec << std::endl;
        return result;
    }
}
