#include "active_tag.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace activetag {
namespace {

const std::set<std::string> documentedFirmware2Fields = {
    "2", "3", "4", "5", "6", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};

constexpr long long disabledLedWriteValue = 0xFFFFFFFFLL;
constexpr long long disabledLedLegacyValue = 0x7FFFFFFFLL;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<long long> parseNumber(const std::string& value) {
    std::smatch match;
    if (std::regex_search(value, match, std::regex(R"(0x([0-9a-fA-F]+))"))) {
        return std::stoll(match[1].str(), nullptr, 16);
    }
    if (std::regex_search(value, match, std::regex(R"((-?\d+))"))) {
        return std::stoll(match[1].str(), nullptr, 10);
    }
    return std::nullopt;
}

bool isDisabledLedValue(long long value) {
    return value == disabledLedWriteValue || value == disabledLedLegacyValue;
}

bool fieldMatchesExpectedLed(long long actual, long long expected) {
    if (isDisabledLedValue(expected)) {
        return isDisabledLedValue(actual);
    }
    return actual == expected;
}

}  // namespace

void ActiveTag::connect(const std::wstring& portPath) {
    serial_.open(portPath);
    const Snapshot snapshot = read();
    if (!isActiveTag(snapshot)) {
        disconnect();
        throw std::runtime_error("The selected serial device is not an OptiTrack Active Tag.");
    }
}

void ActiveTag::disconnect() {
    serial_.close();
}

bool ActiveTag::isConnected() const {
    return serial_.isOpen();
}

const std::wstring& ActiveTag::portPath() const {
    return serial_.path();
}

Snapshot ActiveTag::read() {
    return parseDump(serial_.command("d", 5000));
}

void ActiveTag::setLogCallback(std::function<void(const std::string&)> callback) {
    serial_.setLogCallback(std::move(callback));
}

std::pair<Snapshot, std::vector<Change>> ActiveTag::apply(
    const std::map<std::string, long long>& requested) {
    const Snapshot before = read();
    std::vector<Change> changes;

    for (const auto& [id, target] : requested) {
        const auto fieldIt = before.fields.find(id);
        if (fieldIt == before.fields.end() || !fieldIt->second.supported) {
            throw std::runtime_error("Field [" + id + "] is not writable on this device.");
        }
        if (target < 0) {
            throw std::runtime_error("Negative values are not accepted for [" + id + "].");
        }
        if (id == "3" && (target < 11 || target > 26)) {
            throw std::runtime_error("RF Channel must be between 11 and 26.");
        }
        if (id == "5" && target != 0 && target != 1) {
            throw std::runtime_error("On While Charging accepts only 0 or 1.");
        }
        if (!fieldIt->second.hasNumericValue || fieldIt->second.numericValue != target) {
            changes.push_back({id, fieldIt->second.numericValue, target});
        }
    }

    for (const Change& change : changes) {
        serial_.command("s " + change.id + " " + std::to_string(change.after));
    }

    const Snapshot staged = read();
    for (const Change& change : changes) {
        const auto fieldIt = staged.fields.find(change.id);
        if (fieldIt == staged.fields.end() || !fieldIt->second.hasNumericValue ||
            fieldIt->second.numericValue != change.after) {
            throw std::runtime_error(
                "Verification failed for [" + change.id + "]. Flash was not saved.");
        }
    }

    if (!changes.empty()) {
        serial_.command("v", 5000);
    }

    const Snapshot saved = read();
    for (const Change& change : changes) {
        const auto fieldIt = saved.fields.find(change.id);
        if (fieldIt == saved.fields.end() || !fieldIt->second.hasNumericValue ||
            fieldIt->second.numericValue != change.after) {
            throw std::runtime_error("Saved value could not be verified for [" + change.id + "].");
        }
    }
    return {saved, changes};
}

Snapshot ActiveTag::parseDump(const std::string& raw) {
    Snapshot snapshot;
    snapshot.raw = raw;

    std::smatch firmwareMatch;
    if (std::regex_search(
            raw,
            firmwareMatch,
            std::regex(R"(FIRMWARE VERSION\s*:\s*([^\s]+))", std::regex::icase))) {
        snapshot.firmwareVersion = firmwareMatch[1].str();
    }

    std::istringstream lines(raw);
    std::string line;
    const std::regex fieldPattern(R"(^\s*\[([^\]]+)\]\s+([^:]+?)\s*:\s*(.*?)\s*$)");
    while (std::getline(lines, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, fieldPattern)) {
            continue;
        }

        std::string id = trim(match[1].str());
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        const std::string name = trim(match[2].str());
        const std::string rawValue = trim(match[3].str());

        if (id == "-") {
            snapshot.metadata[name] = rawValue;
            continue;
        }

        const bool unsupported =
            name.find("unsupported") != std::string::npos ||
            rawValue.find("(N/A)") != std::string::npos;
        ConfigField field;
        field.id = id;
        field.name = name;
        field.rawValue = rawValue;
        field.supported = !unsupported;
        field.documented = documentedFirmware2Fields.contains(id);
        if (const auto number = parseNumber(rawValue)) {
            field.numericValue = *number;
            field.hasNumericValue = true;
        }
        snapshot.fields[id] = field;
    }

    snapshot.detectedLabelGroup = detectLabelGroup(snapshot);
    snapshot.detectedTalentTrackGroup = detectTalentTrackGroup(snapshot);
    snapshot.detectedLensProfile = detectLensProfile(snapshot);
    return snapshot;
}

