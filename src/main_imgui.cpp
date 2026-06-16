#include "active_tag.hpp"
#include "generated/resource.h"
#include "generated/version.hpp"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <tchar.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kWindowClass[] = L"ZeroDensityActiveTAGImGuiWindow";
constexpr wchar_t kWindowTitle[] = ACTIVETAG_WINDOW_TITLE_W;
constexpr wchar_t kAppTitle[] = ACTIVETAG_APP_TITLE_W;
constexpr long long kLedDisabledWriteValue = 0x7FFFFFFFLL;
constexpr long long kLedDisabledLegacyValue = 0xFFFFFFFFLL;

enum class ProductType {
    Camera,
    TalentTrack,
    LensProfiling
};

enum class DialogKind {
    Info,
    Warning,
    Error
};

struct ModalDialog {
    bool open = false;
    bool requestOpen = false;
    bool confirm = false;
    DialogKind kind = DialogKind::Info;
    std::string title;
    std::string message;
    std::function<void()> onConfirm;
};

struct DxState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
};

struct AppState {
    activetag::ActiveTag tag;
    activetag::Snapshot snapshot;
    std::vector<activetag::PortInfo> ports;
    std::map<std::string, long long> values;
    std::vector<std::wstring> uiLog;
    std::ofstream logFile;
    std::filesystem::path logPath;
    std::wstring selectedPort;
    std::wstring detectedPort;
    std::thread probeThread;
    std::mutex mutex;
    std::atomic_bool probeRunning = false;
    std::atomic_bool shouldConnectDetected = false;
    bool connected = false;
    bool busy = false;
    bool ledFieldsLocked = false;
    ProductType product = ProductType::Camera;
    int selectedProfile = 0;
    bool darkTheme = true;
    ModalDialog dialog;
};

DxState g_dx;
AppState g_app;

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return buffer;
}

std::wstring timestampLines(const std::wstring& text) {
    std::wstring normalized = text;
    std::replace(normalized.begin(), normalized.end(), L'\r', L'\n');
    std::wstringstream input(normalized);
    std::wstring line;
    std::wstring output;
    while (std::getline(input, line, L'\n')) {
        if (!line.empty()) {
            output += L"[" + timestamp() + L"] " + line + L"\n";
        }
    }
    return output;
}

bool openLogFile() {
    wchar_t executablePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    g_app.logPath = std::filesystem::path(executablePath).parent_path() /
        L"ActiveTAG-Configurator.log";
    g_app.logFile.open(g_app.logPath, std::ios::binary | std::ios::app);
    return g_app.logFile.is_open();
}

void appendLog(const std::wstring& text) {
    const std::wstring lines = timestampLines(text);
    if (lines.empty()) {
        return;
    }
    {
        std::scoped_lock lock(g_app.mutex);
        std::wstringstream input(lines);
        std::wstring line;
        while (std::getline(input, line, L'\n')) {
            if (!line.empty()) {
                g_app.uiLog.push_back(line);
            }
        }
        if (g_app.uiLog.size() > 1500) {
            g_app.uiLog.erase(g_app.uiLog.begin(), g_app.uiLog.begin() + 500);
        }
    }
    if (g_app.logFile.is_open()) {
        g_app.logFile << wideToUtf8(lines);
        g_app.logFile.flush();
    }
}

void showDialog(
    DialogKind kind,
    const std::string& title,
    const std::string& message,
    bool confirm = false,
    std::function<void()> onConfirm = {}) {
    g_app.dialog.kind = kind;
    g_app.dialog.title = title;
    g_app.dialog.message = message;
    g_app.dialog.confirm = confirm;
    g_app.dialog.onConfirm = std::move(onConfirm);
    g_app.dialog.open = true;
    g_app.dialog.requestOpen = true;
}

void showErrorDialog(const std::exception& error) {
    showDialog(DialogKind::Error, "Error", error.what());
}

void clearVisibleLog() {
    {
        std::scoped_lock lock(g_app.mutex);
        g_app.uiLog.clear();
    }
    if (g_app.logFile.is_open()) {
        g_app.logFile << wideToUtf8(
            timestampLines(L"UI log view cleared from the application window.\n"));
        g_app.logFile.flush();
    }
}

