#include "../src/active_tag.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

std::string talentTrackDump(int group, const std::array<long long, 8>& leds) {
    std::ostringstream dump;
    dump << "FIRMWARE VERSION    : 2.3.4 (compiled May  8 2023 15:29:25)\n"
         << "[-]  serialNum      : 29567\n"
         << "[-]  hardwareRev    : G\n"
         << "[2]  uplinkId       : " << group << "\n"
         << "[3]  rfChannel      : 20\n"
         << "[4]  ledBrightness  : 20\n"
         << "[5]  onWhileCharging: 1\n";
    for (int led = 0; led < 8; ++led) {
        dump << "[D" << led << "] led" << led << "Id : "
             << leds[led] << "\n";
    }
    return dump.str();
}

std::string cameraDump(int group, const std::array<long long, 8>& leds) {
    std::ostringstream dump;
    dump << "FIRMWARE VERSION    : 2.3.4 (compiled May  8 2023 15:29:25)\n"
         << "[-]  serialNum      : 29567\n"
         << "[-]  hardwareRev    : G\n"
         << "[2]  uplinkId       : " << group << "\n"
         << "[3]  rfChannel      : 15\n"
         << "[4]  ledBrightness  : 20\n"
         << "[5]  onWhileCharging: 1\n";
    for (int led = 0; led < 8; ++led) {
        dump << "[D" << led << "] led" << led << "Id : "
             << leds[led] << "\n";
    }
    return dump.str();
}

std::string lensProfileDump(int profileIndex, const std::array<long long, 8>& leds) {
    const int uplink = profileIndex + 22;
    std::ostringstream dump;
    dump << "FIRMWARE VERSION    : 2.3.4 (compiled May  8 2023 15:29:25)\n"
         << "[-]  serialNum      : 32501\n"
         << "[-]  hardwareRev    : G\n"
         << "[2]  uplinkId       : " << uplink << "\n"
         << "[3]  rfChannel      : 20\n"
         << "[6]  signalIntensity: 1\n"
         << "[4]  ledBrightness  : 20\n"
         << "[5]  onWhileCharging: 1\n";
    for (int led = 0; led < 8; ++led) {
        dump << "[D" << led << "] led" << led << "Id : "
             << leds[led] << "\n";
    }
    return dump.str();
}

