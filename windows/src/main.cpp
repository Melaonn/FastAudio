#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>
#include <optional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"
#include "microphone.hpp"
#include "qualification.hpp"
#include "ring_buffer.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

// The Windows SDK declares these Core Audio GUIDs but does not provide their
// definitions in the libraries used by this build. MinGW supplies them via
// -luuid, so only MSVC needs definitions here.
#ifdef _MSC_VER
extern "C" {
const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioClient3 = __uuidof(IAudioClient3);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
}
#endif

namespace fs = std::filesystem;
using namespace fastaudio;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> gStop{false};

BOOL WINAPI onConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT
            || signal == CTRL_CLOSE_EVENT) {
        gStop.store(true);
        return TRUE;
    }
    return FALSE;
}

struct ComError : std::runtime_error {
    explicit ComError(const std::string& message, HRESULT hr)
        : std::runtime_error(message + " (HRESULT=0x" + hex(hr) + ")") {
    }

    static std::string hex(HRESULT value) {
        std::ostringstream out;
        out << std::hex << std::uppercase << static_cast<uint32_t>(value);
        return out.str();
    }
};

void checkHr(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw ComError(message, hr);
    }
}

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0,
                                   nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring quote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(c);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring commandLine(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) {
            result.push_back(L' ');
        }
        result += quote(argument);
    }
    return result;
}

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : process(other.process), thread(other.thread) {
        other.process = nullptr;
        other.thread = nullptr;
    }
    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this != &other) {
            close();
            process = other.process;
            thread = other.thread;
            other.process = nullptr;
            other.thread = nullptr;
        }
        return *this;
    }

    ~ChildProcess() {
        close();
    }

    void terminate() {
        if (process) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
        }
    }

    void close() {
        if (thread) {
            CloseHandle(thread);
            thread = nullptr;
        }
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
    }
};

std::string runProcess(const fs::path& executable,
                       const std::vector<std::wstring>& args) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::vector<std::wstring> all{executable.wstring()};
    all.insert(all.end(), args.begin(), args.end());
    std::wstring cmd = commandLine(all);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        throw std::runtime_error("Could not start " + executable.string());
    }

    std::string output;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(readPipe, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &count, nullptr) && count) {
        output.append(buffer.data(), count);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        throw std::runtime_error(executable.string() + " failed: " + output);
    }
    return output;
}

ChildProcess launchProcess(const fs::path& executable,
                           const std::vector<std::wstring>& args) {
    std::vector<std::wstring> all{executable.wstring()};
    all.insert(all.end(), args.begin(), args.end());
    std::wstring cmd = commandLine(all);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("Could not launch " + executable.string());
    }
    ChildProcess child;
    child.process = process.hProcess;
    child.thread = process.hThread;
    return child;
}

fs::path findAdb(const std::optional<std::string>& explicitPath) {
    std::vector<fs::path> candidates;
    if (explicitPath) {
        candidates.emplace_back(*explicitPath);
    }
    if (const char* env = std::getenv("ADB")) {
        candidates.emplace_back(env);
    }

    wchar_t pathBuffer[MAX_PATH];
    DWORD length = SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH,
                               pathBuffer, nullptr);
    if (length && length < MAX_PATH) {
        candidates.emplace_back(pathBuffer);
    }
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        candidates.emplace_back(fs::path(local) / "Android" / "Sdk"
                                / "platform-tools" / "adb.exe");
    }
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            return fs::absolute(candidate);
        }
    }
    throw std::runtime_error("adb.exe not found; use --adb");
}

fs::path executableDirectory() {
    std::array<wchar_t, 32768> path{};
    DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                     static_cast<DWORD>(path.size()));
    return fs::path(std::wstring(path.data(), length)).parent_path();
}

struct Options {
    std::string command = "run";
    std::string serial;
    std::string packageName;
    std::string output = "default";
    std::string mode = "competitive";
    std::optional<std::string> adb;
    std::optional<double> latencyMs;
    std::string microphone = "none";
    bool shared = false;
    bool legacy = false;
    bool requalify = false;
    bool diagnostics = false;
};

void printUsage() {
    std::cout
        << "FastAudio [probe|qualify|run|diagnostics] --serial DEVICE "
           "--package PACKAGE [options]\n"
        << "  --adb PATH          explicit adb.exe\n"
        << "  --output default    Windows output endpoint (default only in v1)\n"
        << "  --mode competitive  qualification mode\n"
        << "  --shared            use WASAPI shared mode\n"
        << "  --legacy            use the original 128-frame transport/controller\n"
        << "  --latency-ms N       diagnostic reserve override (2-40ms; legacy 8-40)\n"
        << "  --requalify         ignore cached qualification\n"
        << "  --microphone MODE   none, select, default, or a Windows endpoint ID\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;
    int index = 1;
    if (index < argc && argv[index][0] != '-') {
        options.command = argv[index++];
    }
    options.diagnostics = options.command == "diagnostics";
    for (; index < argc; ++index) {
        std::string argument = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc) {
                throw std::runtime_error("Missing value for " + argument);
            }
            return argv[index];
        };
        if (argument == "--serial") {
            options.serial = value();
        } else if (argument == "--package") {
            options.packageName = value();
        } else if (argument == "--adb") {
            options.adb = value();
        } else if (argument == "--output") {
            options.output = value();
        } else if (argument == "--mode") {
            options.mode = value();
        } else if (argument == "--shared") {
            options.shared = true;
        } else if (argument == "--legacy") {
            options.legacy = true;
        } else if (argument == "--latency-ms") {
            options.latencyMs = std::stod(value());
        } else if (argument == "--requalify") {
            options.requalify = true;
        } else if (argument == "--microphone") {
            options.microphone = value();
        } else if (argument == "--help" || argument == "-h") {
            printUsage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }
    if (options.serial.empty() || options.packageName.empty()) {
        throw std::runtime_error("--serial and --package are required");
    }
    if (options.output != "default") {
        throw std::runtime_error("v1 currently supports --output default only");
    }
    if (options.latencyMs
            && (!std::isfinite(*options.latencyMs)
                    || *options.latencyMs < (options.legacy ? 8.0 : 2.0)
                    || *options.latencyMs > 40.0)) {
        throw std::runtime_error(options.legacy
                ? "--latency-ms must be between 8 and 40 in legacy mode"
                : "--latency-ms must be between 2 and 40");
    }
    if (options.command != "probe" && options.command != "qualify"
            && options.command != "run" && options.command != "diagnostics") {
        throw std::runtime_error("Unknown command: " + options.command);
    }
    if (options.latencyMs
            && options.command != "run" && options.command != "diagnostics") {
        throw std::runtime_error(
                "--latency-ms is only valid with run or diagnostics");
    }
    if (options.microphone.empty()) {
        throw std::runtime_error("--microphone requires none, select, default, or an endpoint ID");
    }
    if (options.microphone != "none"
            && options.command != "probe"
            && options.command != "run" && options.command != "diagnostics") {
        throw std::runtime_error("--microphone is only valid with probe, run, or diagnostics");
    }
    return options;
}

struct MicrophoneEndpoint {
    std::wstring id;
    std::wstring name;
};

class ComApartment {
public:
    ComApartment() {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        checkHr(result, "CoInitializeEx");
        initialized_ = true;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_ = false;
};

std::wstring deviceFriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    checkHr(device->OpenPropertyStore(STGM_READ, &store), "Open microphone property store");
    PROPVARIANT value;
    PropVariantInit(&value);
    HRESULT result = store->GetValue(PKEY_Device_FriendlyName, &value);
    std::wstring name;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return name.empty() ? L"Unnamed microphone" : name;
}

