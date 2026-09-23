#include "auto_update.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace activetag::update {
namespace {

constexpr wchar_t kGitHubApiHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] = L"/repos/zerodensity/ActiveTAG/releases/latest";
constexpr wchar_t kUserAgent[] = L"ZeroDensity-ActiveTAG-Configurator-Updater";

struct InternetHandleCloser {
    void operator()(void* handle) const {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};
using InternetHandle = std::unique_ptr<void, InternetHandleCloser>;

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("The update URL contains invalid UTF-8.");
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string windowsError(const char* action) {
    return std::string(action) + " Windows error: " + std::to_string(GetLastError());
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

UrlParts crackUrl(const std::string& url) {
    const std::wstring wideUrl = utf8ToWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        throw std::runtime_error(windowsError("The update URL could not be parsed."));
    }
    UrlParts result;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    result.port = parts.nPort;
    result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!result.secure) {
        throw std::runtime_error("Update downloads require HTTPS.");
    }
    return result;
}

InternetHandle openSession() {
    InternetHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        throw std::runtime_error(windowsError("The update network session could not be opened."));
    }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
    return session;
}

struct HttpResponse {
    InternetHandle connection;
    InternetHandle request;
    unsigned long long contentLength = 0;
};

HttpResponse openGet(void* session, const UrlParts& url, const wchar_t* accept) {
    InternetHandle connection(WinHttpConnect(session, url.host.c_str(), url.port, 0));
    if (!connection) {
        throw std::runtime_error(windowsError("The update server could not be reached."));
    }
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", url.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        url.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw std::runtime_error(windowsError("The update request could not be created."));
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
        &redirectPolicy, sizeof(redirectPolicy));
    const std::wstring headers = std::wstring(L"Accept: ") + accept +
        L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(windowsError("The update request failed."));
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        throw std::runtime_error("The update server returned HTTP " + std::to_string(status) + ".");
    }
    unsigned long long length = 0;
    wchar_t lengthBuffer[64]{};
    DWORD lengthSize = sizeof(lengthBuffer);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, lengthBuffer, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
        try {
            length = std::stoull(lengthBuffer);
        } catch (...) {
            length = 0;
        }
    }
    return {std::move(connection), std::move(request), length};
}

std::string readResponse(void* request, std::atomic<unsigned long long>* downloaded = nullptr,
    std::ofstream* output = nullptr) {
    std::string body;
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            throw std::runtime_error(windowsError("The update response could not be read."));
        }
        if (read == 0) {
            break;
        }
        if (output != nullptr) {
            output->write(buffer.data(), read);
            if (!*output) {
                throw std::runtime_error("The update could not be written to disk.");
            }
        } else {
            body.append(buffer.data(), read);
        }
        if (downloaded != nullptr) {
            downloaded->fetch_add(read);
        }
    }
    return body;
}

std::string getText(const std::string& url, const wchar_t* accept) {
    auto session = openSession();
    auto response = openGet(session.get(), crackUrl(url), accept);
    return readResponse(response.request.get());
}

std::filesystem::path currentExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error(windowsError("The application path could not be determined."));
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::wstring argumentValue(int count, wchar_t** arguments, const std::wstring& name) {
    for (int index = 1; index + 1 < count; ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return {};
}

}  // namespace

bool parseVersion(const std::string& text, Version& version) {
    std::string value = text;
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    std::istringstream input(value);
    char dot1 = 0;
    char dot2 = 0;
    Version parsed;
    if (!(input >> parsed.major >> dot1 >> parsed.minor >> dot2 >> parsed.patch) ||
        dot1 != '.' || dot2 != '.' || parsed.major < 0 || parsed.minor < 0 || parsed.patch < 0) {
        return false;
    }
    char trailing = 0;
    if (input >> trailing) {
        return false;
    }
    version = parsed;
    return true;
}