std::wstring metadataValue(const activetag::Snapshot& snapshot, const std::string& key) {
    const auto it = snapshot.metadata.find(key);
    return it == snapshot.metadata.end() ? L"-" : utf8ToWide(it->second);
}

std::string formatHex(long long value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex
           << static_cast<unsigned long long>(value);
    return output.str();
}

bool parseInteger(const std::string& input, long long& value) {
    try {
        size_t consumed = 0;
        value = std::stoll(input, &consumed, 0);
        return consumed == input.size() && value >= 0;
    } catch (...) {
        return false;
    }
}

const char* productLabel(ProductType product) {
    switch (product) {
        case ProductType::TalentTrack:
            return "Talent Track";
        case ProductType::LensProfiling:
            return "Lens Profiling";
        default:
            return "CAM";
    }
}

ProductType productFromIndex(int index) {
    if (index == 1) {
        return ProductType::TalentTrack;
    }
    if (index == 2) {
        return ProductType::LensProfiling;
    }
    return ProductType::Camera;
}

int productIndex(ProductType product) {
    if (product == ProductType::TalentTrack) {
        return 1;
    }
    if (product == ProductType::LensProfiling) {
        return 2;
    }
    return 0;
}

std::vector<std::string> profileNames(ProductType product) {
    std::vector<std::string> names{"Custom"};
    if (product == ProductType::Camera) {
        for (int group = 0; group < 6; ++group) {
            names.push_back("CAM" + std::to_string(group + 1) +
                " - Label Group " + std::to_string(group));
        }
    } else if (product == ProductType::TalentTrack) {
        for (int group = 6; group <= 20; ++group) {
            names.push_back("Talent Track " + std::to_string(group - 5) +
                " - Label Group " + std::to_string(group));
        }
    }
    return names;
}

std::wstring currentProfileName(const activetag::Snapshot& snapshot) {
    if (snapshot.detectedLabelGroup) {
        return L"CAM" + std::to_wstring(*snapshot.detectedLabelGroup + 1) +
            L" / Label Group " + std::to_wstring(*snapshot.detectedLabelGroup);
    }
    if (snapshot.detectedTalentTrackGroup) {
        return L"Talent Track " + std::to_wstring(*snapshot.detectedTalentTrackGroup - 5) +
            L" / Label Group " + std::to_wstring(*snapshot.detectedTalentTrackGroup);
    }
    return L"Custom";
}

void selectProduct(ProductType product, int selectedProfile = 0, bool lockLedFields = false) {
    g_app.product = product;
    g_app.selectedProfile = selectedProfile;
    g_app.ledFieldsLocked = lockLedFields;
}

void setValue(const std::string& id, long long value) {
    g_app.values[id] = value;
}

void applyProfile(int selectedProfile) {
    g_app.selectedProfile = selectedProfile;
    if (selectedProfile <= 0) {
        g_app.ledFieldsLocked = false;
        return;
    }

    const std::array<long long, 8>* leds = nullptr;
    int uplink = 0;
    if (g_app.product == ProductType::Camera && selectedProfile <= 6) {
        uplink = selectedProfile - 1;
        leds = &activetag::ActiveTag::labelGroups()[uplink];
    } else if (g_app.product == ProductType::TalentTrack && selectedProfile <= 15) {
        uplink = selectedProfile + 5;
        leds = &activetag::ActiveTag::talentTrackGroups()[selectedProfile - 1];
        setValue("3", 20);
        setValue("4", 20);
        setValue("5", 1);
    } else {
        g_app.ledFieldsLocked = false;
        return;
    }

    setValue("2", uplink);
    for (int led = 0; led < 8; ++led) {
        setValue("D" + std::to_string(led), (*leds)[led]);
    }
    g_app.ledFieldsLocked = true;
}

