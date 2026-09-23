#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace activetag::update {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

struct Release {
    Version version;
    std::string tag;
    std::string notes;
    std::string executableUrl;
    std::string checksumUrl;
    std::wstring executableName;
};

bool parseVersion(const std::string& text, Version& version);
int compareVersions(const Version& left, const Version& right);
bool parseSha256File(const std::string& text, const std::wstring& expectedFileName,
    std::string& digest);

Release fetchLatestRelease();
void downloadFile(const std::string& url, const std::filesystem::path& destination,
    std::atomic<unsigned long long>& downloaded,
    std::atomic<unsigned long long>& total);
std::string sha256File(const std::filesystem::path& path);

std::wstring quoteArgument(const std::wstring& value);
bool startUpdater(const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& oldExecutable,
    const std::string& expectedSha256,
    unsigned long processId,
    std::string& error);
int runUpdaterMode(int argumentCount, wchar_t** arguments);

}  // namespace activetag::update