int main() {
    if (activetag::ActiveTag::formatSerialValueForWrite(0xFFFFFFFFLL) != "-1") {
        std::cerr << "Disabled LED write value must be sent as -1 for firmware to store 0xFFFFFFFF.\n";
        return 1;
    }
    if (activetag::ActiveTag::formatSerialValueForWrite(2052) != "2052") {
        std::cerr << "Normal LED IDs should stay decimal in serial write commands.\n";
        return 1;
    }

    std::ifstream input("Active Putty Output.txt", std::ios::binary);
    if (!input) {
        std::cerr << "Fixture could not be opened.\n";
        return 1;
    }

    std::ostringstream content;
    content << input.rdbuf();
    const std::string text = content.str();
    const std::string marker = "FIRMWARE VERSION";
    std::vector<std::string> dumps;

    std::size_t position = text.find(marker);
    while (position != std::string::npos) {
        const std::size_t next = text.find(marker, position + marker.size());
        dumps.push_back(text.substr(position, next - position));
        position = next;
    }

    if (dumps.size() != 6) {
        std::cerr << "Expected 6 dumps, got " << dumps.size() << ".\n";
        return 1;
    }

    for (int index = 0; index < 6; ++index) {
        const auto snapshot = activetag::ActiveTag::parseDump(dumps[index]);
        if (!activetag::ActiveTag::isActiveTag(snapshot)) {
            std::cerr << "Dump " << index << " was not recognized as an Active Tag.\n";
            return 1;
        }
        if (snapshot.detectedLabelGroup) {
            std::cerr << "Old zero-based Camera dump " << index
                      << " was incorrectly treated as a current profile.\n";
            return 1;
        }
        if (snapshot.detectedTalentTrackGroup) {
            std::cerr << "Camera dump " << index << " was misidentified as Talent Track.\n";
            return 1;
        }
        if (snapshot.detectedLensProfile) {
            std::cerr << "Camera dump " << index << " was misidentified as Lens Profiling.\n";
            return 1;
        }
        if (snapshot.fields.at("1").supported) {
            std::cerr << "Unsupported field [1] was treated as writable.\n";
            return 1;
        }
        if (!snapshot.fields.at("D0").supported || !snapshot.fields.at("D7").supported) {
            std::cerr << "LED fields were not treated as writable.\n";
            return 1;
        }
    }

    const auto& cameraGroups = activetag::ActiveTag::labelGroups();
    std::set<int> reservedUplinks;
    std::set<long long> activeLedIds;
    auto rememberActiveLedId = [&](long long value) {
        if (value == 0xFFFFFFFFLL || value == 0x7FFFFFFFLL) {
            return true;
        }
        return activeLedIds.insert(value).second;
    };

    for (int index = 0; index < static_cast<int>(cameraGroups.size()); ++index) {
        const int group = index + 1;
        reservedUplinks.insert(group);
        const auto snapshot =
            activetag::ActiveTag::parseDump(cameraDump(group, cameraGroups[index]));
        if (!snapshot.detectedLabelGroup || *snapshot.detectedLabelGroup != group) {
            std::cerr << "Camera mismatch for Label Group " << group << ".\n";
            return 1;
        }
        if (snapshot.detectedTalentTrackGroup) {
            std::cerr << "Camera group " << group << " was misidentified as Talent Track.\n";
            return 1;
        }
        if (snapshot.detectedLensProfile) {
            std::cerr << "Camera group " << group << " was misidentified as Lens Profiling.\n";
            return 1;
        }
        for (const long long value : cameraGroups[index]) {
            if (!rememberActiveLedId(value)) {
                std::cerr << "Duplicate active LED ID in Camera Label Group "
                          << group << ".\n";
                return 1;
            }
        }
    }

    const auto& talentGroups = activetag::ActiveTag::talentTrackGroups();
    for (int index = 0; index < static_cast<int>(talentGroups.size()); ++index) {
        const int group = index + 7;
        reservedUplinks.insert(group);
        const auto snapshot =
            activetag::ActiveTag::parseDump(talentTrackDump(group, talentGroups[index]));
        if (!snapshot.detectedTalentTrackGroup ||
            *snapshot.detectedTalentTrackGroup != group) {
            std::cerr << "Talent Track mismatch for Label Group " << group << ".\n";
            return 1;
        }
        if (snapshot.detectedLabelGroup) {
            std::cerr << "Talent Track group " << group << " was misidentified as CAM.\n";
            return 1;
        }
        if (snapshot.detectedLensProfile) {
            std::cerr << "Talent Track group " << group
                      << " was misidentified as Lens Profiling.\n";
            return 1;
        }
        for (int led = 0; led < 8; ++led) {
            const long long value = snapshot.fields.at("D" + std::to_string(led)).numericValue;
            if (led == 7) {
                if (value != talentGroups[index][7]) {
                    std::cerr << "Talent Track active LED 7 mismatch for group "
                              << group << ".\n";
                    return 1;
                }
                continue;
            }
            if (value != 0xFFFFFFFFLL) {
                std::cerr << "Talent Track disabled LED mismatch for group "
                          << group << ".\n";
                return 1;
            }
        }
        if (group == 7 && snapshot.fields.at("D7").numericValue != 0x804) {
            std::cerr << "Talent Track Label Group 7 should use LED 7 ID 0x804.\n";
            return 1;
        }
        if (group == 7 && snapshot.fields.at("D3").numericValue != 0xFFFFFFFFLL) {
            std::cerr << "Talent Track Label Group 7 should disable LED 3.\n";
            return 1;
        }
        for (const long long value : talentGroups[index]) {
            if (!rememberActiveLedId(value)) {
                std::cerr << "Duplicate active LED ID in Talent Track Label Group "
                          << group << ".\n";
                return 1;
            }
        }

        auto legacyDisabled = talentGroups[index];
        for (int led = 0; led < 8; ++led) {
            if (led != 7) {
                legacyDisabled[led] = 0x7FFFFFFFLL;
            }
        }
        const auto legacySnapshot =
            activetag::ActiveTag::parseDump(talentTrackDump(group, legacyDisabled));
        if (!legacySnapshot.detectedTalentTrackGroup ||
            *legacySnapshot.detectedTalentTrackGroup != group) {
            std::cerr << "Legacy disabled Talent Track mismatch for Label Group "
                      << group << ".\n";
            return 1;
        }
    }

    const auto& lensProfiles = activetag::ActiveTag::lensProfiles();
    if (lensProfiles[0][0] != 0x54 || lensProfiles[0][3] != 0x2A0 ||
        lensProfiles[1][0] != 0xA02 || lensProfiles[1][3] != 0x492) {
        std::cerr << "Lens Profiling fixed LED IDs do not match the captured profiles.\n";
        return 1;
    }
    for (int index = 0; index < static_cast<int>(lensProfiles.size()); ++index) {
        reservedUplinks.insert(index + 22);
        const auto snapshot =
            activetag::ActiveTag::parseDump(lensProfileDump(index, lensProfiles[index]));
        if (!snapshot.detectedLensProfile || *snapshot.detectedLensProfile != index) {
            std::cerr << "Lens Profiling mismatch for Profile TAG "
                      << (index + 1) << ".\n";
            return 1;
        }
        if (snapshot.detectedLabelGroup) {
            std::cerr << "Lens Profile TAG " << (index + 1)
                      << " was misidentified as CAM.\n";
            return 1;
        }
        if (snapshot.detectedTalentTrackGroup) {
            std::cerr << "Lens Profile TAG " << (index + 1)
                      << " was misidentified as Talent Track.\n";
            return 1;
        }
        for (int led = 0; led < 8; ++led) {
            const long long value = snapshot.fields.at("D" + std::to_string(led)).numericValue;
            if (value != lensProfiles[index][led]) {
                std::cerr << "Lens Profile TAG " << (index + 1)
                          << " LED " << led << " mismatch.\n";
                return 1;
            }
            if (led >= 4 && value != 0xFFFFFFFFLL) {
                std::cerr << "Lens Profile TAG " << (index + 1)
                          << " disabled LED mismatch.\n";
                return 1;
            }
            if (!rememberActiveLedId(value)) {
                std::cerr << "Duplicate active LED ID in Lens Profile TAG "
                          << (index + 1) << ".\n";
                return 1;
            }
        }

        auto legacyDisabled = lensProfiles[index];
        for (int led = 4; led < 8; ++led) {
            legacyDisabled[led] = 0x7FFFFFFFLL;
        }
        const auto legacySnapshot =
            activetag::ActiveTag::parseDump(lensProfileDump(index, legacyDisabled));
        if (!legacySnapshot.detectedLensProfile || *legacySnapshot.detectedLensProfile != index) {
            std::cerr << "Legacy disabled Lens Profile TAG " << (index + 1)
                      << " mismatch.\n";
            return 1;
        }
    }

    if (reservedUplinks.size() != 23 ||
        *reservedUplinks.begin() != 1 ||
        *reservedUplinks.rbegin() != 23) {
        std::cerr << "Ready-made profiles must reserve contiguous Label Groups 1-23.\n";
        return 1;
    }

    std::cout << "All native parser tests passed.\n";
    return 0;
}
