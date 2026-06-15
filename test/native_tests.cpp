#include "../src/active_tag.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
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
        if (!snapshot.detectedLabelGroup || *snapshot.detectedLabelGroup != index) {
            std::cerr << "Label Group mismatch for dump " << index << ".\n";
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

    std::cout << "All native parser tests passed.\n";
    return 0;
}