bool ActiveTag::isActiveTag(const Snapshot& snapshot) {
    return !snapshot.firmwareVersion.empty() &&
           snapshot.metadata.contains("serialNum") &&
           snapshot.fields.contains("D0") &&
           snapshot.fields.contains("D7");
}

std::optional<int> ActiveTag::detectLabelGroup(const Snapshot& snapshot) {
    for (int group = 0; group < static_cast<int>(labelGroups().size()); ++group) {
        bool matches = true;
        for (int led = 0; led < 8; ++led) {
            const auto fieldIt = snapshot.fields.find("D" + std::to_string(led));
            if (fieldIt == snapshot.fields.end() ||
                !fieldIt->second.hasNumericValue ||
                !fieldMatchesExpectedLed(fieldIt->second.numericValue, labelGroups()[group][led])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return group;
        }
    }
    return std::nullopt;
}

std::optional<int> ActiveTag::detectTalentTrackGroup(const Snapshot& snapshot) {
    const auto uplinkIt = snapshot.fields.find("2");
    if (uplinkIt == snapshot.fields.end() || !uplinkIt->second.hasNumericValue) {
        return std::nullopt;
    }

    for (int index = 0; index < static_cast<int>(talentTrackGroups().size()); ++index) {
        const int group = index + 6;
        if (uplinkIt->second.numericValue != group) {
            continue;
        }

        bool matches = true;
        for (int led = 0; led < 8; ++led) {
            const auto fieldIt = snapshot.fields.find("D" + std::to_string(led));
            if (fieldIt == snapshot.fields.end() ||
                !fieldIt->second.hasNumericValue ||
                !fieldMatchesExpectedLed(fieldIt->second.numericValue, talentTrackGroups()[index][led])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return group;
        }
    }
    return std::nullopt;
}

std::optional<int> ActiveTag::detectLensProfile(const Snapshot& snapshot) {
    const auto uplinkIt = snapshot.fields.find("2");
    if (uplinkIt == snapshot.fields.end() || !uplinkIt->second.hasNumericValue) {
        return std::nullopt;
    }

    for (int index = 0; index < static_cast<int>(lensProfiles().size()); ++index) {
        const int expectedUplink = index + 21;
        if (uplinkIt->second.numericValue != expectedUplink) {
            continue;
        }

        bool matches = true;
        for (int led = 0; led < 8; ++led) {
            const auto fieldIt = snapshot.fields.find("D" + std::to_string(led));
            if (fieldIt == snapshot.fields.end() ||
                !fieldIt->second.hasNumericValue ||
                !fieldMatchesExpectedLed(fieldIt->second.numericValue, lensProfiles()[index][led])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return index;
        }
    }
    return std::nullopt;
}

const std::array<std::array<long long, 8>, 6>& ActiveTag::labelGroups() {
    static const std::array<std::array<long long, 8>, 6> groups = {{
        {{1, 2, 4, 8, 16, 32, 64, 128}},
        {{256, 512, 1024, 2048, 65, 130, 260, 520}},
        {{1040, 2080, 33, 66, 129, 132, 258, 264}},
        {{516, 528, 1032, 1056, 2064, 2112, 17, 34}},
        {{68, 136, 257, 272, 514, 544, 1028, 1088}},
        {{2056, 2176, 9, 18, 36, 72, 144, 288}}
    }};
    return groups;
}

const std::array<std::array<long long, 8>, 15>& ActiveTag::talentTrackGroups() {
    constexpr long long disabled = disabledLedWriteValue;
    static const std::array<std::array<long long, 8>, 15> groups = {{
        {{disabled, disabled, disabled, 0x804, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x50, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x2008, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x424, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x908, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x2110, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x248, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x920, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x2090, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x228, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0xA08, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x2088, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x128, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0xA04, disabled, disabled, disabled, disabled}},
        {{disabled, disabled, disabled, 0x2048, disabled, disabled, disabled, disabled}}
    }};
    return groups;
}

const std::array<std::array<long long, 8>, 2>& ActiveTag::lensProfiles() {
    constexpr long long disabled = disabledLedWriteValue;
    static const std::array<std::array<long long, 8>, 2> profiles = {{
        {{0x54, 0xA8, 0x150, 0x2A0, disabled, disabled, disabled, disabled}},
        {{0xA02, 0xA80, 0x249, 0x492, disabled, disabled, disabled, disabled}}
    }};
    return profiles;
}

}  // namespace activetag