std::vector<MicrophoneEndpoint> enumerateMicrophones() {
    ComApartment com;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    checkHr(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                             IID_IMMDeviceEnumerator,
                             reinterpret_cast<void**>(&enumerator)),
            "Create microphone device enumerator");
    try {
        checkHr(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices),
                "Enumerate active microphones");
        UINT count = 0;
        checkHr(devices->GetCount(&count), "Get microphone count");
        std::vector<MicrophoneEndpoint> result;
        result.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            IMMDevice* device = nullptr;
            checkHr(devices->Item(index, &device), "Get microphone device");
            LPWSTR id = nullptr;
            checkHr(device->GetId(&id), "Get microphone endpoint ID");
            MicrophoneEndpoint endpoint;
            endpoint.id = id;
            endpoint.name = deviceFriendlyName(device);
            CoTaskMemFree(id);
            device->Release();
            result.push_back(std::move(endpoint));
        }
        devices->Release();
        enumerator->Release();
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return result;
    } catch (...) {
        if (devices) devices->Release();
        enumerator->Release();
        throw;
    }
}

MicrophoneEndpoint defaultMicrophone() {
    ComApartment com;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    checkHr(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                             IID_IMMDeviceEnumerator,
                             reinterpret_cast<void**>(&enumerator)),
            "Create microphone device enumerator");
    try {
        HRESULT result = enumerator->GetDefaultAudioEndpoint(
                eCapture, eCommunications, &device);
        if (FAILED(result)) {
            checkHr(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device),
                    "Get default microphone endpoint");
        }
        LPWSTR id = nullptr;
        checkHr(device->GetId(&id), "Get default microphone endpoint ID");
        MicrophoneEndpoint endpoint;
        endpoint.id = id;
        endpoint.name = deviceFriendlyName(device);
        CoTaskMemFree(id);
        device->Release();
        enumerator->Release();
        return endpoint;
    } catch (...) {
        if (device) device->Release();
        enumerator->Release();
        throw;
    }
}

std::optional<MicrophoneEndpoint> selectMicrophone(const std::string& option) {
    if (option == "none") {
        return std::nullopt;
    }
    if (option == "default") {
        return defaultMicrophone();
    }
    if (option != "select") {
        for (const auto& endpoint : enumerateMicrophones()) {
            if (narrow(endpoint.id) == option) {
                return endpoint;
            }
        }
        throw std::runtime_error("Selected microphone endpoint is not active");
    }

    const auto endpoints = enumerateMicrophones();
    std::cout << "\nPC microphone:\n  0) No PC microphone (playback only)\n";
    for (size_t index = 0; index < endpoints.size(); ++index) {
        std::cout << "  " << (index + 1) << ") "
                  << narrow(endpoints[index].name) << '\n';
    }
    std::cout << "Select microphone [0-" << endpoints.size() << "]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw std::runtime_error("Microphone selection was cancelled");
    }
    if (line.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(line.begin(), line.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        throw std::runtime_error("Microphone selection must be a number");
    }
    const size_t selected = std::stoul(line);
    if (selected == 0) {
        return std::nullopt;
    }
    if (selected > endpoints.size()) {
        throw std::runtime_error("Microphone selection is out of range");
    }
    return endpoints[selected - 1];
}

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET socket) : socket_(socket) {
    }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(other.socket_) {
        other.socket_ = INVALID_SOCKET;
    }
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            if (socket_ != INVALID_SOCKET) {
                closesocket(socket_);
            }
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }
    ~SocketHandle() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }
    SOCKET get() const { return socket_; }
private:
    SOCKET socket_ = INVALID_SOCKET;
};

SocketHandle connectLocal(uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(socket, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0) {
            BOOL noDelay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            return SocketHandle(socket);
        }
        closesocket(socket);
        std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error("Timed out connecting to Android audio daemon");
}