void renderSnapshot(const activetag::Snapshot& snapshot) {
    g_app.snapshot = snapshot;
    g_app.values.clear();
    for (const auto& [id, field] : snapshot.fields) {
        if (field.supported && field.hasNumericValue) {
            g_app.values[id] = field.numericValue;
        }
    }
    if (snapshot.detectedLabelGroup) {
        selectProduct(ProductType::Camera, *snapshot.detectedLabelGroup + 1, true);
    } else if (snapshot.detectedTalentTrackGroup) {
        selectProduct(ProductType::TalentTrack, *snapshot.detectedTalentTrackGroup - 5, true);
    } else {
        selectProduct(g_app.product, 0, false);
    }
}

void refreshPorts() {
    g_app.ports = activetag::SerialPort::enumerate();
    if (g_app.selectedPort.empty() && !g_app.ports.empty()) {
        g_app.selectedPort = g_app.ports.front().path;
    }
}

void startAutoProbe() {
    if (g_app.connected || g_app.busy || g_app.probeRunning || g_app.ports.empty()) {
        return;
    }
    if (g_app.probeThread.joinable()) {
        g_app.probeThread.join();
    }
    std::vector<std::wstring> paths;
    for (const auto& port : g_app.ports) {
        paths.push_back(port.path);
    }
    g_app.probeRunning = true;
    g_app.probeThread = std::thread([paths = std::move(paths)]() {
        for (const auto& path : paths) {
            try {
                activetag::ActiveTag candidate;
                candidate.connect(path);
                candidate.disconnect();
                {
                    std::scoped_lock lock(g_app.mutex);
                    g_app.detectedPort = path;
                }
                g_app.shouldConnectDetected = true;
                g_app.probeRunning = false;
                return;
            } catch (...) {
            }
        }
        g_app.probeRunning = false;
    });
}

void connectPort(const std::wstring& port) {
    if (port.empty()) {
        return;
    }
    g_app.busy = true;
    try {
        appendLog(L"\nConnecting to " + port + L"...\n");
        g_app.tag.connect(port);
        g_app.connected = true;
        g_app.selectedPort = port;
        renderSnapshot(g_app.tag.read());
    } catch (const std::exception& error) {
        g_app.tag.disconnect();
        g_app.connected = false;
        appendLog(L"\nERROR: " + utf8ToWide(error.what()) + L"\n");
        showErrorDialog(error);
    }
    g_app.busy = false;
}

std::wstring chooseFile(bool save) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"ActiveTAG Config (*.json)\0*.json\0All Files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"json";
    dialog.Flags = save
        ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST
        : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
        ? std::wstring(path)
        : std::wstring();
}

json snapshotToJson(const activetag::Snapshot& snapshot) {
    json fields = json::object();
    for (const auto& [id, field] : snapshot.fields) {
        if (field.supported && field.documented && field.hasNumericValue) {
            fields[id] = field.numericValue;
        }
    }
    return {
        {"schema", "activetag-config/v1"},
        {"device", {
            {"serialNumber", snapshot.metadata.contains("serialNum")
                ? snapshot.metadata.at("serialNum") : ""},
            {"hardwareRevision", snapshot.metadata.contains("hardwareRev")
                ? snapshot.metadata.at("hardwareRev") : ""},
            {"firmwareVersion", snapshot.firmwareVersion}
        }},
        {"configuration", {
            {"detectedLabelGroup", snapshot.detectedLabelGroup
                ? json(*snapshot.detectedLabelGroup) : json(nullptr)},
            {"detectedTalentTrackGroup", snapshot.detectedTalentTrackGroup
                ? json(*snapshot.detectedTalentTrackGroup) : json(nullptr)},
            {"fields", fields}
        }},
        {"source", {
            {"checksum", snapshot.metadata.contains("checksum")
                ? snapshot.metadata.at("checksum") : ""},
            {"rawDump", snapshot.raw}
        }}
    };
}

void exportConfig() {
    std::wstring path = chooseFile(true);
    if (path.empty()) {
        return;
    }
    if (std::filesystem::path(path).extension().empty()) {
        path += L".json";
    }
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output << snapshotToJson(g_app.snapshot).dump(2) << '\n';
    appendLog(L"Config exported: " + path + L"\n");
}

