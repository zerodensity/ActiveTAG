#include "serial_port.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace activetag {
namespace {

std::string windowsError(const char* operation) {
    const DWORD code = GetLastError();
    return std::string(operation) + " failed. Windows error: " + std::to_string(code);
}

}  // namespace

SerialPort::~SerialPort() {
    close();
}

void SerialPort::open(const std::wstring& path) {
    close();

    std::wstring devicePath = path;
    if (!devicePath.starts_with(LR"(\\.\)")) {
        devicePath = LR"(\\.\)" + devicePath;
    }

    handle_ = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(windowsError("Opening serial port"));
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle_, &dcb)) {
        close();
        throw std::runtime_error(windowsError("Reading serial settings"));
    }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(handle_, &dcb)) {
        close();
        throw std::runtime_error(windowsError("Configuring serial port"));
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 40;
    timeouts.ReadTotalTimeoutConstant = 80;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(handle_, &timeouts)) {
        close();
        throw std::runtime_error(windowsError("Configuring serial timeouts"));
    }

    SetupComm(handle_, 4096, 4096);
    PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    path_ = path;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void SerialPort::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    path_.clear();
}

bool SerialPort::isOpen() const {
    return handle_ != INVALID_HANDLE_VALUE;
}

const std::wstring& SerialPort::path() const {
    return path_;
}

std::string SerialPort::command(const std::string& value, DWORD timeoutMs) {
    if (!isOpen()) {
        throw std::runtime_error("Active Tag is not connected.");
    }

    PurgeComm(handle_, PURGE_RXCLEAR);
    const std::string payload = value + "\r";
    DWORD written = 0;
    if (!WriteFile(handle_, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) ||
        written != payload.size()) {
        throw std::runtime_error(windowsError("Writing serial command"));
    }

    std::string response;
    char buffer[512];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        DWORD bytesRead = 0;
        if (!ReadFile(handle_, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            throw std::runtime_error(windowsError("Reading serial response"));
        }
        if (bytesRead > 0) {
            response.append(buffer, bytesRead);
            const auto lastNonSpace = response.find_last_not_of(" \t\r\n");
            if (lastNonSpace != std::string::npos && response[lastNonSpace] == '>') {
                return response;
            }
        }
    }

    throw std::runtime_error("Serial command timed out: " + value);
}

std::vector<PortInfo> SerialPort::enumerate() {
    std::vector<PortInfo> ports;
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS) {
        return ports;
    }

    DWORD index = 0;
    wchar_t valueName[256];
    BYTE data[256];
    while (true) {
        DWORD valueNameSize = static_cast<DWORD>(std::size(valueName));
        DWORD dataSize = sizeof(data);
        DWORD type = 0;
        const LONG result = RegEnumValueW(
            key,
            index++,
            valueName,
            &valueNameSize,
            nullptr,
            &type,
            data,
            &dataSize);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result == ERROR_SUCCESS && type == REG_SZ) {
            const auto* portName = reinterpret_cast<const wchar_t*>(data);
            ports.push_back({portName, std::wstring(portName) + L" - USB Serial Device"});
        }
    }
    RegCloseKey(key);

    std::sort(ports.begin(), ports.end(), [](const PortInfo& left, const PortInfo& right) {
        return left.path < right.path;
    });
    return ports;
}

}  // namespace activetag