void receiveAll(SOCKET socket, void* target, size_t size) {
    auto* bytes = static_cast<uint8_t*>(target);
    while (size) {
        int count = recv(socket, reinterpret_cast<char*>(bytes),
                         static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
        if (count <= 0) {
            throw std::runtime_error("Audio socket disconnected");
        }
        bytes += count;
        size -= count;
    }
}

void sendAll(SOCKET socket, const void* source, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(source);
    while (size) {
        int count = send(socket, reinterpret_cast<const char*>(bytes),
                         static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
        if (count <= 0) {
            throw std::runtime_error("Microphone socket disconnected");
        }
        bytes += count;
        size -= count;
    }
}

Hello receiveHello(SOCKET socket) {
    std::array<uint8_t, kHelloSize> bytes{};
    receiveAll(socket, bytes.data(), bytes.size());
    if (readU32(bytes.data()) != kHelloMagic
            || readU16(bytes.data() + 4) != kProtocolVersion
            || readU16(bytes.data() + 6) != kHelloSize) {
        throw std::runtime_error("Unsupported FastAudio hello");
    }
    Hello hello;
    hello.sampleRate = readU32(bytes.data() + 8);
    hello.channels = readU16(bytes.data() + 12);
    hello.bitsPerSample = readU16(bytes.data() + 14);
    hello.packetFrames = readU32(bytes.data() + 16);
    hello.sessionId = readU32(bytes.data() + 20);
    hello.capabilities = readU32(bytes.data() + 24);
    if (hello.sampleRate != kSampleRate || hello.channels != kChannels
            || hello.bitsPerSample != kBitsPerSample) {
        throw std::runtime_error("Unsupported Android PCM format");
    }
    return hello;
}

MicHello receiveMicHello(SOCKET socket) {
    std::array<uint8_t, kMicHelloSize> bytes{};
    receiveAll(socket, bytes.data(), bytes.size());
    if (readU32(bytes.data()) != kMicHelloMagic
            || readU16(bytes.data() + 4) != kProtocolVersion
            || readU16(bytes.data() + 6) != kMicHelloSize) {
        throw std::runtime_error("Unsupported FastAudio microphone hello");
    }
    MicHello hello;
    hello.sampleRate = readU32(bytes.data() + 8);
    hello.channels = readU16(bytes.data() + 12);
    hello.bitsPerSample = readU16(bytes.data() + 14);
    hello.packetFrames = readU32(bytes.data() + 16);
    hello.status = readU32(bytes.data() + 20);
    hello.packageUid = readU32(bytes.data() + 24);
    if (hello.sampleRate != kMicSampleRate || hello.channels != kMicChannels
            || hello.bitsPerSample != kMicBitsPerSample
            || hello.packetFrames != kMicPacketFrames) {
        throw std::runtime_error("Unsupported Android microphone PCM format");
    }
    return hello;
}

PacketHeader receivePacket(SOCKET socket, std::vector<int16_t>& pcm) {
    std::array<uint8_t, kPacketHeaderSize> bytes{};
    receiveAll(socket, bytes.data(), bytes.size());
    if (readU32(bytes.data()) != kPacketMagic
            || readU16(bytes.data() + 4) != kProtocolVersion
            || readU16(bytes.data() + 6) != kPacketHeaderSize) {
        throw std::runtime_error("Invalid PCM packet header");
    }
    PacketHeader header;
    header.sequence = readU64(bytes.data() + 8);
    header.captureTimeNs = readU64(bytes.data() + 16);
    header.frameCount = readU32(bytes.data() + 24);
    header.flags = readU32(bytes.data() + 28);
    if (!header.frameCount || header.frameCount > 4096) {
        throw std::runtime_error("Invalid PCM frame count");
    }
    pcm.resize(static_cast<size_t>(header.frameCount) * kChannels);
    receiveAll(socket, pcm.data(),
               static_cast<size_t>(header.frameCount) * kBytesPerFrame);
    return header;
}

class AdbSession {
public:
    AdbSession(fs::path adb, std::string serial, std::string packageName,
               uint32_t packetFrames, bool microphone = false)
        : adb_(std::move(adb)), serial_(std::move(serial)),
          packageName_(std::move(packageName)), packetFrames_(packetFrames),
          microphone_(microphone) {
    }

    ~AdbSession() {
        stop();
    }

    void start() {
        const fs::path daemonJar = executableDirectory() / "fastaudio-daemon.jar";
        if (!fs::exists(daemonJar)) {
            throw std::runtime_error("Missing " + daemonJar.string()
                    + "; run scripts\\build.ps1");
        }
        run({L"-s", widen(serial_), L"get-state"});
        run({L"-s", widen(serial_), L"push", daemonJar.wstring(),
             L"/data/local/tmp/fastaudio-daemon.jar"});
        try {
            run({L"-s", widen(serial_), L"forward", L"--remove", L"tcp:27183"});
        } catch (...) {
            // No previous forward is normal.
        }
        run({L"-s", widen(serial_), L"forward", L"tcp:27183",
             L"localabstract:fastaudio"});
        if (microphone_) {
            try {
                run({L"-s", widen(serial_), L"forward", L"--remove", L"tcp:27184"});
            } catch (...) {
                // No previous forward is normal.
            }
            run({L"-s", widen(serial_), L"forward", L"tcp:27184",
                 L"localabstract:fastaudio-mic"});
        }
        std::vector<std::wstring> daemonArguments{
            L"-s", widen(serial_), L"shell",
            L"CLASSPATH=/data/local/tmp/fastaudio-daemon.jar",
            L"app_process", L"/system/bin", L"com.fastaudio.Main",
            L"--package", widen(packageName_), L"--socket", L"fastaudio",
            L"--packet-frames", std::to_wstring(packetFrames_)
        };
        if (microphone_) {
            daemonArguments.emplace_back(L"--microphone");
            daemonArguments.emplace_back(L"--microphone-socket");
            daemonArguments.emplace_back(L"fastaudio-mic");
        }
        daemon_ = launchProcess(adb_, daemonArguments);
        std::string lastError;
        for (int attempt = 0; attempt < 100; ++attempt) {
            try {
                socket_ = connectLocal(27183);
                hello_ = receiveHello(socket_.get());
                if (hello_.packetFrames != packetFrames_) {
                    throw std::runtime_error(
                            "Android daemon packet size mismatch: requested "
                            + std::to_string(packetFrames_) + ", received "
                            + std::to_string(hello_.packetFrames));
                }
                if (microphone_) {
                    micSocket_ = connectLocal(27184);
                    int sendBufferBytes = 4096;
                    setsockopt(micSocket_.get(), SOL_SOCKET, SO_SNDBUF,
                               reinterpret_cast<const char*>(&sendBufferBytes),
                               sizeof(sendBufferBytes));
                    micHello_ = receiveMicHello(micSocket_.get());
                }
                return;
            } catch (const std::exception& e) {
                lastError = e.what();
                socket_ = SocketHandle();
                if (lastError.rfind(
                            "Android daemon packet size mismatch", 0) == 0) {
                    break;
                }
                std::this_thread::sleep_for(50ms);
            }
        }
        throw std::runtime_error(
                "Android daemon did not become ready: " + lastError);
    }

    void stop() {
        micSocket_ = SocketHandle();
        socket_ = SocketHandle();
        daemon_.terminate();
        daemon_.close();
        if (!adb_.empty()) {
            try {
                run({L"-s", widen(serial_), L"forward", L"--remove", L"tcp:27183"});
            } catch (...) {
            }
            if (microphone_) {
                try {
                    run({L"-s", widen(serial_), L"forward", L"--remove", L"tcp:27184"});
                } catch (...) {
                }
            }
        }
    }

    SOCKET socket() const { return socket_.get(); }
    SOCKET microphoneSocket() const { return micSocket_.get(); }
    const Hello& hello() const { return hello_; }
    const MicHello& microphoneHello() const { return micHello_; }
    bool microphoneReady() const {
        return microphone_ && (micHello_.status & kMicStatusReady);
    }

    std::string fingerprint() {
        std::string output = run({L"-s", widen(serial_), L"shell",
                                  L"getprop", L"ro.build.fingerprint"});
        output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
        output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
        return output;
    }

private:
    std::string run(const std::vector<std::wstring>& args) {
        return runProcess(adb_, args);
    }

    fs::path adb_;
    std::string serial_;
    std::string packageName_;
    uint32_t packetFrames_;
    bool microphone_ = false;
    ChildProcess daemon_;
    SocketHandle socket_;
    SocketHandle micSocket_;
    Hello hello_;
    MicHello micHello_;
};

struct RuntimeMetrics {
    std::atomic<uint64_t> receivedFrames{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> concealedFrames{0};
    std::atomic<uint64_t> hardUnderruns{0};
    std::atomic<uint64_t> rebufferEvents{0};
    std::atomic<uint64_t> resyncs{0};
    std::atomic<uint32_t> queueFrames{0};
    std::atomic<int32_t> correctionPpm{0};
    std::atomic<bool> rebuffering{false};

    void resetSessionCounters() {
        droppedFrames.store(0);
        concealedFrames.store(0);
        hardUnderruns.store(0);
        rebufferEvents.store(0);
        resyncs.store(0);
    }
};

struct MicrophoneMetrics {
    std::atomic<uint64_t> capturedFrames{0};
    std::atomic<uint64_t> sentFrames{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> discontinuities{0};
    std::atomic<uint32_t> queueFrames{0};
    std::atomic<bool> active{false};
    std::atomic<bool> unavailable{false};
};

class WasapiMicrophoneCapture {
public:
    WasapiMicrophoneCapture(std::wstring endpointId, MonoPcmRingBuffer& ring,
                            MicrophoneMetrics& metrics, std::atomic<bool>& stop,
                            std::atomic<bool>& finished)
        : endpointId_(std::move(endpointId)), ring_(ring), metrics_(metrics),
          stop_(stop), finished_(finished) {
    }

    ~WasapiMicrophoneCapture() {
        close();
    }

    void run(std::promise<void> ready) {
        bool readyReported = false;
        try {
            open();
            ready.set_value();
            readyReported = true;
            metrics_.active.store(true);
            capture();
        } catch (...) {
            metrics_.unavailable.store(true);
            if (!readyReported) {
                ready.set_exception(std::current_exception());
            } else if (!gStop.load() && !stop_.load()) {
                try {
                    throw;
                } catch (const std::exception& error) {
                    std::cerr << "Microphone capture stopped: " << error.what() << '\n';
                } catch (...) {
                    std::cerr << "Microphone capture stopped unexpectedly\n";
                }
            }
        }
        close();
        metrics_.active.store(false);
        finished_.store(true);
    }

private:
    void open() {
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
                "CoInitializeEx microphone capture");
        comInitialized_ = true;
        checkHr(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                 IID_IMMDeviceEnumerator,
                                 reinterpret_cast<void**>(&enumerator_)),
                "Create microphone device enumerator");
        checkHr(enumerator_->GetDevice(endpointId_.c_str(), &device_),
                "Get selected microphone endpoint");
        checkHr(device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client_)),
                "Activate selected microphone");

        WAVEFORMATEX* mixFormat = nullptr;
        checkHr(client_->GetMixFormat(&mixFormat), "Get microphone mix format");
        try {
            sourceRate_ = mixFormat->nSamplesPerSec;
            channels_ = mixFormat->nChannels;
            blockAlign_ = mixFormat->nBlockAlign;
            if (!sourceRate_ || !channels_ || !blockAlign_) {
                throw std::runtime_error("Invalid selected microphone format");
            }
            if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                encoding_ = MicSampleEncoding::Float32;
            } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                    // WAVEFORMATEXTENSIBLE stores the original format tag in
                    // the SubFormat GUID's Data1 field. Avoid a ksmedia GUID
                    // link dependency in the standalone UCRT build.
                    && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat)
                            ->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT) {
                encoding_ = MicSampleEncoding::Float32;
            } else {
                encoding_ = MicSampleEncoding::PcmInteger;
            }
            if (blockAlign_ / channels_ < 2 || blockAlign_ / channels_ > 4) {
                throw std::runtime_error("Unsupported selected microphone bit depth");
            }
            resampler_ = std::make_unique<LinearMonoResampler>(sourceRate_, kMicSampleRate);
            checkHr(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        0, 0, mixFormat, nullptr),
                    "Initialize microphone capture");
        } catch (...) {
            CoTaskMemFree(mixFormat);
            throw;
        }
        CoTaskMemFree(mixFormat);

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            throw std::runtime_error("Create microphone capture event failed");
        }
        checkHr(client_->SetEventHandle(event_), "Set microphone capture event");
        checkHr(client_->GetService(IID_IAudioCaptureClient,
                                    reinterpret_cast<void**>(&captureClient_)),
                "Get microphone capture client");
        checkHr(client_->Start(), "Start microphone capture");
        started_ = true;
        std::cout << "PC microphone ready: " << narrow(deviceFriendlyName(device_))
                  << " (" << sourceRate_ << "Hz, " << channels_ << "ch)\n";
    }

    void capture() {
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        if (mmcss) {
            AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
        while (!gStop.load() && !stop_.load()) {
            DWORD wait = WaitForSingleObject(event_, 500);
            if (wait != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 packetFrames = 0;
            checkHr(captureClient_->GetNextPacketSize(&packetFrames),
                    "Get microphone packet size");
            while (packetFrames) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                checkHr(captureClient_->GetBuffer(&data, &frames, &flags,
                                                  nullptr, nullptr),
                        "Get microphone capture buffer");
                try {
                    std::vector<float> mono = (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            ? std::vector<float>(frames, 0.0f)
                            : downmixMicrophone(data, frames, channels_, blockAlign_, encoding_);
                    std::vector<int16_t> converted;
                    converted.reserve(static_cast<size_t>(frames)
                                      * kMicSampleRate / sourceRate_ + 4);
                    resampler_->append(mono, converted);
                    if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                        metrics_.discontinuities.fetch_add(1);
                    }
                    if (!converted.empty()) {
                        uint32_t accepted = ring_.push(
                                converted.data(), static_cast<uint32_t>(converted.size()));
                        if (accepted != converted.size()) {
                            metrics_.droppedFrames.fetch_add(converted.size() - accepted);
                            metrics_.discontinuities.fetch_add(1);
                        }
                    }
                    metrics_.capturedFrames.fetch_add(frames);
                    metrics_.queueFrames.store(ring_.available());
                } catch (...) {
                    captureClient_->ReleaseBuffer(frames);
                    throw;
                }
                checkHr(captureClient_->ReleaseBuffer(frames),
                        "Release microphone capture buffer");
                checkHr(captureClient_->GetNextPacketSize(&packetFrames),
                        "Get microphone packet size");
            }
        }
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
    }

    void close() {
        if (started_ && client_) {
            client_->Stop();
            started_ = false;
        }
        if (captureClient_) captureClient_->Release();
        if (client_) client_->Release();
        if (device_) device_->Release();
        if (enumerator_) enumerator_->Release();
        if (event_) CloseHandle(event_);
        if (comInitialized_) CoUninitialize();
        captureClient_ = nullptr;
        client_ = nullptr;
        device_ = nullptr;
        enumerator_ = nullptr;
        event_ = nullptr;
        comInitialized_ = false;
    }

    std::wstring endpointId_;
    MonoPcmRingBuffer& ring_;
    MicrophoneMetrics& metrics_;
    std::atomic<bool>& stop_;
    std::atomic<bool>& finished_;
    bool comInitialized_ = false;
    bool started_ = false;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    HANDLE event_ = nullptr;
    uint32_t sourceRate_ = 0;
    uint16_t channels_ = 0;
    uint16_t blockAlign_ = 0;
    MicSampleEncoding encoding_ = MicSampleEncoding::PcmInteger;
    std::unique_ptr<LinearMonoResampler> resampler_;
};

