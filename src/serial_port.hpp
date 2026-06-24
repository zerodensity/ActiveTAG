#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace activetag {

struct PortInfo {
    std::wstring path;
    std::wstring displayName;
};

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    void open(const std::wstring& path);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const std::wstring& path() const;

    std::string command(const std::string& command, DWORD timeoutMs = 4000);
    bool probe(const std::string& command, DWORD timeoutMs = 1000);
    void setLogCallback(std::function<void(const std::string&)> callback);

    static std::vector<PortInfo> enumerate();

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::function<void(const std::string&)> logCallback_;
};

}  // namespace activetag
