#pragma once

#include "serial_port.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace activetag {

struct ConfigField {
    std::string id;
    std::string name;
    std::string rawValue;
    long long numericValue = 0;
    bool hasNumericValue = false;
    bool supported = false;
    bool documented = false;
};

struct Snapshot {
    std::string raw;
    std::string firmwareVersion;
    std::map<std::string, std::string> metadata;
    std::map<std::string, ConfigField> fields;
    std::optional<int> detectedLabelGroup;
    std::optional<int> detectedTalentTrackGroup;
    std::optional<int> detectedLensProfile;
};

struct Change {
    std::string id;
    long long before = 0;
    long long after = 0;
};

class ActiveTag {
public:
    void connect(const std::wstring& portPath);
    void disconnect();
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] const std::wstring& portPath() const;

    Snapshot read();
    std::pair<Snapshot, std::vector<Change>> apply(
        const std::map<std::string, long long>& requested);
    void setLogCallback(std::function<void(const std::string&)> callback);

    static Snapshot parseDump(const std::string& raw);
    static bool isActiveTag(const Snapshot& snapshot);
    static std::optional<int> detectLabelGroup(const Snapshot& snapshot);
    static std::optional<int> detectTalentTrackGroup(const Snapshot& snapshot);
    static std::optional<int> detectLensProfile(const Snapshot& snapshot);
    static const std::array<std::array<long long, 8>, 6>& labelGroups();
    static const std::array<std::array<long long, 8>, 15>& talentTrackGroups();
    static const std::array<std::array<long long, 8>, 2>& lensProfiles();

private:
    SerialPort serial_;
};

}  // namespace activetag
