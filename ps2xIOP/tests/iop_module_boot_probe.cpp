// Explicit private-input diagnostic. Never registered as a normal CTest.
#include "iop_compat_test_support.h"
#include "emulator/iop_emulator.h"
#include <fstream>
#include <iomanip>
#include <vector>

int main(int argc, char **argv)
{
    try {
        if (argc < 2) throw std::runtime_error("Expected ordered original IRX files");
        iop_test::Host host(32u * 1024u * 1024u);
        ps2x::iop::detail::IopEmulator iop(host);
        bool scheduleNextModuleStart = false;
        std::vector<int32_t> scheduledModules;
        std::vector<ps2x::iop::IopScheduledCall> scheduledCalls;
        for (int n = 1; n < argc; ++n) {
            if (std::string_view(argv[n]) == "--cd-root") {
                if (++n >= argc)
                    throw std::runtime_error("--cd-root requires an extracted disc directory");
                host.cdRoot = argv[n];
                std::cout << "{\"cd_root\":" << std::quoted(host.cdRoot) << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--scheduled-start") {
                scheduleNextModuleStart = true;
                continue;
            }
            if (std::string_view(argv[n]) == "--module-start-argv") {
                if (n + 2 >= argc)
                    throw std::runtime_error("--module-start-argv requires an IRX path and at least one argv string");
                const std::string imagePath = argv[++n];
                std::vector<uint32_t> guestArgv;
                while (n + 1 < argc && !std::string_view(argv[n + 1]).starts_with("--")) {
                    const std::string arg = argv[++n];
                    const uint32_t address = iop.allocateMemory(static_cast<uint32_t>(arg.size() + 1u), 4u);
                    std::vector<char> bytes(arg.begin(), arg.end());
                    bytes.push_back('\0');
                    if (address == 0u || !iop.writeMemory(address, bytes.data(), bytes.size()))
                        throw std::runtime_error("Could not allocate/write IOP module argv string");
                    guestArgv.push_back(address);
                }
                if (guestArgv.empty())
                    throw std::runtime_error("--module-start-argv requires at least one argv string");
                const auto result = iop.loadModuleScheduledWithArgc(
                    imagePath, guestArgv.data(), static_cast<uint32_t>(guestArgv.size() * sizeof(guestArgv[0])),
                    static_cast<uint32_t>(guestArgv.size()));
                if (result.startPending)
                    scheduledModules.push_back(result.moduleId);
                std::cout << "{\"module_argv\":" << std::quoted(imagePath)
                          << ",\"argc\":" << guestArgv.size()
                          << ",\"argument_bytes\":" << guestArgv.size() * sizeof(guestArgv[0])
                          << ",\"id\":" << result.moduleId
                          << ",\"start_result\":" << result.startResult
                          << ",\"start_pending\":" << result.startPending
                          << ",\"iop_instructions\":" << iop.instructions() << "}\n";
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                continue;
            }
            if (std::string_view(argv[n]) == "--romdir-start") {
                if (n + 2 >= argc)
                    throw std::runtime_error("--romdir-start requires a module name and image path");
                const std::string moduleName = argv[++n];
                const std::string imagePath = argv[++n];
                std::ifstream imageFile(imagePath, std::ios::binary | std::ios::ate);
                if (!imageFile || imageFile.tellg() <= 0 || imageFile.tellg() > 8 * 1024 * 1024)
                    throw std::runtime_error("Invalid ROMDIR image file");
                host.file.resize(static_cast<size_t>(imageFile.tellg()));
                imageFile.seekg(0);
                imageFile.read(reinterpret_cast<char *>(host.file.data()), static_cast<std::streamsize>(host.file.size()));
                if (!imageFile)
                    throw std::runtime_error("Truncated ROMDIR image read");
                const auto result = iop.loadRomDirModuleScheduled(imagePath, moduleName, nullptr, 0u);
                if (result.startPending)
                    scheduledModules.push_back(result.moduleId);
                std::cout << "{\"romdir_module\":" << std::quoted(moduleName)
                          << ",\"image\":" << std::quoted(imagePath)
                          << ",\"id\":" << result.moduleId
                          << ",\"start_result\":" << result.startResult
                          << ",\"start_pending\":" << result.startPending
                          << ",\"iop_instructions\":" << iop.instructions() << "}\n";
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                continue;
            }
            if (std::string_view(argv[n]) == "--romdir-start-args") {
                if (n + 3 >= argc)
                    throw std::runtime_error("--romdir-start-args requires a module name, image path, and one argument string");
                const std::string moduleName = argv[++n];
                const std::string imagePath = argv[++n];
                const std::string moduleArguments = argv[++n];
                std::ifstream imageFile(imagePath, std::ios::binary | std::ios::ate);
                if (!imageFile || imageFile.tellg() <= 0 || imageFile.tellg() > 8 * 1024 * 1024)
                    throw std::runtime_error("Invalid ROMDIR image file");
                host.file.resize(static_cast<size_t>(imageFile.tellg()));
                imageFile.seekg(0);
                imageFile.read(reinterpret_cast<char *>(host.file.data()), static_cast<std::streamsize>(host.file.size()));
                if (!imageFile)
                    throw std::runtime_error("Truncated ROMDIR image read");
                const auto result = iop.loadRomDirModuleScheduled(imagePath, moduleName,
                                                                  moduleArguments.data(),
                                                                  static_cast<uint32_t>(moduleArguments.size()));
                if (result.startPending)
                    scheduledModules.push_back(result.moduleId);
                std::cout << "{\"romdir_module\":" << std::quoted(moduleName)
                          << ",\"image\":" << std::quoted(imagePath)
                          << ",\"argument\":" << std::quoted(moduleArguments)
                          << ",\"argument_size\":" << moduleArguments.size()
                          << ",\"id\":" << result.moduleId
                          << ",\"start_result\":" << result.startResult
                          << ",\"start_pending\":" << result.startPending
                          << ",\"iop_instructions\":" << iop.instructions() << "}\n";
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                continue;
            }
            if (std::string_view(argv[n]) == "--romdir-start-argv") {
                if (n + 3 >= argc)
                    throw std::runtime_error("--romdir-start-argv requires a module name, image path, and argv strings");
                const std::string moduleName = argv[++n];
                const std::string imagePath = argv[++n];
                std::vector<uint32_t> guestArgv;
                while (n + 1 < argc && !std::string_view(argv[n + 1]).starts_with("--")) {
                    const std::string arg = argv[++n];
                    const uint32_t address = iop.allocateMemory(static_cast<uint32_t>(arg.size() + 1u), 4u);
                    std::vector<char> bytes(arg.begin(), arg.end());
                    bytes.push_back('\0');
                    if (address == 0u || !iop.writeMemory(address, bytes.data(), bytes.size()))
                        throw std::runtime_error("Could not allocate/write IOP module argv string");
                    guestArgv.push_back(address);
                }
                if (guestArgv.empty())
                    throw std::runtime_error("--romdir-start-argv requires at least one argv string");
                const auto result = iop.loadRomDirModuleScheduledWithArgc(
                    imagePath, moduleName, guestArgv.data(),
                    static_cast<uint32_t>(guestArgv.size() * sizeof(guestArgv[0])),
                    static_cast<uint32_t>(guestArgv.size()));
                if (result.startPending)
                    scheduledModules.push_back(result.moduleId);
                std::cout << "{\"romdir_module_argv\":" << std::quoted(moduleName)
                          << ",\"image\":" << std::quoted(imagePath)
                          << ",\"argc\":" << guestArgv.size()
                          << ",\"argument_bytes\":" << guestArgv.size() * sizeof(guestArgv[0])
                          << ",\"id\":" << result.moduleId
                          << ",\"start_result\":" << result.startResult
                          << ",\"start_pending\":" << result.startPending
                          << ",\"iop_instructions\":" << iop.instructions() << "}\n";
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                continue;
            }
            if (std::string_view(argv[n]) == "--replace-host-file") {
                if (++n >= argc)
                    throw std::runtime_error("--replace-host-file requires a file path");
                const std::string hostFilePath = argv[n];
                std::ifstream hostFile(hostFilePath, std::ios::binary | std::ios::ate);
                if (!hostFile || hostFile.tellg() <= 0 || hostFile.tellg() > 8 * 1024 * 1024)
                    throw std::runtime_error("Invalid replacement host file");
                host.file.resize(static_cast<size_t>(hostFile.tellg()));
                hostFile.seekg(0);
                hostFile.read(reinterpret_cast<char *>(host.file.data()), static_cast<std::streamsize>(host.file.size()));
                if (!hostFile)
                    throw std::runtime_error("Truncated replacement host file read");
                std::cout << "{\"host_file_replaced\":" << std::quoted(hostFilePath)
                          << ",\"size\":" << host.file.size() << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--map-host-file") {
                if (n + 2 >= argc)
                    throw std::runtime_error("--map-host-file requires a guest path and a file path");
                const std::string guestPath = argv[++n];
                const std::string hostFilePath = argv[++n];
                std::ifstream hostFile(hostFilePath, std::ios::binary | std::ios::ate);
                if (!hostFile || hostFile.tellg() <= 0 || hostFile.tellg() > 64 * 1024 * 1024)
                    throw std::runtime_error("Invalid mapped host file");
                std::vector<uint8_t> bytes(static_cast<size_t>(hostFile.tellg()));
                hostFile.seekg(0);
                hostFile.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!hostFile)
                    throw std::runtime_error("Truncated mapped host file read");
                host.hostFiles[guestPath] = std::move(bytes);
                std::cout << "{\"host_file_mapped\":" << std::quoted(guestPath)
                          << ",\"source\":" << std::quoted(hostFilePath)
                          << ",\"size\":" << host.hostFiles[guestPath].size() << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--call-open") {
                if (++n >= argc)
                    throw std::runtime_error("--call-open requires a guest path");
                const std::string guestPath = argv[n];
                const uint32_t pathAddress = iop.allocateMemory(static_cast<uint32_t>(guestPath.size() + 1u), 16u);
                std::vector<char> pathBytes(guestPath.begin(), guestPath.end());
                pathBytes.push_back('\0');
                if (pathAddress == 0u || !iop.writeMemory(pathAddress, pathBytes.data(), pathBytes.size()))
                    throw std::runtime_error("Could not allocate/write IOP open path");
                const auto opened = iop.callExport("ioman", 4u, pathAddress, 1u, 0u, 0u, 100000u);
                std::cout << "{\"ioman_open\":" << std::quoted(guestPath)
                          << ",\"found\":" << opened.found
                          << ",\"returned\":" << opened.returned
                          << ",\"result\":" << opened.value
                          << ",\"entry\":" << opened.entry
                          << ",\"stop_pc\":" << opened.stopPc
                          << ",\"instructions\":" << opened.instructions << "}\n";
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                continue;
            }
            if (std::string_view(argv[n]) == "--dump-memory") {
                if (n + 2 >= argc)
                    throw std::runtime_error("--dump-memory requires an address and byte count");
                const uint32_t address = static_cast<uint32_t>(std::stoul(argv[++n], nullptr, 0));
                const uint32_t size = static_cast<uint32_t>(std::stoul(argv[++n], nullptr, 0));
                if (size == 0u || size > 4096u)
                    throw std::runtime_error("--dump-memory size must be between 1 and 4096 bytes");
                std::vector<uint8_t> bytes(size);
                if (!iop.readMemory(address, bytes.data(), bytes.size()))
                    throw std::runtime_error("Could not read requested IOP memory range");
                std::cout << "{\"memory_dump\":true,\"address\":" << address
                          << ",\"size\":" << size << ",\"hex\":\"";
                constexpr char hex[] = "0123456789abcdef";
                for (const uint8_t byte : bytes)
                    std::cout << hex[(byte >> 4u) & 0x0Fu] << hex[byte & 0x0Fu];
                std::cout << "\"}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--run-ee-cycles") {
                if (++n >= argc) throw std::runtime_error("--run-ee-cycles requires a cycle count");
                const uint64_t eeCycles = std::stoull(argv[n], nullptr, 0);
                iop.runEeCycles(eeCycles);
                for (const int32_t moduleId : scheduledModules) {
                    const auto state = iop.moduleStartState(moduleId);
                    std::cout << "{\"module_start_state\":true,\"module_id\":" << moduleId
                              << ",\"found\":" << state.found << ",\"pending\":" << state.pending
                              << ",\"returned\":" << state.returned << ",\"start_result\":" << state.startResult
                              << ",\"entry\":" << state.entry << ",\"stop_pc\":" << state.stopPc
                              << ",\"instructions\":" << state.instructions
                              << ",\"thread_id\":" << state.threadId
                              << ",\"thread_state\":" << state.threadState
                              << ",\"current_pc\":" << state.currentPc
                              << ",\"wait_id\":" << state.waitId
                              << ",\"wait_bits\":" << state.waitBits
                              << ",\"a0\":" << state.a0 << ",\"a1\":" << state.a1
                              << ",\"ra\":" << state.ra << "}\n";
                }
                for (const auto &scheduled : scheduledCalls) {
                    const auto state = iop.scheduledCallState(scheduled.id);
                    std::cout << "{\"scheduled_call_state\":true,\"call_id\":" << scheduled.id
                              << ",\"found\":" << state.found << ",\"finished\":" << state.finished
                              << ",\"returned\":" << state.returned << ",\"entry\":" << state.entry
                              << ",\"stop_pc\":" << state.stopPc << ",\"instructions\":" << state.instructions
                              << ",\"result\":" << state.value << "}\n";
                }
                for (const auto &line : host.logs) std::cerr << line << '\n';
                host.logs.clear();
                uint32_t ms = 0u, sm = 0u;
                (void)iop.readSifRegister(3u, ms);
                (void)iop.readSifRegister(4u, sm);
                std::cout << "{\"sif_sample\":true,\"ee_cycles\":" << eeCycles
                          << ",\"iop_cycles\":" << iop.cycles()
                          << ",\"iop_instructions\":" << iop.instructions()
                          << ",\"msflag\":" << ms << ",\"smflag\":" << sm << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--schedule-sifman-init") {
                const auto call = iop.scheduleExportCall("sifman", 5u, 0u, 0u, 0u, 0u);
                scheduledCalls.push_back(call);
                std::cout << "{\"scheduled_call\":\"IOP_sceSifInit\",\"found\":" << call.found
                          << ",\"call_id\":" << call.id << ",\"entry\":" << call.entry << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--schedule-sifcmd-init") {
                const auto call = iop.scheduleExportCall("sifcmd", 4u, 0u, 0u, 0u, 0u);
                scheduledCalls.push_back(call);
                std::cout << "{\"scheduled_call\":\"IOP_sceSifInitCmd\",\"found\":" << call.found
                          << ",\"call_id\":" << call.id << ",\"entry\":" << call.entry << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--fixture-ee-msflag") {
                if (++n >= argc) throw std::runtime_error("--fixture-ee-msflag requires a value");
                const uint32_t value = static_cast<uint32_t>(std::stoul(argv[n], nullptr, 0));
                uint32_t before = 0u;
                (void)iop.readSifRegister(3u, before);
                if (!iop.writeSifRegisterFromEe(3u, value))
                    throw std::runtime_error("fixture EE MSFLAG write failed");
                uint32_t after = 0u, sm = 0u;
                (void)iop.readSifRegister(3u, after);
                (void)iop.readSifRegister(4u, sm);
                std::cout << "{\"evidence\":\"fixture_only\",\"event\":\"EE_MSFLAG_write\""
                          << ",\"before_msflag\":" << before << ",\"after_msflag\":" << after
                          << ",\"smflag\":" << sm << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--fixture-ee-sifinit") {
                constexpr uint32_t kSifRegMsflag = 3u;
                constexpr uint32_t kSifStatSifInit = 0x00010000u;
                uint32_t beforeMs = 0u, beforeSm = 0u;
                (void)iop.readSifRegister(kSifRegMsflag, beforeMs);
                (void)iop.readSifRegister(4u, beforeSm);
                if (!iop.writeSifRegisterFromEe(kSifRegMsflag, kSifStatSifInit))
                    throw std::runtime_error("fixture EE MSFLAG write failed");
                uint32_t afterMs = 0u, afterSm = 0u;
                (void)iop.readSifRegister(kSifRegMsflag, afterMs);
                (void)iop.readSifRegister(4u, afterSm);
                std::cout << "{\"evidence\":\"fixture_only\",\"event\":\"EE_MSFLAG_SIFINIT\""
                          << ",\"before_msflag\":" << beforeMs << ",\"before_smflag\":" << beforeSm
                          << ",\"after_msflag\":" << afterMs << ",\"after_smflag\":" << afterSm << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--call-sifman-init") {
                const auto call = iop.callExport("sifman", 5u, 0u, 0u, 0u, 0u, 100000u);
                uint32_t ms = 0, sm = 0;
                (void)iop.readSifRegister(3, ms); (void)iop.readSifRegister(4, sm);
                std::cout << "{\"call\":\"IOP_sceSifInit\",\"found\":" << call.found
                          << ",\"returned\":" << call.returned << ",\"entry\":" << call.entry
                          << ",\"stop_pc\":" << call.stopPc << ",\"instructions\":" << call.instructions
                          << ",\"result\":" << call.value << ",\"msflag\":" << ms << ",\"smflag\":" << sm << "}\n";
                continue;
            }
            if (std::string_view(argv[n]) == "--call-sifcmd-init") {
                const auto call = iop.callExport("sifcmd", 4u, 0u, 0u, 0u, 0u, 100000u);
                uint32_t ms = 0, sm = 0;
                (void)iop.readSifRegister(3, ms); (void)iop.readSifRegister(4, sm);
                std::cout << "{\"call\":\"IOP_sceSifInitCmd\",\"found\":" << call.found
                          << ",\"returned\":" << call.returned << ",\"entry\":" << call.entry
                          << ",\"stop_pc\":" << call.stopPc << ",\"instructions\":" << call.instructions
                          << ",\"result\":" << call.value << ",\"msflag\":" << ms << ",\"smflag\":" << sm << "}\n";
                continue;
            }
            std::ifstream f(argv[n], std::ios::binary | std::ios::ate);
            if (!f || f.tellg() <= 0 || f.tellg() > 2 * 1024 * 1024)
                throw std::runtime_error("Invalid IRX file");
            host.file.resize(static_cast<size_t>(f.tellg()));
            f.seekg(0); f.read(reinterpret_cast<char *>(host.file.data()), host.file.size());
            if (!f) throw std::runtime_error("Truncated IRX read");
            const auto result = scheduleNextModuleStart
                                    ? iop.loadModuleScheduled(argv[n], nullptr, 0)
                                    : iop.loadModule(argv[n], nullptr, 0);
            scheduleNextModuleStart = false;
            if (result.startPending)
                scheduledModules.push_back(result.moduleId);
            uint32_t ms = 0, sm = 0;
            (void)iop.readSifRegister(3, ms);
            (void)iop.readSifRegister(4, sm);
            std::cout << "{\"module\":" << std::quoted(argv[n])
                      << ",\"id\":" << result.moduleId << ",\"start_result\":" << result.startResult
                      << ",\"start_pending\":" << result.startPending
                      << ",\"iop_instructions\":" << iop.instructions()
                      << ",\"msflag\":" << ms << ",\"smflag\":" << sm << "}\n";
            for (const auto &line : host.logs) std::cerr << line << '\n';
            host.logs.clear();
            if (result.moduleId < 0) return 1;
        }
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 2; }
}