void importConfig() {
    const std::wstring path = chooseFile(false);
    if (path.empty()) {
        return;
    }
    try {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        const json value = json::parse(input);
        if (value.value("schema", "") != "activetag-config/v1") {
            throw std::runtime_error("Unsupported config schema.");
        }
        const auto& fields = value.at("configuration").at("fields");
        for (const auto& id : {"2", "3", "4", "5", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"}) {
            if (fields.contains(id) && fields.at(id).is_number_integer()) {
                g_app.values[id] = fields.at(id).get<long long>();
            }
        }
        g_app.selectedProfile = 0;
        g_app.ledFieldsLocked = false;
        appendLog(L"Config loaded into editor; it has not been written yet: " + path + L"\n");
    } catch (const std::exception& error) {
        appendLog(L"\nERROR: " + utf8ToWide(error.what()) + L"\n");
        showErrorDialog(error);
    }
}

std::map<std::string, long long> collectValues() {
    std::map<std::string, long long> values;
    for (const auto& id : {"2", "3", "4", "5", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"}) {
        values[id] = g_app.values[id];
    }
    return values;
}

void writeConfigToTag() {
    if (!g_app.connected) {
        return;
    }
    g_app.busy = true;
    try {
        const auto [saved, changes] = g_app.tag.apply(collectValues());
        renderSnapshot(saved);
        appendLog(L"Saved and verified " + std::to_wstring(changes.size()) + L" changed field(s).\n");
        showDialog(DialogKind::Info, "Config Saved", "Config saved and verified.");
    } catch (const std::exception& error) {
        appendLog(L"\nERROR: " + utf8ToWide(error.what()) + L"\n");
        showErrorDialog(error);
    }
    g_app.busy = false;
}

void confirmSaveToTag() {
    if (!g_app.connected) {
        return;
    }
    showDialog(
        DialogKind::Warning,
        "Confirm Write",
        "Changes will be verified and written to the Active Tag flash memory.\n\nContinue?",
        true,
        []() { writeConfigToTag(); });
}

void setTheme() {
    if (g_app.darkTheme) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 7.0f;
    style.WindowPadding = ImVec2(16, 14);
    style.ItemSpacing = ImVec2(10, 8);
    style.FramePadding = ImVec2(10, 6);
    style.ScrollbarSize = 12.0f;
    style.Colors[ImGuiCol_Header] = ImVec4(0.07f, 0.42f, 0.88f, 0.75f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.09f, 0.50f, 0.95f, 0.90f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.06f, 0.36f, 0.78f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.07f, 0.42f, 0.88f, 0.85f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.09f, 0.50f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.05f, 0.31f, 0.68f, 1.0f);
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel{};
    return D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels, 2,
        D3D11_SDK_VERSION, &sd, &g_dx.swapChain, &g_dx.device,
        &featureLevel, &g_dx.context) == S_OK;
}

void cleanupRenderTarget() {
    if (g_dx.renderTarget) {
        g_dx.renderTarget->Release();
        g_dx.renderTarget = nullptr;
    }
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_dx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_dx.device->CreateRenderTargetView(backBuffer, nullptr, &g_dx.renderTarget);
    backBuffer->Release();
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_dx.swapChain) {
        g_dx.swapChain->Release();
        g_dx.swapChain = nullptr;
    }
    if (g_dx.context) {
        g_dx.context->Release();
        g_dx.context = nullptr;
    }
    if (g_dx.device) {
        g_dx.device->Release();
        g_dx.device = nullptr;
    }
}