void streamMicrophone(SOCKET socket, MonoPcmRingBuffer& ring,
                      MicrophoneMetrics& metrics,
                      const std::atomic<bool>& captureFinished,
                      std::atomic<bool>& stop) {
    std::array<uint8_t, kMicPacketHeaderSize> header{};
    std::array<int16_t, kMicPacketFrames> pcm{};
    uint64_t sequence = 0;
    uint64_t reportedDiscontinuities = 0;
    try {
        while (!gStop.load() && !stop.load()) {
            uint32_t available = ring.available();
            if (available < kMicPacketFrames && !captureFinished.load()) {
                metrics.queueFrames.store(available);
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (!available && captureFinished.load()) {
                break;
            }
            uint32_t frames = ring.pop(pcm.data(),
                    std::min<uint32_t>(kMicPacketFrames, available));
            if (!frames) {
                continue;
            }
            const uint64_t currentDiscontinuities = metrics.discontinuities.load();
            const bool discontinuity = currentDiscontinuities != reportedDiscontinuities;
            reportedDiscontinuities = currentDiscontinuities;
            writeU32(header.data(), kMicPacketMagic);
            writeU16(header.data() + 4, kProtocolVersion);
            writeU16(header.data() + 6, kMicPacketHeaderSize);
            writeU64(header.data() + 8, sequence++);
            writeU64(header.data() + 16, static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count()));
            writeU32(header.data() + 24, frames);
            writeU32(header.data() + 28, discontinuity ? kMicFlagDiscontinuity : 0);
            sendAll(socket, header.data(), header.size());
            sendAll(socket, pcm.data(), static_cast<size_t>(frames) * kMicBytesPerFrame);
            metrics.sentFrames.fetch_add(frames);
            metrics.queueFrames.store(ring.available());
        }
    } catch (const std::exception& error) {
        if (!gStop.load()) {
            std::cerr << "Microphone sender stopped: " << error.what() << '\n';
        }
        stop.store(true);
    }
    shutdown(socket, SD_SEND);
}

class WasapiRenderer {
public:
    WasapiRenderer(PcmRingBuffer& ring, RuntimeMetrics& metrics,
                   double targetMs, bool shared, uint32_t burstFrames,
                   bool legacy)
        : ring_(ring), metrics_(metrics),
          targetFrames_(static_cast<uint32_t>(targetMs * kSampleRate / 1000.0)),
          shared_(shared), burstFrames_(burstFrames), legacy_(legacy) {
        open();
        setTargetMs(targetMs);
    }

    ~WasapiRenderer() {
        close();
    }

    std::string endpointId() const {
        return narrow(endpointId_);
    }

    uint32_t periodFrames() const {
        return periodFrames_;
    }

    uint32_t bufferFrames() const {
        return bufferFrames_;
    }

    double streamLatencyMs() const {
        return static_cast<double>(streamLatencyHns_) / 10'000.0;
    }

    double endpointLatencyMs() const {
        return std::max(streamLatencyMs(),
                periodFrames_ * 1000.0 / kSampleRate);
    }

    double minimumReserveMs() const {
        return minimumBurstReserveMs(
                legacy_, shared_, bufferFrames_, burstFrames_, kSampleRate);
    }

    double targetMs() const {
        return targetFrames_ * 1000.0 / kSampleRate;
    }

    const std::string& initializationPath() const {
        return initializationPath_;
    }

    void setTargetMs(double targetMs) {
        targetFrames_ = static_cast<uint32_t>(
                targetMs * kSampleRate / 1000.0);
        if (minimumReserveMs() > 0) {
            targetFrames_ = std::max(targetFrames_, bufferFrames_ - burstFrames_);
        }
    }

    bool isShared() const {
        return shared_;
    }

