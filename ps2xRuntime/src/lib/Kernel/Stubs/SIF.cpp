#include "Common.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"
#include "../../armageddon_hle.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;

        void resetSifCommandStateLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x1u:
            case 0x2u:
            case 0x3u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                resetSifCommandStateLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        resetSifCommandStateLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    namespace
    {
        constexpr uint32_t kSifCmdRpcEnd = 0x80000008u;
        constexpr uint32_t kSifCmdRpcBind = 0x80000009u;
        constexpr uint32_t kSifCmdRpcCall = 0x8000000Au;

        struct ArmageddonHleRpcServer
        {
            uint32_t serverAddress = 0u;
            uint32_t bufferAddress = 0u;
            uint32_t callbackBufferAddress = 0u;
        };

        struct ArmageddonHleRpcRegistry
        {
            std::mutex mutex;
            std::unordered_map<PS2Runtime *, std::unordered_map<uint32_t, ArmageddonHleRpcServer>> serversBySid;
            std::unordered_map<PS2Runtime *, std::unordered_map<uint32_t, uint32_t>> sidByClient;
            std::unordered_map<PS2Runtime *, std::unordered_map<uint32_t, uint32_t>> sidByServer;
        };

        ArmageddonHleRpcRegistry g_armageddonHleRpcRegistry;

        bool readGuestWord(const uint8_t *rdram, uint32_t address, uint32_t &value)
        {
            const uint8_t *const word = getConstMemPtr(rdram, address);
            if (!word)
                return false;
            std::memcpy(&value, word, sizeof(value));
            return true;
        }

        bool writeGuestWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            uint8_t *const word = getMemPtr(rdram, address);
            if (!word)
                return false;
            std::memcpy(word, &value, sizeof(value));
            return true;
        }

        const char *armageddonHleServiceName(uint32_t sid)
        {
            switch (sid)
            {
            case 0x80000001u: return "SceStdio-FILEIO-HLE";
            case 0x80000003u: return "FILEIO";
            case 0x80000592u: return "CDVDFSV";
            case 0x80000593u: return "CDVDFSV-SCMD";
            case 0x8000059Cu: return "CDVDFSV-DiskReady";
            case 0x80000006u: return "LOADCORE";
            case 0x80000100u: return "PADMAN";
            case 0x80000101u: return "PADMAN-extension";
            case 0x80000400u:
            case 0x80000480u: return "memory-card";
            case 0x80000701u: return "sound";
            default: return "unknown-generic-HLE";
            }
        }

        bool getOrCreateArmageddonHleRpcServer(PS2Runtime *runtime,
                                                uint32_t sid,
                                                ArmageddonHleRpcServer &server)
        {
            std::lock_guard<std::mutex> lock(g_armageddonHleRpcRegistry.mutex);
            auto &forRuntime = g_armageddonHleRpcRegistry.serversBySid[runtime];
            const auto existing = forRuntime.find(sid);
            if (existing != forRuntime.end())
            {
                server = existing->second;
                return true;
            }

            // These are real IOP-RAM allocations used only as opaque host-server
            // addresses in the EE RPC data structures. No guest IOP code is run.
            ArmageddonHleRpcServer created{};
            created.serverAddress = runtime->allocateIopMemory(0x50u, 64u);
            created.bufferAddress = runtime->allocateIopMemory(0x1000u, 64u);
            created.callbackBufferAddress = runtime->allocateIopMemory(0x100u, 64u);
            if (created.serverAddress == 0u || created.bufferAddress == 0u ||
                created.callbackBufferAddress == 0u)
            {
                if (created.callbackBufferAddress != 0u)
                    (void)runtime->freeIopMemory(created.callbackBufferAddress);
                if (created.bufferAddress != 0u)
                    (void)runtime->freeIopMemory(created.bufferAddress);
                if (created.serverAddress != 0u)
                    (void)runtime->freeIopMemory(created.serverAddress);
                return false;
            }

            std::array<uint32_t, 0x50u / sizeof(uint32_t)> descriptor{};
            descriptor[0] = sid; // SifRpcServerData_t::sid
            descriptor[2] = created.bufferAddress; // buf
            descriptor[3] = 0x1000u; // size
            descriptor[5] = created.callbackBufferAddress; // cbuf
            descriptor[6] = 0x100u; // size2
            if (!runtime->writeIopMemory(created.serverAddress,
                                         descriptor.data(),
                                         sizeof(descriptor)))
            {
                (void)runtime->freeIopMemory(created.callbackBufferAddress);
                (void)runtime->freeIopMemory(created.bufferAddress);
                (void)runtime->freeIopMemory(created.serverAddress);
                return false;
            }

            server = created;
            forRuntime.emplace(sid, created);
            g_armageddonHleRpcRegistry.sidByServer[runtime][created.serverAddress] = sid;
            return true;
        }

        bool getArmageddonHleRpcServer(PS2Runtime *runtime,
                                       uint32_t serverAddress,
                                       uint32_t &sid,
                                       ArmageddonHleRpcServer &server)
        {
            std::lock_guard<std::mutex> lock(g_armageddonHleRpcRegistry.mutex);
            const auto runtimeServers = g_armageddonHleRpcRegistry.serversBySid.find(runtime);
            const auto runtimeSids = g_armageddonHleRpcRegistry.sidByServer.find(runtime);
            if (runtimeServers == g_armageddonHleRpcRegistry.serversBySid.end() ||
                runtimeSids == g_armageddonHleRpcRegistry.sidByServer.end())
                return false;
            const auto sidEntry = runtimeSids->second.find(serverAddress);
            if (sidEntry == runtimeSids->second.end())
                return false;
            const auto serverEntry = runtimeServers->second.find(sidEntry->second);
            if (serverEntry == runtimeServers->second.end())
                return false;
            sid = sidEntry->second;
            server = serverEntry->second;
            return true;
        }

        void rememberArmageddonHleRpcClient(PS2Runtime *runtime, uint32_t clientAddress, uint32_t sid)
        {
            std::lock_guard<std::mutex> lock(g_armageddonHleRpcRegistry.mutex);
            g_armageddonHleRpcRegistry.sidByClient[runtime][clientAddress] = sid;
        }

        uint32_t getArmageddonHleRpcClientSid(PS2Runtime *runtime, uint32_t clientAddress)
        {
            std::lock_guard<std::mutex> lock(g_armageddonHleRpcRegistry.mutex);
            const auto runtimeClients = g_armageddonHleRpcRegistry.sidByClient.find(runtime);
            if (runtimeClients == g_armageddonHleRpcRegistry.sidByClient.end())
                return 0u;
            const auto client = runtimeClients->second.find(clientAddress);
            return client == runtimeClients->second.end() ? 0u : client->second;
        }

        bool completeArmageddonHleRpcBind(uint8_t *rdram,
                                          PS2Runtime *runtime,
                                          uint32_t packetAddress,
                                          uint32_t packetSize,
                                          uint32_t sourcePc,
                                          uint32_t sourceRa)
        {
            if (!armageddon_hle::enabled(runtime) || packetSize < 36u || packetSize > 112u)
                return false;

            std::array<uint8_t, 112u> request{};
            if (!readEeRange(rdram, packetAddress, request.data(), packetSize))
                return false;
            uint32_t commandId = 0u;
            std::memcpy(&commandId, request.data() + 8u, sizeof(commandId));
            if (commandId != kSifCmdRpcBind)
                return false;

            uint32_t sid = 0u;
            uint32_t clientAddress = 0u;
            std::memcpy(&clientAddress, request.data() + 28u, sizeof(clientAddress));
            std::memcpy(&sid, request.data() + 32u, sizeof(sid));
            uint32_t semaWord = 0xFFFFFFFFu;
            if (clientAddress == 0u || !readGuestWord(rdram, clientAddress + 8u, semaWord))
                return false;
            uint32_t endFunction = 0u;
            uint32_t endParameter = 0u;
            (void)readGuestWord(rdram, clientAddress + 0x1Cu, endFunction);
            (void)readGuestWord(rdram, clientAddress + 0x20u, endParameter);

            ArmageddonHleRpcServer server{};
            if (!getOrCreateArmageddonHleRpcServer(runtime, sid, server))
            {
                std::cerr << "[Armageddon HLE RPC bind] sid=0x" << std::hex << sid
                          << " service=" << armageddonHleServiceName(sid)
                          << " result=host-server-allocation-failed\n" << std::dec;
                return false;
            }
            rememberArmageddonHleRpcClient(runtime, clientAddress, sid);

            std::array<uint8_t, 64u> reply{};
            std::memcpy(reply.data(), request.data(), 4u); // preserve the packet size word
            const uint32_t replyCommand = kSifCmdRpcEnd;
            std::memcpy(reply.data() + 8u, &replyCommand, sizeof(replyCommand));
            std::memcpy(reply.data() + 16u, request.data() + 16u, 16u); // rec_id, pkt_addr, rpc_id, client
            const uint32_t originalCommand = kSifCmdRpcBind;
            std::memcpy(reply.data() + 32u, &originalCommand, sizeof(originalCommand));
            std::memcpy(reply.data() + 36u, &server.serverAddress, sizeof(server.serverAddress));
            std::memcpy(reply.data() + 40u, &server.bufferAddress, sizeof(server.bufferAddress));
            std::memcpy(reply.data() + 44u, &server.callbackBufferAddress, sizeof(server.callbackBufferAddress));

            const bool guestCallbackQueued = dispatchSifCommand(rdram,
                                                                runtime,
                                                                kSifCmdRpcEnd,
                                                                reply.data(),
                                                                reply.size());
            bool directCompletion = false;
            if (!guestCallbackQueued)
            {
                // Same client fields and semaphore handoff as the SDK's
                // _request_end(RPC_BIND) callback, with the shortcut explicit.
                const bool clientUpdated =
                    writeGuestWord(rdram, clientAddress + 0x14u, server.bufferAddress) &&
                    writeGuestWord(rdram, clientAddress + 0x18u, server.callbackBufferAddress) &&
                    writeGuestWord(rdram, clientAddress + 0x24u, server.serverAddress);
                if (!clientUpdated)
                    return false;
                if (static_cast<int32_t>(semaWord) >= 0)
                    directCompletion = runtime->eeScheduler().signalSemaphore(static_cast<int32_t>(semaWord), true) >= 0;
            }

            std::cout << "[Armageddon HLE RPC bind] sid=0x" << std::hex << sid
                      << " source_pc=0x" << sourcePc << " source_ra=0x" << sourceRa
                      << " service=" << armageddonHleServiceName(sid)
                      << " client=0x" << clientAddress
                      << " sema=" << static_cast<int32_t>(semaWord)
                      << " end_function=0x" << endFunction
                      << " end_parameter=0x" << endParameter
                      << " server=0x" << server.serverAddress
                      << " buffer=0x" << server.bufferAddress
                      << " callback_buffer=0x" << server.callbackBufferAddress
                      << " reply_size=0x40 reply=RPC_END route=" << (guestCallbackQueued ? "guest-command-handler-8"
                                                        : (directCompletion ? "documented-host-completion-fallback" : "host-completion-failed"))
                      << (std::strcmp(armageddonHleServiceName(sid), "unknown-generic-HLE") == 0
                              ? " default=unimplemented-until-request"
                              : "")
                      << std::dec << std::endl;
            return guestCallbackQueued || directCompletion || static_cast<int32_t>(semaWord) < 0;
        }

        bool completeArmageddonHleRpcCall(uint8_t *rdram,
                                          PS2Runtime *runtime,
                                          R5900Context *ctx,
                                          uint32_t packetAddress,
                                          uint32_t packetSize,
                                          const Ps2SifDmaTransfer *transfers,
                                          uint32_t transferCount,
                                          uint32_t sourcePc,
                                          uint32_t sourceRa)
        {
            if (!armageddon_hle::enabled(runtime) || packetSize < 56u || packetSize > 112u)
                return false;

            std::array<uint8_t, 112u> requestBytes{};
            if (!readEeRange(rdram, packetAddress, requestBytes.data(), packetSize))
                return false;
            uint32_t commandId = 0u;
            std::memcpy(&commandId, requestBytes.data() + 8u, sizeof(commandId));
            if (commandId != kSifCmdRpcCall)
                return false;

            auto readPacketWord = [&](size_t offset, uint32_t &value)
            {
                if (offset + sizeof(value) > packetSize)
                    return false;
                std::memcpy(&value, requestBytes.data() + offset, sizeof(value));
                return true;
            };

            uint32_t clientAddress = 0u, rpcNumber = 0u, sendSize = 0u;
            uint32_t receiveAddress = 0u, receiveSize = 0u, mode = 0u, serverAddress = 0u;
            if (!readPacketWord(28u, clientAddress) || !readPacketWord(32u, rpcNumber) ||
                !readPacketWord(36u, sendSize) || !readPacketWord(40u, receiveAddress) ||
                !readPacketWord(44u, receiveSize) || !readPacketWord(48u, mode) ||
                !readPacketWord(52u, serverAddress))
                return false;

            uint32_t sid = 0u;
            ArmageddonHleRpcServer server{};
            if (!getArmageddonHleRpcServer(runtime, serverAddress, sid, server))
                sid = getArmageddonHleRpcClientSid(runtime, clientAddress);
            if (sid == 0u)
            {
                std::cerr << "[Armageddon HLE RPC call] unresolved server"
                          << " client=0x" << std::hex << clientAddress
                          << " server=0x" << serverAddress << std::dec << std::endl;
                return false;
            }
            if (server.serverAddress == 0u)
            {
                uint32_t ignoredSid = 0u;
                if (!getArmageddonHleRpcServer(runtime, serverAddress, ignoredSid, server))
                    return false;
            }

            uint32_t packetSourceAddress = 0u;
            std::memcpy(&packetSourceAddress, requestBytes.data() + 20u, sizeof(packetSourceAddress));
            uint32_t sendAddress = server.bufferAddress;
            if (sendSize != 0u)
            {
                bool foundPayload = false;
                for (uint32_t i = 0u; i < transferCount; ++i)
                {
                    const Ps2SifDmaTransfer &transfer = transfers[i];
                    if (transfer.dest == server.bufferAddress && transfer.size >= 0 &&
                        static_cast<uint32_t>(transfer.size) >= sendSize)
                    {
                        sendAddress = transfer.dest;
                        foundPayload = true;
                        break;
                    }
                }
                if (!foundPayload || !runtime->isIopMemoryRange(sendAddress, sendSize))
                {
                    std::cerr << "[Armageddon HLE RPC call] missing-send-transfer sid=0x"
                              << std::hex << sid << " rpc=0x" << rpcNumber
                              << " size=0x" << sendSize << " server_buffer=0x" << server.bufferAddress
                              << std::dec << std::endl;
                    return false;
                }
            }

            ps2x::iop::RpcRequest rpcRequest{};
            rpcRequest.clientAddress = clientAddress;
            rpcRequest.serverAddress = serverAddress;
            rpcRequest.serverFunction = 0u;
            rpcRequest.serverBuffer = server.bufferAddress;
            rpcRequest.sid = sid;
            rpcRequest.function = rpcNumber;
            rpcRequest.mode = mode;
            rpcRequest.send = {sendAddress, sendSize};
            rpcRequest.receive = {receiveAddress, receiveSize};

            uint32_t semaWord = 0xFFFFFFFFu;
            uint32_t endFunction = 0u;
            uint32_t endParameter = 0u;
            (void)readGuestWord(rdram, clientAddress + 8u, semaWord);
            if (semaWord == 0xFFFFFFFFu)
            {
                (void)readGuestWord(rdram, clientAddress + 0x28u, semaWord);
            }
            (void)readGuestWord(rdram, clientAddress + 0x1Cu, endFunction);
            (void)readGuestWord(rdram, clientAddress + 0x20u, endParameter);
            std::cout << "[Armageddon HLE RPC request] source_pc=0x" << std::hex << sourcePc
                      << " source_ra=0x" << sourceRa << " sid=0x" << sid
                      << " rpc=0x" << rpcNumber << " send=0x" << sendAddress << "/0x" << sendSize
                      << " reply=0x" << receiveAddress << "/0x" << receiveSize
                      << " reply_size=0x" << receiveSize
                      << " client=0x" << clientAddress
                      << " client_sema=0x" << semaWord
                      << " end_function=0x" << endFunction
                      << " end_parameter=0x" << endParameter
                      << std::dec << std::endl;

            const R5900Context callerCtx = ctx ? *ctx : R5900Context{};
            auto completeRpc = [runtime, rdram, rpcRequest, clientAddress, packetAddress,
                                requestBytes, packetSize, sid, rpcNumber, sourcePc, sourceRa,
                                serverAddress, server, semaWord, endFunction, endParameter,
                                sendAddress, sendSize, receiveAddress, receiveSize,
                                packetSourceAddress, callerCtx]() mutable
            {
                const ps2x::iop::RpcResult result = armageddon_hle::handleRpc(runtime, rdram, rpcRequest);
                if (!result.handled)
                {
                    std::cerr << "[Armageddon HLE RPC call] no-host-handler sid=0x"
                              << std::hex << sid << " rpc=0x" << rpcNumber
                              << " reply_size=0x" << receiveSize
                              << " action=fail-stop-no-synthetic-reply" << std::dec << std::endl;
                    runtime->requestStop();
                    return;
                }

                if (!readGuestWord(rdram, clientAddress + 8u, semaWord) ||
                    !readGuestWord(rdram, clientAddress + 0x1Cu, endFunction))
                {
                    return;
                }
                if (semaWord == 0xFFFFFFFFu)
                {
                    (void)readGuestWord(rdram, clientAddress + 0x28u, semaWord);
                }

                std::array<uint8_t, 64u> reply{};
                std::memcpy(reply.data(), requestBytes.data(), 4u);
                const uint32_t replyCommand = kSifCmdRpcEnd;
                std::memcpy(reply.data() + 8u, &replyCommand, sizeof(replyCommand));
                std::memcpy(reply.data() + 16u, requestBytes.data() + 16u, 16u);
                const uint32_t originalCommand = kSifCmdRpcCall;
                std::memcpy(reply.data() + 32u, &originalCommand, sizeof(originalCommand));
                std::memcpy(reply.data() + 36u, &serverAddress, sizeof(serverAddress));
                std::memcpy(reply.data() + 40u, &server.bufferAddress, sizeof(server.bufferAddress));
                std::memcpy(reply.data() + 44u, &server.callbackBufferAddress, sizeof(server.callbackBufferAddress));

                const bool guestCallbackQueued = dispatchSifCommand(rdram,
                                                                    runtime,
                                                                    kSifCmdRpcEnd,
                                                                    reply.data(),
                                                                    reply.size());
                bool directCompletion = false;
                if (!guestCallbackQueued)
                {
                    if (endFunction != 0u)
                    {
                        uint32_t callbackFunction = endFunction;
                        if (!runtime->hasFunction(callbackFunction) && callbackFunction >= 0x10000u &&
                            runtime->hasFunction(callbackFunction - 0x10000u))
                        {
                            callbackFunction -= 0x10000u;
                        }
                        if (!runtime->hasFunction(callbackFunction))
                        {
                            std::cerr << "[Armageddon HLE RPC call] end-callback-unresolved"
                                      << " guest_function=0x" << std::hex << endFunction
                                      << " client=0x" << clientAddress
                                      << " action=fail-stop" << std::dec << std::endl;
                            runtime->requestStop();
                            return;
                        }

                        uint32_t recordId = 0u;
                        std::memcpy(&recordId, requestBytes.data() + 16u, sizeof(recordId));
                        GuestInvocation callback{};
                        callback.kind = GuestInvocationKind::RpcCallback;
                        callback.context = callerCtx;
                        callback.context.pc = callbackFunction;
                        SET_GPR_U32(&callback.context, 4, endParameter);
                        SET_GPR_U32(&callback.context, 29, runtime->eeScheduler().invocationStackTop());
                        SET_GPR_U32(&callback.context, 31, 0u);
                        callback.onComplete = [runtime, rdram, clientAddress, packetAddress,
                                               recordId, semaWord, sid, rpcNumber](const R5900Context &, R5900Context &)
                        {
                            const bool clientCleared = writeGuestWord(rdram, clientAddress, 0u);
                            const bool packetReleased = writeGuestWord(rdram, packetAddress + 16u, recordId & ~1u);
                            bool semaphoreSignaled = false;
                            if (static_cast<int32_t>(semaWord) >= 0)
                            {
                                semaphoreSignaled = runtime->eeScheduler().signalSemaphore(
                                    static_cast<int32_t>(semaWord), true) >= 0;
                            }
                            std::cout << "[Armageddon HLE RPC callback completion] sid=0x" << std::hex << sid
                                      << " rpc=0x" << rpcNumber << " client=0x" << clientAddress
                                      << " client_cleared=" << (clientCleared ? "true" : "false")
                                      << " packet_released=" << (packetReleased ? "true" : "false")
                                      << " semaphore_signaled=" << (semaphoreSignaled ? "true" : "false")
                                      << std::dec << std::endl;
                        };
                        try
                        {
                            runtime->eeScheduler().queueInvocation(std::move(callback));
                        }
                        catch (...)
                        {
                            std::cerr << "[Armageddon HLE RPC call] end-callback-queue-failed"
                                      << " guest_function=0x" << std::hex << callbackFunction
                                      << " sid=0x" << sid << " rpc=0x" << rpcNumber
                                      << " action=fail-stop" << std::dec << std::endl;
                            runtime->requestStop();
                            return;
                        }
                    }
                    else
                    {
                        (void)writeGuestWord(rdram, clientAddress, 0u); // _request_end clears hdr.pkt_addr
                        uint32_t recordId = 0u;
                        std::memcpy(&recordId, requestBytes.data() + 16u, sizeof(recordId));
                        (void)writeGuestWord(rdram, packetAddress + 16u, recordId & ~1u); // release RPC packet
                        if (static_cast<int32_t>(semaWord) >= 0)
                            directCompletion = runtime->eeScheduler().signalSemaphore(static_cast<int32_t>(semaWord), true) >= 0;
                    }
                }

                std::cout << "[Armageddon HLE RPC call completion] sid=0x" << std::hex << sid
                          << " source_pc=0x" << sourcePc << " source_ra=0x" << sourceRa
                          << " rpc=0x" << rpcNumber << " client=0x" << clientAddress
                          << " server=0x" << serverAddress << " send=0x" << sendAddress
                          << "/0x" << sendSize << " recv=0x" << receiveAddress << "/0x" << receiveSize
                          << " reply_size=0x" << receiveSize << " reply_packet_size=0x40"
                          << " client_sema=0x" << semaWord
                          << " direct_completion=" << (directCompletion ? "true" : "false")
                          << " result=" << (result.handled ? "handled" : "unhandled")
                          << " reply=RPC_END route=" << (guestCallbackQueued ? "guest-command-handler-8"
                                                            : (endFunction != 0u ? "guest-end-callback-queued"
                                                            : (directCompletion ? "documented-host-completion-fallback" : "host-completion-failed")))
                          << " end_function=0x" << endFunction
                          << " packet_source=0x" << packetSourceAddress
                          << std::dec << std::endl;
            };

            if ((mode & 1u) != 0u)
            {
                constexpr uint64_t kDefaultRpcDeferCycles = 4960000ull;
                runtime->eeScheduler().queueDeferredAction(std::move(completeRpc), kDefaultRpcDeferCycles);
                return true;
            }
            else
            {
                completeRpc();
                return true;
            }

        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        resetSifCommandStateLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        if (armageddon_hle::readSifRegister(runtime, reg, value))
        {
            if (reg == 4u)
            {
                constexpr uint32_t kArmageddonSreg0Address = 0x006505C0u;
                const uint32_t assumedSreg0 = 1u;
                if (uint8_t *const sreg0 = getMemPtr(rdram, kArmageddonSreg0Address))
                {
                    std::memcpy(sreg0, &assumedSreg0, sizeof(assumedSreg0));
                    std::cout << "[Armageddon HLE assumption] publish SREG[0]=1"
                              << " guest_address=0x006505C0 source=host" << std::endl;
                }
                else
                {
                    std::cerr << "[Armageddon HLE error] cannot publish SREG[0]"
                              << " guest_address=0x006505C0" << std::endl;
                }
            }
            setReturnU32(ctx, value);
            return;
        }
        bool shouldLog = false;
        if (reg >= 1u && reg <= 4u)
        {
            if (!PS2IopTransport::readSifRegister(runtime, reg, value))
            {
                std::cerr << "[sceSifGetReg] IOP SIF register source unavailable for reg="
                          << reg << std::endl;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        if (armageddon_hle::readSifSreg(runtime, rdram, reg, value))
        {
            setReturnU32(ctx, value);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (armageddon_hle::skipIopReset(runtime, ctx, "sceSifRebootIop"))
        {
            setReturnS32(ctx, 1);
            return;
        }
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (armageddon_hle::skipIopReset(runtime, ctx, "sceSifResetIop"))
        {
            setReturnS32(ctx, 1);
            return;
        }
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
                if (std::getenv("PS2X_SIF_TRACE_DATA"))
                {
                    uint32_t commandId = 0u;
                    if (size >= 12u)
                    {
                        const uint8_t *commandBytes = getConstMemPtr(rdram, src + 8u);
                        if (commandBytes)
                            std::memcpy(&commandId, commandBytes, sizeof(commandId));
                    }
                    const bool isResetCommand = commandId == 0x80000003u && size >= 0x18u;
                    std::cerr << (isResetCommand ? "[sceSifSetDma:RESET_PACKET] i=" : "[sceSifSetDma:DATA] i=")
                              << i << " src=0x" << std::hex << src << " size=0x" << size << " bytes=";
                    const uint32_t previewSize = std::min<uint32_t>(size, isResetCommand ? 0x68u : 32u);
                    for (uint32_t byteIndex = 0u; byteIndex < previewSize; ++byteIndex)
                    {
                        const uint8_t *byte = getConstMemPtr(rdram, src + byteIndex);
                        if (!byte)
                        {
                            std::cerr << "??";
                            break;
                        }
                        const unsigned value = *byte;
                        if (value < 16u)
                            std::cerr << '0';
                        std::cerr << value;
                    }
                    std::cerr << std::dec << std::endl;
                }
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        if (armageddon_hle::enabled(runtime))
        {
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                (void)completeArmageddonHleRpcBind(rdram,
                                                   runtime,
                                                   xfer.src,
                                                   static_cast<uint32_t>(xfer.size),
                                                   ctx ? ctx->pc : 0u,
                                                   ctx ? getRegU32(ctx, 31) : 0u);
                (void)completeArmageddonHleRpcCall(rdram,
                                                   runtime,
                                                   ctx,
                                                   xfer.src,
                                                   static_cast<uint32_t>(xfer.size),
                                                   pending.data(),
                                                   pendingCount,
                                                   ctx ? ctx->pc : 0u,
                                                   ctx ? getRegU32(ctx, 31) : 0u);
            }
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        if (reg >= 1u && reg <= 4u)
        {
            if (!PS2IopTransport::readSifRegister(runtime, reg, prev) ||
                !PS2IopTransport::writeSifRegisterFromEe(runtime, reg, value))
            {
                std::cerr << "[sceSifSetReg] IOP SIF register write failed for reg="
                          << reg << std::endl;
                setReturnS32(ctx, -1);
                return;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
        }
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