int compareVersions(const Version& left, const Version& right) {
    const auto leftTuple = std::array{left.major, left.minor, left.patch};
    const auto rightTuple = std::array{right.major, right.minor, right.patch};
    return leftTuple < rightTuple ? -1 : (leftTuple > rightTuple ? 1 : 0);
}

bool parseSha256File(const std::string& text, const std::wstring& expectedFileName,
    std::string& digest) {
    std::istringstream lines(text);
    std::string line;
    const std::string expected = wideToUtf8(expectedFileName);
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string candidate;
        std::string file;
        if (!(fields >> candidate)) {
            continue;
        }
        fields >> file;
        if (!file.empty() && file.front() == '*') {
            file.erase(file.begin());
        }
        if (!file.empty() && file != expected) {
            continue;
        }
        if (candidate.size() != 64 || !std::all_of(candidate.begin(), candidate.end(),
                [](unsigned char character) { return std::isxdigit(character) != 0; })) {
            continue;
        }
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        digest = candidate;
        return true;
    }
    return false;
}

Release fetchLatestRelease() {
    const std::string endpoint = "https://" + wideToUtf8(kGitHubApiHost) +
        wideToUtf8(kLatestReleasePath);
    const auto document = nlohmann::json::parse(
        getText(endpoint, L"application/vnd.github+json"));
    Release release;
    release.tag = document.at("tag_name").get<std::string>();
    release.notes = document.value("body", std::string{});
    if (!parseVersion(release.tag, release.version)) {
        throw std::runtime_error("The latest GitHub release has an invalid version tag.");
    }
    const std::string canonicalExecutable = "ActiveTAG-Configurator.exe";
    const std::string canonicalChecksum = canonicalExecutable + ".sha256";
    const std::string legacyExecutable = "ActiveTAG-Configurator-" + release.tag + ".exe";
    const std::string legacyChecksum = legacyExecutable + ".sha256";
    std::string canonicalExecutableUrl;
    std::string canonicalChecksumUrl;
    std::string legacyExecutableUrl;
    std::string legacyChecksumUrl;
    for (const auto& asset : document.at("assets")) {
        const std::string name = asset.at("name").get<std::string>();
        const std::string url = asset.at("browser_download_url").get<std::string>();
        if (name == canonicalExecutable) {
            canonicalExecutableUrl = url;
        } else if (name == canonicalChecksum) {
            canonicalChecksumUrl = url;
        } else if (name == legacyExecutable) {
            legacyExecutableUrl = url;
        } else if (name == legacyChecksum) {
            legacyChecksumUrl = url;
        }
    }
    if (!canonicalExecutableUrl.empty() && !canonicalChecksumUrl.empty()) {
        release.executableName = utf8ToWide(canonicalExecutable);
        release.executableUrl = canonicalExecutableUrl;
        release.checksumUrl = canonicalChecksumUrl;
    } else if (!legacyExecutableUrl.empty() && !legacyChecksumUrl.empty()) {
        release.executableName = utf8ToWide(legacyExecutable);
        release.executableUrl = legacyExecutableUrl;
        release.checksumUrl = legacyChecksumUrl;
    }
    if (release.executableUrl.empty() || release.checksumUrl.empty()) {
        throw std::runtime_error(
            "The latest release is missing its EXE or SHA-256 checksum asset.");
    }
    return release;
}

void downloadFile(const std::string& url, const std::filesystem::path& destination,
    std::atomic<unsigned long long>& downloaded, std::atomic<unsigned long long>& total) {
    downloaded = 0;
    total = 0;
    auto session = openSession();
    auto response = openGet(session.get(), crackUrl(url), L"application/octet-stream");
    total = response.contentLength;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("The update file could not be created in the application folder.");
    }
    try {
        readResponse(response.request.get(), &downloaded, &output);
        output.close();
        if (!output) {
            throw std::runtime_error("The downloaded update could not be finalized.");
        }
    } catch (...) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        throw;
    }
}