    void run() {
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        if (mmcss) {
            AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
        }

        if (legacy_) {
            while (!gStop.load() && ring_.available() < targetFrames_) {
                std::this_thread::sleep_for(1ms);
            }
        } else {
            const uint32_t startupFrames = std::max(
                    burstFrames_, targetFrames_ + periodFrames_);
            while (!gStop.load() && ring_.available() < startupFrames) {
                std::this_thread::sleep_for(1ms);
            }
            const auto phaseDelay = std::chrono::nanoseconds(
                    static_cast<int64_t>(targetFrames_ + periodFrames_)
                    * 1'000'000'000 / kSampleRate);
            const auto readyAt = std::chrono::steady_clock::now() + phaseDelay;
            while (!gStop.load() && std::chrono::steady_clock::now() < readyAt) {
                std::this_thread::sleep_for(1ms);
            }
        }
        if (gStop.load()) {
            return;
        }

        prefill();
        checkHr(client_->Start(), "IAudioClient::Start");
        metrics_.resetSessionCounters();
        metricsArmed_ = false;
        renderedFrames_ = 0;
        while (!gStop.load()) {
            DWORD wait = WaitForSingleObject(event_, 1000);
            if (wait != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 frames = bufferFrames_;
            if (shared_) {
                UINT32 padding = 0;
                checkHr(client_->GetCurrentPadding(&padding),
                        "IAudioClient::GetCurrentPadding");
                if (padding >= bufferFrames_) {
                    continue;
                }
                frames = bufferFrames_ - padding;
            }
            render(frames);
        }
        client_->Stop();
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
    }

private:
    void open() {
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
                "CoInitializeEx");
        comInitialized_ = true;
        checkHr(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                 CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                 reinterpret_cast<void**>(&enumerator_)),
                "Create MMDeviceEnumerator");
        checkHr(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_),
                "Get default audio endpoint");
        LPWSTR endpoint = nullptr;
        checkHr(device_->GetId(&endpoint), "IMMDevice::GetId");
        endpointId_ = endpoint;
        CoTaskMemFree(endpoint);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            throw std::runtime_error("CreateEvent failed");
        }

        if (!shared_ && openExclusive()) {
            return;
        }
        shared_ = true;
        openShared();
    }

    void activateClient() {
        if (renderClient_) {
            renderClient_->Release();
            renderClient_ = nullptr;
        }
        if (client_) {
            client_->Release();
            client_ = nullptr;
        }
        checkHr(device_->Activate(IID_IAudioClient, CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void**>(&client_)),
                "Activate IAudioClient");
    }

    static WAVEFORMATEX pcmFormat() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kSampleRate * 4;
        return format;
    }

    bool openExclusive() {
        const uint32_t periods[] = {64, 96, 128, 144, 192, 240, 256, 480};
        for (uint32_t period : periods) {
            activateClient();
            WAVEFORMATEX format = pcmFormat();
            REFERENCE_TIME duration =
                    static_cast<REFERENCE_TIME>(period) * 10'000'000 / kSampleRate;
            HRESULT hr = client_->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                    duration, duration, &format, nullptr);
            if (SUCCEEDED(hr)) {
                periodFrames_ = period;
                outputFloat_ = false;
                initializationPath_ = "exclusive";
                finishOpen();
                return true;
            }
        }
        return false;
    }

    void openShared() {
        activateClient();
        WAVEFORMATEX desired = pcmFormat();
        if (legacy_) {
            IAudioClient3* legacyClient3 = nullptr;
            HRESULT legacyQuery = client_->QueryInterface(
                    IID_IAudioClient3,
                    reinterpret_cast<void**>(&legacyClient3));
            if (SUCCEEDED(legacyQuery)) {
                UINT32 defaultPeriod = 0;
                UINT32 fundamental = 0;
                UINT32 minimum = 0;
                UINT32 maximum = 0;
                HRESULT periodResult = legacyClient3->GetSharedModeEnginePeriod(
                        &desired, &defaultPeriod, &fundamental,
                        &minimum, &maximum);
                if (SUCCEEDED(periodResult)) {
                    HRESULT initialize = legacyClient3->InitializeSharedAudioStream(
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                    | AUDCLNT_STREAMFLAGS_NOPERSIST,
                            minimum, &desired, nullptr);
                    if (SUCCEEDED(initialize)) {
                        periodFrames_ = minimum;
                        outputFloat_ = false;
                        initializationPath_ = "shared-legacy-iaudioclient3";
                        legacyClient3->Release();
                        finishOpen();
                        return;
                    }
                }
                legacyClient3->Release();
            }
            activateClient();
            REFERENCE_TIME duration =
                    INT64_C(10'000'000) * 3 * 192 / kSampleRate;
            checkHr(client_->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                    | AUDCLNT_STREAMFLAGS_NOPERSIST,
                            duration, 0, &desired, nullptr),
                    "Initialize WASAPI legacy shared stream");
            periodFrames_ = 192;
            outputFloat_ = false;
            initializationPath_ = "shared-legacy-12ms";
            finishOpen();
            return;
        }
        IAudioClient3* client3 = nullptr;
        HRESULT query = client_->QueryInterface(
                IID_IAudioClient3, reinterpret_cast<void**>(&client3));
        if (SUCCEEDED(query)) {
            UINT32 defaultPeriod = 0;
            UINT32 fundamental = 0;
            UINT32 minimum = 0;
            UINT32 maximum = 0;
            HRESULT periodResult = client3->GetSharedModeEnginePeriod(
                    &desired, &defaultPeriod, &fundamental, &minimum, &maximum);
            if (SUCCEEDED(periodResult)) {
                HRESULT initialize = client3->InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        minimum, &desired, nullptr);
                if (SUCCEEDED(initialize)) {
                    periodFrames_ = minimum;
                    outputFloat_ = false;
                    initializationPath_ = "shared-iaudioclient3-minimum";
                    client3->Release();
                    finishOpen();
                    return;
                }
            }
            client3->Release();
        }

        activateClient();
        REFERENCE_TIME defaultPeriod = 0;
        HRESULT periodResult = client_->GetDevicePeriod(&defaultPeriod, nullptr);
        HRESULT minimumBuffer = client_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                0, 0, &desired, nullptr);
        if (SUCCEEDED(minimumBuffer)) {
            periodFrames_ = SUCCEEDED(periodResult)
                    ? std::max<uint32_t>(1, static_cast<uint32_t>(
                            (defaultPeriod * kSampleRate + 5'000'000)
                            / 10'000'000))
                    : 192;
            outputFloat_ = false;
            initializationPath_ = "shared-minimum-buffer";
            finishOpen();
            return;
        }

        // Exact pre-v2 shared initialization remains the final compatibility path.
        activateClient();
        REFERENCE_TIME duration =
                INT64_C(10'000'000) * 3 * 192 / kSampleRate;
        checkHr(client_->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                | AUDCLNT_STREAMFLAGS_NOPERSIST,
                        duration, 0, &desired, nullptr),
                "Initialize WASAPI legacy shared stream");
        periodFrames_ = 192;
        outputFloat_ = false;
        initializationPath_ = "shared-legacy-12ms";
        finishOpen();
    }

    void finishOpen() {
        checkHr(client_->SetEventHandle(event_), "Set WASAPI event");
        checkHr(client_->GetBufferSize(&bufferFrames_), "Get WASAPI buffer size");
        if (FAILED(client_->GetStreamLatency(&streamLatencyHns_))) {
            streamLatencyHns_ = 0;
        }
        checkHr(client_->GetService(
                        IID_IAudioRenderClient,
                        reinterpret_cast<void**>(&renderClient_)),
                "Get IAudioRenderClient");
        sourceBuffer_.resize(static_cast<size_t>(bufferFrames_ + 16) * 2);
    }

    void prefill() {
        UINT32 frames = shared_ ? periodFrames_ : bufferFrames_;
        render(frames);
    }

    void render(UINT32 outputFrames) {
        if (!outputFrames) {
            return;
        }
        BYTE* destination = nullptr;
        checkHr(renderClient_->GetBuffer(outputFrames, &destination),
                "Get WASAPI render buffer");

        uint32_t available = ring_.available();
        // The low-latency transport intentionally arrives one Android HAL
        // quantum at a time. That quantum is useful audio, not stale backlog.
        const uint32_t highMargin = legacy_
                ? (shared_
                        ? std::max<uint32_t>(bufferFrames_ * 2, targetFrames_ * 4)
                        : std::max<uint32_t>(
                                periodFrames_ * 2, kSampleRate * 20 / 1000))
                : burstFrames_ + std::max<uint32_t>(
                        bufferFrames_, periodFrames_ * 2);
        const uint32_t high = targetFrames_ + highMargin;
        if (available > high) {
            uint32_t keepBeforePull = targetFrames_ + outputFrames
                    + (legacy_ ? 0 : burstFrames_);
            uint32_t dropped = ring_.discard(
                    available > keepBeforePull ? available - keepBeforePull : 0);
            metrics_.droppedFrames.fetch_add(dropped);
            metrics_.resyncs.fetch_add(1);
            available = ring_.available();
            if (!legacy_) {
                smoothedQueue_ = 0;
                integralError_ = 0;
                sourceFraction_ = 0;
                fadeIn_ = true;
            }
        }

        if (rebuffering_) {
            uint32_t required = legacy_
                    ? targetFrames_
                    : std::max(burstFrames_, targetFrames_ + outputFrames);
            if (available < required) {
                rebufferDelayArmed_ = false;
                std::memset(destination, 0,
                            static_cast<size_t>(outputFrames) * kBytesPerFrame);
                metrics_.queueFrames.store(available);
                metrics_.correctionPpm.store(0);
                checkHr(renderClient_->ReleaseBuffer(
                                outputFrames, AUDCLNT_BUFFERFLAGS_SILENT),
                        "Release WASAPI rebuffer silence");
                return;
            }
            if (!legacy_) {
                const auto now = std::chrono::steady_clock::now();
                if (!rebufferDelayArmed_) {
                    const auto delay = std::chrono::nanoseconds(
                            static_cast<int64_t>(targetFrames_ + periodFrames_)
                            * 1'000'000'000 / kSampleRate);
                    rebufferReadyAt_ = now + delay;
                    rebufferDelayArmed_ = true;
                }
                if (now < rebufferReadyAt_) {
                    std::memset(destination, 0,
                                static_cast<size_t>(outputFrames)
                                        * kBytesPerFrame);
                    metrics_.queueFrames.store(available);
                    metrics_.correctionPpm.store(0);
                    checkHr(renderClient_->ReleaseBuffer(
                                    outputFrames, AUDCLNT_BUFFERFLAGS_SILENT),
                            "Release WASAPI phase-align silence");
                    return;
                }
            }
            rebuffering_ = false;
            rebufferDelayArmed_ = false;
            starvationFrames_ = 0;
            smoothedQueue_ = legacy_
                    ? available
                    : std::max<double>(
                            0.0, static_cast<double>(available - outputFrames));
            integralError_ = 0;
            fadeIn_ = true;
            metrics_.rebuffering.store(false);
        }

        double callbackSeconds =
                static_cast<double>(outputFrames) / kSampleRate;
        double smoothing =
                1.0 - std::exp(-callbackSeconds / 0.5);
        const double observedQueue = legacy_
                ? available
                : available > outputFrames ? available - outputFrames : 0;
        const double controlTarget = legacy_
                ? targetFrames_
                : targetFrames_ + burstFrames_ * 0.5;
        if (smoothedQueue_ == 0) {
            smoothedQueue_ = observedQueue;
        } else {
            smoothedQueue_ += smoothing * (observedQueue - smoothedQueue_);
        }
        double normalizedError =
                (smoothedQueue_ - controlTarget)
                / std::max<double>(controlTarget, 1);
        integralError_ = std::clamp(
                integralError_ + normalizedError * callbackSeconds,
                -1.0, 1.0);
        const double correctionLimit = legacy_ ? 0.005 : 0.001;
        double correction = std::clamp(
                normalizedError * 0.004 + integralError_ * 0.0005,
                -correctionLimit, correctionLimit);
        metrics_.correctionPpm.store(static_cast<int32_t>(correction * 1'000'000));

        double exactInput = outputFrames * (1.0 + correction) + sourceFraction_;
        uint32_t inputFrames = std::max<uint32_t>(
                1, static_cast<uint32_t>(std::floor(exactInput)));
        sourceFraction_ = exactInput - inputFrames;
        inputFrames = std::min<uint32_t>(
                inputFrames, static_cast<uint32_t>(sourceBuffer_.size() / 2));

        uint32_t received = ring_.pop(sourceBuffer_.data(), inputFrames);
        if (received < inputFrames) {
            uint32_t shortage = inputFrames - received;
            uint32_t concealBudget = kSampleRate * 3 / 1000;
            uint32_t remainingBudget = starvationFrames_ < concealBudget
                    ? concealBudget - starvationFrames_ : 0;
            uint32_t concealed = std::min(shortage, remainingBudget);
            if (concealed) {
                metrics_.concealedFrames.fetch_add(concealed);
            }
            starvationFrames_ += shortage;
            if (starvationFrames_ > concealBudget && !rebuffering_) {
                metrics_.hardUnderruns.fetch_add(1);
                metrics_.rebufferEvents.fetch_add(1);
                metrics_.rebuffering.store(true);
                rebuffering_ = true;
                rebufferDelayArmed_ = false;
            }
            int16_t left = received ? sourceBuffer_[(received - 1) * 2] : lastLeft_;
            int16_t right = received ? sourceBuffer_[(received - 1) * 2 + 1] : lastRight_;
            for (uint32_t i = 0; i < shortage; ++i) {
                double gain = i < concealed
                        ? 1.0 - static_cast<double>(i + 1) / (concealed + 1)
                        : 0.0;
                sourceBuffer_[(received + i) * 2] =
                        static_cast<int16_t>(left * gain);
                sourceBuffer_[(received + i) * 2 + 1] =
                        static_cast<int16_t>(right * gain);
            }
        } else {
            starvationFrames_ = 0;
        }

        auto* out = reinterpret_cast<int16_t*>(destination);
        if (outputFrames == 1 || inputFrames == 1) {
            for (uint32_t frame = 0; frame < outputFrames; ++frame) {
                out[frame * 2] = sourceBuffer_[0];
                out[frame * 2 + 1] = sourceBuffer_[1];
            }
        } else {
            for (uint32_t frame = 0; frame < outputFrames; ++frame) {
                double position = static_cast<double>(frame)
                        * (inputFrames - 1) / (outputFrames - 1);
                uint32_t first = static_cast<uint32_t>(position);
                uint32_t second = std::min(first + 1, inputFrames - 1);
                double fraction = position - first;
                for (uint32_t channel = 0; channel < 2; ++channel) {
                    double a = sourceBuffer_[first * 2 + channel];
                    double b = sourceBuffer_[second * 2 + channel];
                    out[frame * 2 + channel] =
                            static_cast<int16_t>(a + (b - a) * fraction);
                }
            }
        }
        if (fadeIn_) {
            uint32_t fadeFrames = std::min<uint32_t>(outputFrames, 64);
            for (uint32_t frame = 0; frame < fadeFrames; ++frame) {
                double gain = static_cast<double>(frame + 1) / fadeFrames;
                out[frame * 2] =
                        static_cast<int16_t>(out[frame * 2] * gain);
                out[frame * 2 + 1] =
                        static_cast<int16_t>(out[frame * 2 + 1] * gain);
            }
            fadeIn_ = false;
        }
        lastLeft_ = out[(outputFrames - 1) * 2];
        lastRight_ = out[(outputFrames - 1) * 2 + 1];
        metrics_.queueFrames.store(ring_.available());
        checkHr(renderClient_->ReleaseBuffer(outputFrames, 0),
                "Release WASAPI render buffer");

        if (!metricsArmed_) {
            renderedFrames_ += outputFrames;
            if (renderedFrames_ >= kSampleRate) {
                metrics_.resetSessionCounters();
                metricsArmed_ = true;
            }
        }
    }

    void close() {
        if (renderClient_) renderClient_->Release();
        if (client_) client_->Release();
        if (device_) device_->Release();
        if (enumerator_) enumerator_->Release();
        if (event_) CloseHandle(event_);
        if (comInitialized_) CoUninitialize();
        renderClient_ = nullptr;
        client_ = nullptr;
        device_ = nullptr;
        enumerator_ = nullptr;
        event_ = nullptr;
        comInitialized_ = false;
    }

    PcmRingBuffer& ring_;
    RuntimeMetrics& metrics_;
    uint32_t targetFrames_;
    bool shared_;
    uint32_t burstFrames_;
    bool legacy_;
    bool outputFloat_ = false;
    bool comInitialized_ = false;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    HANDLE event_ = nullptr;
    std::wstring endpointId_;
    uint32_t periodFrames_ = 0;
    uint32_t bufferFrames_ = 0;
    REFERENCE_TIME streamLatencyHns_ = 0;
    std::string initializationPath_;
    std::vector<int16_t> sourceBuffer_;
    double sourceFraction_ = 0;
    double smoothedQueue_ = 0;
    double integralError_ = 0;
    uint32_t starvationFrames_ = 0;
    bool rebuffering_ = false;
    bool rebufferDelayArmed_ = false;
    std::chrono::steady_clock::time_point rebufferReadyAt_{};
    bool fadeIn_ = false;
    bool metricsArmed_ = false;
    uint64_t renderedFrames_ = 0;
    int16_t lastLeft_ = 0;
    int16_t lastRight_ = 0;
};