LRESULT WINAPI windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && g_dx.device) {
                cleanupRenderTarget();
                g_dx.swapChain->ResizeBuffers(
                    0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) {
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void drawProductSelector() {
    ImGui::TextUnformatted("Product");
    const char* labels[] = {"CAM", "Talent Track", "Lens Profiling"};
    const int current = productIndex(g_app.product);
    const float available = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = std::clamp((available - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f,
        120.0f, 145.0f);
    for (int index = 0; index < 3; ++index) {
        if (index > 0) {
            ImGui::SameLine();
        }
        const bool selected = current == index;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }
        if (ImGui::Button(labels[index], ImVec2(buttonWidth, 34))) {
            selectProduct(productFromIndex(index), 0, false);
        }
        ImGui::PopStyleColor(3);
    }
}

void drawProfileCombo() {
    const auto names = profileNames(g_app.product);
    if (g_app.selectedProfile >= static_cast<int>(names.size())) {
        g_app.selectedProfile = 0;
    }
    const char* preview = names[g_app.selectedProfile].c_str();
    ImGui::TextUnformatted("Profile");
    ImGui::SetNextItemWidth(std::min(390.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##profile", preview)) {
        for (int index = 0; index < static_cast<int>(names.size()); ++index) {
            const bool selected = index == g_app.selectedProfile;
            if (ImGui::Selectable(names[index].c_str(), selected)) {
                applyProfile(index);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void drawNumberField(const char* label, const std::string& id) {
    int value = static_cast<int>(g_app.values[id]);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(std::min(190.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputInt(("##" + id).c_str(), &value, 0, 0)) {
        g_app.values[id] = std::max(0, value);
    }
}

void drawLedField(int led) {
    const std::string id = "D" + std::to_string(led);
    long long value = g_app.values[id];
    std::string text = formatHex(value);
    const bool locked = g_app.ledFieldsLocked;
    if (locked) {
        ImGui::BeginDisabled();
    }
    char buffer[32]{};
    strcpy_s(buffer, text.c_str());
    const std::string label = "LED " + std::to_string(led) + " Active ID";
    ImGui::PushID(led);
    ImGui::TextUnformatted(label.c_str());
    ImGui::SetNextItemWidth(std::min(210.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputText("##hex", buffer, static_cast<size_t>(std::size(buffer)),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
        long long parsed = 0;
        std::string input = buffer;
        if (input.rfind("0x", 0) != 0 && input.rfind("0X", 0) != 0) {
            input = "0x" + input;
        }
        if (parseInteger(input, parsed)) {
            g_app.values[id] = parsed;
            value = parsed;
        }
    }
    if (locked) {
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled(
        (value == kLedDisabledWriteValue || value == kLedDisabledLegacyValue)
            ? "Decimal: %lld  (Disabled)"
            : "Decimal: %lld",
        value);
    ImGui::PopID();
}

void drawSectionHeader(const char* label) {
    ImGui::TextUnformatted(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Separator);
    drawList->AddLine(
        ImVec2(start.x, start.y + 3.0f),
        ImVec2(start.x + width, start.y + 3.0f),
        color);
    ImGui::Dummy(ImVec2(0, 12));
}

int visibleLedCount() {
    if (g_app.product == ProductType::TalentTrack) {
        return 1;
    }
    if (g_app.product == ProductType::LensProfiling) {
        return 4;
    }
    return 8;
}

void drawModalDialog() {
    if (!g_app.dialog.open) {
        return;
    }
    if (g_app.dialog.requestOpen) {
        ImGui::OpenPopup(g_app.dialog.title.c_str());
        g_app.dialog.requestOpen = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);

    bool keepOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 7));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.62f));
    if (ImGui::BeginPopupModal(
            g_app.dialog.title.c_str(),
            &keepOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const char* icon = "i";
        ImVec4 iconColor = ImVec4(0.18f, 0.55f, 1.0f, 1.0f);
        if (g_app.dialog.kind == DialogKind::Warning) {
            icon = "!";
            iconColor = ImVec4(1.0f, 0.72f, 0.20f, 1.0f);
        } else if (g_app.dialog.kind == DialogKind::Error) {
            icon = "x";
            iconColor = ImVec4(1.0f, 0.30f, 0.30f, 1.0f);
        }

        ImGui::TextColored(iconColor, "%s", icon);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
        ImGui::TextUnformatted(g_app.dialog.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 120.0f;
        const float totalWidth = g_app.dialog.confirm
            ? buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x
            : buttonWidth;
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - totalWidth) * 0.5f));

        bool runConfirm = false;
        std::function<void()> confirmAction;
        if (g_app.dialog.confirm) {
            if (ImGui::Button("Yes", ImVec2(buttonWidth, 32))) {
                confirmAction = g_app.dialog.onConfirm;
                g_app.dialog = {};
                ImGui::CloseCurrentPopup();
                runConfirm = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(buttonWidth, 32))) {
                g_app.dialog = {};
                ImGui::CloseCurrentPopup();
            }
        } else if (ImGui::Button("OK", ImVec2(buttonWidth, 32))) {
            g_app.dialog = {};
            ImGui::CloseCurrentPopup();
        }

        if (!keepOpen) {
            g_app.dialog = {};
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
        if (runConfirm && confirmAction) {
            confirmAction();
        }
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void drawDeviceHeader() {
    if (!g_app.connected) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
            "No Active Tag connected. New COM ports are probed automatically.");
        return;
    }
    ImGui::Text("Serial: %s", wideToUtf8(metadataValue(g_app.snapshot, "serialNum")).c_str());
    ImGui::SameLine(140);
    ImGui::Text("Firmware: %s", g_app.snapshot.firmwareVersion.c_str());
    ImGui::SameLine(295);
    ImGui::Text("Hardware: %s", wideToUtf8(metadataValue(g_app.snapshot, "hardwareRev")).c_str());
    ImGui::SameLine(430);
    ImGui::Text("COM: %s", wideToUtf8(g_app.tag.portPath()).c_str());
    ImGui::SameLine(560);
    ImGui::Text("Profile: %s", wideToUtf8(currentProfileName(g_app.snapshot)).c_str());
}

void drawMainUi() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("ActiveTAG", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("%s", wideToUtf8(kAppTitle).c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 145);
    bool dark = g_app.darkTheme;
    if (ImGui::Checkbox("Dark theme", &dark)) {
        g_app.darkTheme = dark;
        setTheme();
    }
    ImGui::Separator();

    drawDeviceHeader();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    ImGui::BeginChild(
        "TopCard",
        ImVec2(0, 168),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float topWidth = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float actionWidth = std::clamp(topWidth * 0.44f, 420.0f, 500.0f);
    float leftWidth = topWidth - actionWidth - gap;
    if (leftWidth < 500.0f) {
        leftWidth = std::max(430.0f, topWidth - 390.0f - gap);
        actionWidth = std::max(360.0f, topWidth - leftWidth - gap);
    }

    ImGui::BeginChild(
        "ConnectionPanel",
        ImVec2(leftWidth, 0),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextUnformatted("COM Port");
    ImGui::SameLine(95);
    const std::string selectedPortUtf8 = wideToUtf8(g_app.selectedPort);
    const float connectButtonWidth = g_app.connected ? 96.0f : 86.0f;
    const float comboWidth = std::clamp(
        leftWidth - 95.0f - 78.0f - connectButtonWidth - gap * 3.0f,
        180.0f,
        285.0f);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##ports", selectedPortUtf8.empty() ? "Select COM" : selectedPortUtf8.c_str())) {
        for (const auto& port : g_app.ports) {
            const std::string name = wideToUtf8(port.path);
            const bool selected = port.path == g_app.selectedPort;
            if (ImGui::Selectable(name.c_str(), selected)) {
                g_app.selectedPort = port.path;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(78, 32))) {
        refreshPorts();
        startAutoProbe();
    }
    ImGui::SameLine();
    if (!g_app.connected) {
        if (ImGui::Button("Connect", ImVec2(connectButtonWidth, 32))) {
            connectPort(g_app.selectedPort);
        }
    } else if (ImGui::Button("Disconnect", ImVec2(connectButtonWidth, 32))) {
        g_app.tag.disconnect();
        g_app.connected = false;
        appendLog(L"Disconnected.\n");
    }
    drawProductSelector();
    drawProfileCombo();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild(
        "ActionsPanel",
        ImVec2(actionWidth, 0),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted("Actions");
    const float actionButtonWidth = std::max(
        150.0f,
        (ImGui::GetContentRegionAvail().x - gap) / 2.0f);
    ImGui::BeginDisabled(!g_app.connected);
    if (ImGui::Button("Read Again", ImVec2(actionButtonWidth, 32))) {
        try {
            renderSnapshot(g_app.tag.read());
        } catch (const std::exception& error) {
            appendLog(L"\nERROR: " + utf8ToWide(error.what()) + L"\n");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to Active Tag", ImVec2(actionButtonWidth, 32))) {
        confirmSaveToTag();
    }
    if (ImGui::Button("Export Config", ImVec2(actionButtonWidth, 32))) {
        exportConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Import Config", ImVec2(actionButtonWidth, 32))) {
        importConfig();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Ready-made profiles lock LED IDs. Custom unlocks them.");
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);

    const float logWidth = 390.0f;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float lowerHeight = available.y;
    ImGui::BeginChild(
        "Editor",
        ImVec2(std::max(360.0f, available.x - logWidth - 10.0f), lowerHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 270);
    drawSectionHeader("LED Active IDs");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
    for (int led = 0; led < visibleLedCount(); ++led) {
        drawLedField(led);
    }
    ImGui::PopStyleVar(2);
    ImGui::NextColumn();
    drawSectionHeader("General Settings");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    drawNumberField("Uplink ID", "2");
    drawNumberField("RF Channel", "3");
    drawNumberField("LED Brightness", "4");
    drawNumberField("On While Charging", "5");
    ImGui::PopStyleVar(2);
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild(
        "Log",
        ImVec2(logWidth, lowerHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted("Serial Communication Log");
    ImGui::Separator();
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f;
    ImGui::BeginChild("LogLines", ImVec2(0, -footerHeight), false);
    std::vector<std::wstring> lines;
    {
        std::scoped_lock lock(g_app.mutex);
        lines = g_app.uiLog;
    }
    for (const auto& line : lines) {
        ImGui::TextWrapped("%s", wideToUtf8(line).c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("Clear Log", ImVec2(110, 30))) {
        clearVisibleLog();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("View only. File stays append-only.");
    ImGui::EndChild();
    ImGui::End();
    drawModalDialog();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (!openLogFile()) {
        MessageBoxW(nullptr, L"ActiveTAG-Configurator.log could not be opened.", kAppTitle,
            MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_ACTIVETAG), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_ACTIVETAG), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    HWND hwnd = CreateWindowW(kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 760,
        nullptr, nullptr, instance, nullptr);
    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(kWindowClass, instance);
        return 1;
    }
    createRenderTarget();

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    setTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dx.device, g_dx.context);

    g_app.tag.setLogCallback([](const std::string& message) {
        appendLog(utf8ToWide(message));
    });
    appendLog(L"ActiveTAG Configurator ImGui started.");
    appendLog(L"Log file: " + g_app.logPath.wstring());
    refreshPorts();
    startAutoProbe();

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        if (g_app.shouldConnectDetected.exchange(false)) {
            std::wstring port;
            {
                std::scoped_lock lock(g_app.mutex);
                port = g_app.detectedPort;
            }
            if (!g_app.connected) {
                connectPort(port);
            }
        } else if (!g_app.connected && !g_app.probeRunning) {
            static DWORD lastProbe = 0;
            const DWORD now = GetTickCount();
            if (now - lastProbe > 2000) {
                refreshPorts();
                startAutoProbe();
                lastProbe = now;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawMainUi();
        ImGui::Render();
        const float clearColor[4] = {0.08f, 0.09f, 0.11f, 1.0f};
        g_dx.context->OMSetRenderTargets(1, &g_dx.renderTarget, nullptr);
        g_dx.context->ClearRenderTargetView(g_dx.renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_dx.swapChain->Present(1, 0);
    }

    g_app.tag.disconnect();
    if (g_app.probeThread.joinable()) {
        g_app.probeThread.join();
    }
    appendLog(L"ActiveTAG Configurator stopped.");
    if (g_app.logFile.is_open()) {
        g_app.logFile.flush();
        g_app.logFile.close();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, instance);
    return 0;
}