std::string sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD bytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0) < 0) {
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 verification could not be initialized.");
    }
    std::vector<unsigned char> object(objectSize);
    std::array<unsigned char, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 verification could not be initialized.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("The downloaded update could not be opened for verification.");
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("SHA-256 verification failed.");
        }
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 verification failed.");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) {
        output << std::setw(2) << static_cast<int>(byte);
    }
    return output.str();
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool startUpdater(const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable, const std::filesystem::path& oldExecutable,
    const std::string& expectedSha256, unsigned long processId, std::string& error) {
    try {
        const auto current = currentExecutablePath();
        const auto helper = std::filesystem::temp_directory_path() /
            (L"ActiveTAG-Updater-" + std::to_wstring(processId) + L".exe");
        std::filesystem::copy_file(current, helper, std::filesystem::copy_options::overwrite_existing);
        std::wstring command = quoteArgument(helper.wstring()) +
            L" --apply-update --wait-pid " + std::to_wstring(processId) +
            L" --source " + quoteArgument(downloadedExecutable.wstring()) +
            L" --target " + quoteArgument(targetExecutable.wstring()) +
            L" --old " + quoteArgument(oldExecutable.wstring()) +
            L" --sha256 " + quoteArgument(utf8ToWide(expectedSha256));
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(helper.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, targetExecutable.parent_path().c_str(),
                &startup, &process)) {
            throw std::runtime_error(windowsError("The updater helper could not be started."));
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

int runUpdaterMode(int argumentCount, wchar_t** arguments) {
    const std::wstring pidText = argumentValue(argumentCount, arguments, L"--wait-pid");
    const std::filesystem::path source = argumentValue(argumentCount, arguments, L"--source");
    const std::filesystem::path target = argumentValue(argumentCount, arguments, L"--target");
    const std::filesystem::path old = argumentValue(argumentCount, arguments, L"--old");
    const std::string expectedSha256 = wideToUtf8(
        argumentValue(argumentCount, arguments, L"--sha256"));
    if (pidText.empty() || source.empty() || target.empty() || expectedSha256.size() != 64) {
        return 2;
    }
    try {
        const DWORD pid = static_cast<DWORD>(std::stoul(pidText));
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
        if (sha256File(source) != expectedSha256) {
            MessageBoxW(nullptr,
                L"The downloaded update changed after verification. The current version was preserved.",
                L"ActiveTAG Update Failed", MB_OK | MB_ICONERROR);
            DeleteFileW(source.c_str());
            return 3;
        }

        std::filesystem::path backup = target;
        backup += L".update-backup";
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
        const bool hadTarget = std::filesystem::exists(target);
        if (hadTarget && !MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            MessageBoxW(nullptr, L"The existing application could not be backed up.",
                L"ActiveTAG Update Failed", MB_OK | MB_ICONERROR);
            return 4;
        }
        if (!MoveFileExW(source.c_str(), target.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            if (hadTarget) {
                MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            MessageBoxW(nullptr, L"The downloaded update could not replace the application file.",
                L"ActiveTAG Update Failed", MB_OK | MB_ICONERROR);
            return 5;
        }
        std::wstring command = quoteArgument(target.wstring());
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(target.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                nullptr, target.parent_path().c_str(), &startup, &process)) {
            DeleteFileW(target.c_str());
            if (hadTarget) {
                MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            MessageBoxW(nullptr, L"The update was installed, but the new version could not start.",
                L"ActiveTAG Update", MB_OK | MB_ICONWARNING);
            return 6;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (hadTarget) {
            DeleteFileW(backup.c_str());
        }
        if (!old.empty() && old != target) {
            DeleteFileW(old.c_str());
        }
        const auto self = currentExecutablePath();
        MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return 0;
    } catch (...) {
        MessageBoxW(nullptr, L"The update could not be installed.",
            L"ActiveTAG Update Failed", MB_OK | MB_ICONERROR);
        return 7;
    }
}

}  // namespace activetag::update