std::vector<ArrivalEvent> measure(SOCKET socket) {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    }
    std::vector<ArrivalEvent> events;
    events.reserve(5000);
    std::vector<int16_t> pcm;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 12s) {
        PacketHeader packet = receivePacket(socket, pcm);
        auto now = std::chrono::steady_clock::now();
        if (now - start >= 2s) {
            events.push_back({
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch()).count(),
                packet.frameCount
            });
        }
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    return events;
}

fs::path cachePath(const std::string& key) {
    const char* local = std::getenv("LOCALAPPDATA");
    fs::path directory = local
            ? fs::path(local) / "FastAudio" / "qualifications"
            : executableDirectory() / "qualifications";
    fs::create_directories(directory);
    std::ostringstream name;
    name << std::hex << std::hash<std::string>{}(key) << ".txt";
    return directory / name.str();
}

void saveQualification(const std::string& key,
                       const QualificationResult& result) {
    std::ofstream file(cachePath(key), std::ios::trunc);
    file << key << '\n' << std::setprecision(4) << result.targetMs
         << '\n' << result.estimatedLatencyMs
         << '\n' << result.grade << '\n';
}

std::optional<QualificationResult> loadQualification(const std::string& key) {
    std::ifstream file(cachePath(key));
    std::string storedKey;
    QualificationResult result;
    if (!std::getline(file, storedKey) || storedKey != key
            || !(file >> result.targetMs
                      >> result.estimatedLatencyMs
                      >> result.grade)) {
        return std::nullopt;
    }
    result.accepted = result.grade != "Unsupported";
    return result;
}

std::string qualificationKey(const std::string& fingerprint,
                             const std::string& packageName,
                             const WasapiRenderer& renderer, bool shared,
                             uint32_t packetFrames, bool legacy) {
    if (legacy) {
        return fingerprint + "|" + packageName + "|" + renderer.endpointId()
                + "|" + (shared ? "shared" : "exclusive")
                + "|" + std::to_string(renderer.periodFrames())
                + "|protocol=" + std::to_string(kProtocolVersion)
                + "|qualifier=3";
    }
    return fingerprint + "|" + packageName + "|" + renderer.endpointId()
            + "|" + (shared ? "shared" : "exclusive")
            + "|period=" + std::to_string(renderer.periodFrames())
            + "|buffer=" + std::to_string(renderer.bufferFrames())
            + "|path=" + renderer.initializationPath()
            + "|endpoint-us=" + std::to_string(static_cast<int64_t>(
                    std::llround(renderer.endpointLatencyMs() * 1000.0)))
            + "|packet=" + std::to_string(packetFrames)
            + "|controller=burst-v1"
            + "|protocol=" + std::to_string(kProtocolVersion)
            + "|qualifier=7";
}

void printQualification(const QualificationResult& result) {
    if (!result.accepted) {
        std::cout << "Qualification: Unsupported\n";
        return;
    }
    std::cout << "Qualification: " << result.grade
              << ", locked reserve=" << std::fixed << std::setprecision(1)
              << result.targetMs << "ms"
              << ", estimated floor=" << result.estimatedLatencyMs << "ms";
    if (result.details.maxDeficitFrames) {
        std::cout << ", max simulated deficit="
                  << result.details.maxDeficitFrames << " frames";
    }
    std::cout << '\n';
}

QualificationResult performQualification(
        const fs::path& adb, const Options& options,
        const std::string& key, double phasePeriodMs,
        double endpointLatencyMs, double minimumReserveMs) {
    std::cout << "Qualifying: 2s warm-up + 10s measurement...\n";
    const uint32_t packetFrames = options.legacy
            ? kLegacyPacketFrames : kLowLatencyPacketFrames;
    AdbSession session(
            adb, options.serial, options.packageName, packetFrames);
    session.start();
    auto events = measure(session.socket());
    session.stop();
    double maxGapMs = 0;
    for (size_t i = 1; i < events.size(); ++i) {
        maxGapMs = std::max(
                maxGapMs,
                (events[i].arrivalNs - events[i - 1].arrivalNs) / 1'000'000.0);
    }
    std::cout << "Measured " << events.size()
              << " packets, maximum arrival gap=" << std::fixed
              << std::setprecision(2) << maxGapMs << "ms\n";
    QualificationResult result = options.legacy
            ? qualify(events, phasePeriodMs)
            : qualifyBurst(events, endpointLatencyMs,
                    packetFrames, minimumReserveMs);
    printQualification(result);
    if (result.accepted) {
        saveQualification(key, result);
    }
    return result;
}

void streamToRing(SOCKET socket, PcmRingBuffer& ring,
                  RuntimeMetrics& metrics) {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    std::vector<int16_t> pcm;
    uint64_t expectedSequence = 0;
    try {
        while (!gStop.load()) {
            PacketHeader packet = receivePacket(socket, pcm);
            if (packet.sequence != expectedSequence
                    || (packet.flags & kFlagDiscontinuity)) {
                metrics.resyncs.fetch_add(1);
            }
            expectedSequence = packet.sequence + 1;
            uint32_t accepted = ring.push(pcm.data(), packet.frameCount);
            metrics.receivedFrames.fetch_add(accepted);
            if (accepted < packet.frameCount) {
                metrics.droppedFrames.fetch_add(packet.frameCount - accepted);
            }
        }
    } catch (const std::exception& e) {
        if (!gStop.load()) {
            std::cerr << "Receiver stopped: " << e.what() << '\n';
            gStop.store(true);
        }
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        SetConsoleCtrlHandler(onConsoleSignal, TRUE);
        Options options = parseOptions(argc, argv);
        fs::path adb = findAdb(options.adb);

        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock)) {
            throw std::runtime_error("WSAStartup failed");
        }
        const uint32_t packetFrames = options.legacy
                ? kLegacyPacketFrames : kLowLatencyPacketFrames;

        if (options.command == "probe") {
            const std::optional<MicrophoneEndpoint> microphone =
                    selectMicrophone(options.microphone);
            AdbSession session(
                    adb, options.serial, options.packageName, packetFrames,
                    microphone.has_value());
            session.start();
            const Hello& hello = session.hello();
            std::cout << "Android capture ready: " << hello.sampleRate << "Hz, "
                      << hello.channels << "ch, s" << hello.bitsPerSample
                      << ", packet=" << hello.packetFrames << " frames\n"
                      << "Voice capture rule: "
                      << ((hello.capabilities & kCapabilityVoice)
                                  ? "accepted (OEM may still filter voice)"
                                  : "not available")
                      << "\nUID package filter: "
                      << ((hello.capabilities & kCapabilityUidFilter)
                                  ? "active"
                                  : "not supported by this ROM; usage-only capture")
                      << '\n';
            if (microphone) {
                std::cout << "PC microphone: " << narrow(microphone->name) << '\n'
                          << "Android game-mic injector: "
                          << (session.microphoneReady()
                                  ? "ready"
                                  : "unavailable; phone microphone will be used")
                          << '\n';
            }
            session.stop();
            WSACleanup();
            return 0;
        }

        PcmRingBuffer ring(kSampleRate * 2);
        RuntimeMetrics metrics;
        auto renderer = std::make_unique<WasapiRenderer>(
                ring, metrics, 24.0, options.shared,
                packetFrames, options.legacy);
        std::cout << "Windows endpoint: "
                  << (renderer->isShared() ? "shared" : "exclusive")
                  << " (" << renderer->initializationPath() << ")"
                  << ", period=" << renderer->periodFrames() << " frames/"
                  << std::fixed << std::setprecision(2)
                  << renderer->periodFrames() * 1000.0 / kSampleRate
                  << "ms, buffer=" << renderer->bufferFrames() << " frames/"
                  << renderer->bufferFrames() * 1000.0 / kSampleRate
                  << "ms, stream latency=" << renderer->streamLatencyMs()
                  << "ms, endpoint floor=" << renderer->endpointLatencyMs()
                  << "ms\n";

        AdbSession fingerprintSession(
                adb, options.serial, options.packageName, packetFrames);
        std::string fingerprint = fingerprintSession.fingerprint();
        std::string key = qualificationKey(
                fingerprint, options.packageName, *renderer,
                renderer->isShared(), packetFrames, options.legacy);

        QualificationResult qualification;
        auto cached = options.requalify || options.latencyMs
                ? std::nullopt : loadQualification(key);
        if (options.latencyMs) {
            qualification.accepted = true;
            qualification.targetMs = std::max(
                    *options.latencyMs, renderer->minimumReserveMs());
            qualification.estimatedLatencyMs =
                    qualification.targetMs
                    + (options.legacy
                            ? renderer->periodFrames() * 1000.0 / kSampleRate
                            : renderer->endpointLatencyMs()
                                    + packetFrames * 1000.0 / kSampleRate);
            qualification.grade = "Diagnostic override";
            if (qualification.targetMs > *options.latencyMs) {
                std::cout << "Endpoint catch-up buffer requires at least "
                          << qualification.targetMs << "ms reserve\n";
            }
            std::cout << "Using diagnostic reserve " << qualification.targetMs
                      << "ms, estimated floor="
                      << qualification.estimatedLatencyMs << "ms\n";
        } else if (options.command == "qualify" || !cached) {
            qualification = performQualification(
                    adb, options, key,
                    renderer->periodFrames() * 1000.0 / kSampleRate,
                    renderer->endpointLatencyMs(),
                    renderer->minimumReserveMs());
        } else {
            qualification = *cached;
            std::cout << "Using cached ";
            printQualification(qualification);
        }
        if (!qualification.accepted) {
            WSACleanup();
            return 4;
        }
        if (options.command == "qualify") {
            WSACleanup();
            return 0;
        }

        renderer->setTargetMs(qualification.targetMs);

        const std::optional<MicrophoneEndpoint> microphone =
                selectMicrophone(options.microphone);
        if (microphone) {
            std::cout << "Selected PC microphone: " << narrow(microphone->name) << '\n';
        }

        AdbSession session(
                adb, options.serial, options.packageName, packetFrames,
                microphone.has_value());
        session.start();

        std::unique_ptr<MonoPcmRingBuffer> microphoneRing;
        std::unique_ptr<WasapiMicrophoneCapture> microphoneCapture;
        MicrophoneMetrics microphoneMetrics;
        std::atomic<bool> microphoneStop{false};
        std::atomic<bool> microphoneFinished{false};
        std::thread microphoneCaptureThread;
        std::thread microphoneSenderThread;
        bool microphoneRunning = false;
        if (microphone) {
            if (!session.microphoneReady()) {
                std::cerr << "PC microphone is unavailable on this Android firmware; "
                          << "continuing with phone microphone and FastAudio playback.\n";
                shutdown(session.microphoneSocket(), SD_SEND);
            } else {
                // 60 ms is enough to absorb endpoint scheduling variation but prevents
                // a stalled ADB socket from turning into a long-latency microphone queue.
                microphoneRing = std::make_unique<MonoPcmRingBuffer>(
                        kMicSampleRate * 60 / 1000);
                microphoneCapture = std::make_unique<WasapiMicrophoneCapture>(
                        microphone->id, *microphoneRing, microphoneMetrics,
                        microphoneStop, microphoneFinished);
                std::promise<void> microphoneReady;
                std::future<void> microphoneReadyFuture = microphoneReady.get_future();
                microphoneCaptureThread = std::thread(
                        [&capture = *microphoneCapture,
                         ready = std::move(microphoneReady)]() mutable {
                            capture.run(std::move(ready));
                        });
                try {
                    microphoneReadyFuture.get();
                    microphoneSenderThread = std::thread(
                            streamMicrophone, session.microphoneSocket(),
                            std::ref(*microphoneRing), std::ref(microphoneMetrics),
                            std::cref(microphoneFinished), std::ref(microphoneStop));
                    microphoneRunning = true;
                    std::cout << "Android microphone injection: ready for package UID "
                              << session.microphoneHello().packageUid
                              << " (use PUBG's normal team-mic button to talk)\n";
                } catch (const std::exception& error) {
                    std::cerr << "Selected PC microphone could not start: "
                              << error.what()
                              << ". Phone microphone remains available; playback continues.\n";
                    microphoneStop.store(true);
                    shutdown(session.microphoneSocket(), SD_SEND);
                    if (microphoneCaptureThread.joinable()) {
                        microphoneCaptureThread.join();
                    }
                }
            }
        }

        std::thread receiver(streamToRing, session.socket(),
                             std::ref(ring), std::ref(metrics));
        std::thread monitor([&]() {
            while (!gStop.load()) {
                std::this_thread::sleep_for(1s);
                if (options.diagnostics) {
                    std::cout
                        << "queue=" << metrics.queueFrames.load()
                                           * 1000.0 / kSampleRate << "ms"
                        << " correction=" << metrics.correctionPpm.load() << "ppm"
                        << " concealed=" << metrics.concealedFrames.load()
                        << " hardUnderruns=" << metrics.hardUnderruns.load()
                        << " rebuffers=" << metrics.rebufferEvents.load()
                        << " state="
                        << (metrics.rebuffering.load() ? "rebuffering" : "playing")
                        << " dropped=" << metrics.droppedFrames.load()
                        << " resyncs=" << metrics.resyncs.load();
                    if (microphoneRunning) {
                        std::cout << " | micQueue="
                                  << microphoneMetrics.queueFrames.load()
                                             * 1000.0 / kMicSampleRate << "ms"
                                  << " micSent=" << microphoneMetrics.sentFrames.load()
                                  << " micDropped=" << microphoneMetrics.droppedFrames.load()
                                  << " micDiscontinuities="
                                  << microphoneMetrics.discontinuities.load();
                    }
                    std::cout << '\n';
                }
            }
        });

        std::cout << "FastAudio running at locked reserve "
                  << qualification.targetMs
                  << "ms (estimated floor "
                  << qualification.estimatedLatencyMs
                  << "ms). Press Ctrl+C to stop.\n";
        renderer->run();
        gStop.store(true);
        microphoneStop.store(true);
        if (microphoneSenderThread.joinable()) microphoneSenderThread.join();
        if (microphoneCaptureThread.joinable()) microphoneCaptureThread.join();
        session.stop();
        if (receiver.joinable()) receiver.join();
        if (monitor.joinable()) monitor.join();
        std::cout
            << "Session summary: concealed="
            << metrics.concealedFrames.load()
            << " hardUnderruns=" << metrics.hardUnderruns.load()
            << " rebuffers=" << metrics.rebufferEvents.load()
            << " dropped=" << metrics.droppedFrames.load()
            << " resyncs=" << metrics.resyncs.load();
        if (microphone) {
            std::cout << " | micCaptured=" << microphoneMetrics.capturedFrames.load()
                      << " micSent=" << microphoneMetrics.sentFrames.load()
                      << " micDropped=" << microphoneMetrics.droppedFrames.load()
                      << " micDiscontinuities="
                      << microphoneMetrics.discontinuities.load()
                      << " micState="
                      << (microphoneRunning && !microphoneMetrics.unavailable.load()
                                  ? "closed-cleanly" : "phone-mic-fallback");
        }
        std::cout << '\n';
        WSACleanup();
        return metrics.hardUnderruns.load() ? 5 : 0;
    } catch (const std::exception& e) {
        std::cerr << "FastAudio: " << e.what() << '\n';
        printUsage();
        return 1;
    }
}
