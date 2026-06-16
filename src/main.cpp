#include "active_tag.hpp"
#include "generated/resource.h"
#include "generated/version.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr wchar_t kWindowClass[] = L"ActiveTAGConfiguratorWindow";
constexpr wchar_t kProductSelectorClass[] = L"ActiveTAGProductSelector";
constexpr wchar_t kWindowTitle[] = ACTIVETAG_WINDOW_TITLE_W;
constexpr wchar_t kAppTitle[] = ACTIVETAG_APP_TITLE_W;
constexpr UINT_PTR kPortTimer = 1;
constexpr UINT kPortScanIntervalMs = 1400;
constexpr UINT WM_ACTIVE_TAG_FOUND = WM_APP + 1;
constexpr UINT WM_ACTIVE_TAG_PROBE_FINISHED = WM_APP + 2;

enum ControlId {
    IDC_PORTS = 1001,
    IDC_REFRESH_PORTS,
    IDC_CONNECT,
    IDC_DISCONNECT,
    IDC_READ,
    IDC_EXPORT,
    IDC_IMPORT,
    IDC_SAVE,
    IDC_GROUP,
    IDC_PRODUCT_SELECTOR,
    IDC_THEME,
    IDC_LOG,
    IDC_DEVICE_INFO,
    IDC_FIRST_FIELD = 2000
};

struct FieldUi {
    std::string id;
    HWND label = nullptr;
    HWND edit = nullptr;
    HWND note = nullptr;
    bool isLedId = false;
};

enum class ThemeMode {
    Light,
    Dark
};

enum class ProductType {
    Camera,
    TalentTrack,
    LensProfiling
};

struct AppState {
    HWND window = nullptr;
    HWND portCombo = nullptr;
    HWND connectButton = nullptr;
    HWND disconnectButton = nullptr;
    HWND readButton = nullptr;
    HWND exportButton = nullptr;
    HWND importButton = nullptr;
    HWND saveButton = nullptr;
    HWND groupCombo = nullptr;
    HWND productSelector = nullptr;
    HWND themeCombo = nullptr;
    HWND deviceInfo = nullptr;
    HWND log = nullptr;
    HFONT normalFont = nullptr;
    HFONT titleFont = nullptr;
    HFONT tabFont = nullptr;
    activetag::ActiveTag tag;
    activetag::Snapshot snapshot;
    std::vector<FieldUi> fields;
    std::vector<activetag::PortInfo> ports;
    std::thread probeThread;
    std::atomic_bool probeRunning = false;
    std::ofstream logFile;
    std::filesystem::path logPath;
    bool busy = false;
    bool ledFieldsLocked = false;
    ProductType product = ProductType::Camera;
    ThemeMode theme = ThemeMode::Light;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HBRUSH editBrush = nullptr;
    COLORREF textColor = RGB(28, 32, 36);
    COLORREF mutedTextColor = RGB(83, 91, 99);
    COLORREF backgroundColor = RGB(246, 247, 249);
    COLORREF panelColor = RGB(255, 255, 255);
    COLORREF editColor = RGB(255, 255, 255);
    COLORREF accentColor = RGB(21, 112, 239);
    COLORREF accentTextColor = RGB(255, 255, 255);
};

AppState g;

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

void setFont(HWND control, HFONT font = nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g.normalFont), TRUE);
}

HWND createControl(
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id,
    DWORD extendedStyle = 0) {
    HWND control = CreateWindowExW(
        extendedStyle,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        g.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    setFont(control);
    return control;
}

std::wstring timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

std::wstring timestampLines(const std::wstring& text) {
    std::wstring normalized = text;
    std::replace(normalized.begin(), normalized.end(), L'\r', L'\n');

    std::wstringstream input(normalized);
    std::wstring line;
    std::wstring output;
    while (std::getline(input, line, L'\n')) {
        if (line.empty()) {
            continue;
        }
        output += L"[" + timestamp() + L"] " + line + L"\r\n";
    }
    return output;
}

bool openLogFile() {
    wchar_t executablePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executablePath,
        static_cast<DWORD>(std::size(executablePath)));
    if (length == 0 || length >= std::size(executablePath)) {
        return false;
    }

    g.logPath = std::filesystem::path(executablePath).parent_path() /
        L"ActiveTAG-Configurator.log";
    g.logFile.open(g.logPath, std::ios::binary | std::ios::app);
    return g.logFile.is_open();
}

void appendLog(const std::wstring& text) {
    const std::wstring timestamped = timestampLines(text);
    if (timestamped.empty()) {
        return;
    }

    if (IsWindow(g.log)) {
        const int length = GetWindowTextLengthW(g.log);
        SendMessageW(g.log, EM_SETSEL, length, length);
        SendMessageW(
            g.log,
            EM_REPLACESEL,
            FALSE,
            reinterpret_cast<LPARAM>(timestamped.c_str()));
        SendMessageW(g.log, EM_SCROLLCARET, 0, 0);
    }

    if (g.logFile.is_open()) {
        g.logFile << wideToUtf8(timestamped);
        g.logFile.flush();
    }
}

void deleteBrush(HBRUSH& brush) {
    if (brush) {
        DeleteObject(brush);
        brush = nullptr;
    }
}

void rebuildThemeBrushes() {
    if (g.window) {
        SetClassLongPtrW(
            g.window,
            GCLP_HBRBACKGROUND,
            reinterpret_cast<LONG_PTR>(GetStockObject(NULL_BRUSH)));
    }
    deleteBrush(g.backgroundBrush);
    deleteBrush(g.panelBrush);
    deleteBrush(g.editBrush);

    if (g.theme == ThemeMode::Dark) {
        g.backgroundColor = RGB(27, 30, 34);
        g.panelColor = RGB(34, 38, 43);
        g.editColor = RGB(42, 47, 53);
        g.textColor = RGB(238, 241, 244);
        g.mutedTextColor = RGB(166, 174, 182);
        g.accentColor = RGB(45, 156, 219);
        g.accentTextColor = RGB(255, 255, 255);
    } else {
        g.backgroundColor = RGB(246, 247, 249);
        g.panelColor = RGB(255, 255, 255);
        g.editColor = RGB(255, 255, 255);
        g.textColor = RGB(28, 32, 36);
        g.mutedTextColor = RGB(83, 91, 99);
        g.accentColor = RGB(21, 112, 239);
        g.accentTextColor = RGB(255, 255, 255);
    }

    g.backgroundBrush = CreateSolidBrush(g.backgroundColor);
    g.panelBrush = CreateSolidBrush(g.panelColor);
    g.editBrush = CreateSolidBrush(g.editColor);
}

BOOL CALLBACK applyThemeToControl(HWND control, LPARAM) {
    wchar_t className[32]{};
    GetClassNameW(control, className, static_cast<int>(std::size(className)));
    const bool dark = g.theme == ThemeMode::Dark;

    if (_wcsicmp(className, L"Button") == 0 ||
        _wcsicmp(className, L"ComboBox") == 0 ||
        _wcsicmp(className, L"Edit") == 0) {
        SetWindowTheme(control, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    InvalidateRect(control, nullptr, TRUE);
    return TRUE;
}

void applyTheme(bool writeLog = true) {
    rebuildThemeBrushes();
    SetClassLongPtrW(
        g.window,
        GCLP_HBRBACKGROUND,
        reinterpret_cast<LONG_PTR>(g.backgroundBrush));

    const BOOL darkTitleBar = g.theme == ThemeMode::Dark;
    DwmSetWindowAttribute(
        g.window,
        20,
        &darkTitleBar,
        sizeof(darkTitleBar));
    EnumChildWindows(g.window, applyThemeToControl, 0);
    InvalidateRect(g.window, nullptr, TRUE);
    RedrawWindow(
        g.window,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);

    if (writeLog) {
        appendLog(g.theme == ThemeMode::Dark
            ? L"Dark theme enabled."
            : L"Light theme enabled.");
    }
}

void showError(const std::string& message) {
    appendLog(L"\r\nERROR: " + utf8ToWide(message) + L"\r\n");
    MessageBoxW(g.window, utf8ToWide(message).c_str(), L"ActiveTAG Error", MB_OK | MB_ICONERROR);
}

void updateFieldEnableState() {
    const bool connectedAndIdle = !g.busy && g.tag.isConnected();
    for (const FieldUi& field : g.fields) {
        EnableWindow(field.edit, connectedAndIdle && !(field.isLedId && g.ledFieldsLocked));
    }
}

void setBusy(bool busy) {
    g.busy = busy;
    EnableWindow(g.portCombo, !busy && !g.tag.isConnected());
    EnableWindow(g.connectButton, !busy && !g.tag.isConnected());
    EnableWindow(g.disconnectButton, !busy && g.tag.isConnected());
    EnableWindow(g.readButton, !busy && g.tag.isConnected());
    EnableWindow(g.exportButton, !busy && g.tag.isConnected());
    EnableWindow(g.saveButton, !busy && g.tag.isConnected());
    EnableWindow(g.groupCombo, !busy && g.tag.isConnected());
    EnableWindow(g.themeCombo, !busy);
    updateFieldEnableState();
}

std::wstring metadataValue(const activetag::Snapshot& snapshot, const std::string& key) {
    const auto it = snapshot.metadata.find(key);
    return it == snapshot.metadata.end() ? L"-" : utf8ToWide(it->second);
}

std::wstring formatHex(long long value) {
    std::wstringstream output;
    output << L"0x" << std::uppercase << std::hex
           << static_cast<unsigned long long>(value);
    return output.str();
}

bool getEditNumber(HWND edit, long long& value) {
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t* end = nullptr;
    value = wcstoll(buffer, &end, 0);
    return end != buffer && *end == L'\0';
}

void updateLedDecimal(FieldUi& field, long long value) {
    std::wstring text = L"Decimal: " + std::to_wstring(value);
    if (value == 0x7FFFFFFFLL || value == 0xFFFFFFFFLL) {
        text += L"  (Disabled)";
    }
    SetWindowTextW(field.note, text.c_str());
}

void setFieldNumber(FieldUi& field, long long value) {
    SetWindowTextW(
        field.edit,
        (field.isLedId ? formatHex(value) : std::to_wstring(value)).c_str());
    if (field.isLedId) {
        updateLedDecimal(field, value);
    }
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

void populateProfileCombo(int selectedProfile = 0) {
    SendMessageW(g.groupCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.groupCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));

    if (g.product == ProductType::Camera) {
        for (int group = 0; group < 6; ++group) {
            const std::wstring name =
                L"CAM" + std::to_wstring(group + 1) +
                L" - Label Group " + std::to_wstring(group);
            SendMessageW(g.groupCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    } else if (g.product == ProductType::TalentTrack) {
        for (int group = 6; group <= 20; ++group) {
            const std::wstring name =
                L"Talent Track " + std::to_wstring(group - 5) +
                L" - Label Group " + std::to_wstring(group);
            SendMessageW(g.groupCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    const LRESULT itemCount = SendMessageW(g.groupCombo, CB_GETCOUNT, 0, 0);
    const int safeSelection =
        selectedProfile >= 0 && selectedProfile < itemCount ? selectedProfile : 0;
    SendMessageW(g.groupCombo, CB_SETCURSEL, safeSelection, 0);
}

int productIndex(ProductType product) {
    return product == ProductType::Camera
        ? 0
        : product == ProductType::TalentTrack ? 1 : 2;
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

const wchar_t* productLabel(ProductType product) {
    if (product == ProductType::TalentTrack) {
        return L"Talent Track";
    }
    if (product == ProductType::LensProfiling) {
        return L"Lens Profiling";
    }
    return L"CAM";
}

void selectProduct(
    ProductType product,
    int selectedProfile = 0,
    bool lockLedFields = false) {
    g.product = product;
    populateProfileCombo(selectedProfile);
    g.ledFieldsLocked = lockLedFields;
    updateFieldEnableState();
    if (IsWindow(g.productSelector)) {
        InvalidateRect(g.productSelector, nullptr, TRUE);
    }
}

void renderSnapshot(const activetag::Snapshot& snapshot) {
    g.snapshot = snapshot;
    const std::wstring info =
        L"Serial: " + metadataValue(snapshot, "serialNum") +
        L"     Firmware: " + utf8ToWide(snapshot.firmwareVersion) +
        L"     Hardware: " + metadataValue(snapshot, "hardwareRev") +
        L"     COM: " + g.tag.portPath() +
        L"     Profile: " + currentProfileName(snapshot);
    SetWindowTextW(g.deviceInfo, info.c_str());

    for (FieldUi& fieldUi : g.fields) {
        const auto it = snapshot.fields.find(fieldUi.id);
        const bool available =
            it != snapshot.fields.end() && it->second.supported && it->second.hasNumericValue;
        ShowWindow(fieldUi.label, available ? SW_SHOW : SW_HIDE);
        ShowWindow(fieldUi.edit, available ? SW_SHOW : SW_HIDE);
        ShowWindow(fieldUi.note, available ? SW_SHOW : SW_HIDE);
        if (available) {
            setFieldNumber(fieldUi, it->second.numericValue);
            if (!fieldUi.isLedId) {
                SetWindowTextW(
                    fieldUi.note,
                    it->second.documented
                        ? utf8ToWide("Field [" + fieldUi.id + "]").c_str()
                        : L"Advanced: not documented for firmware 2.x");
            }
        }
    }

    if (snapshot.detectedLabelGroup) {
        selectProduct(
            ProductType::Camera,
            *snapshot.detectedLabelGroup + 1,
            true);
    } else if (snapshot.detectedTalentTrackGroup) {
        selectProduct(
            ProductType::TalentTrack,
            *snapshot.detectedTalentTrackGroup - 5,
            true);
    } else {
        populateProfileCombo(0);
        g.ledFieldsLocked = false;
        updateFieldEnableState();
    }
    setBusy(false);
}

void startAutoProbe() {
    if (g.tag.isConnected() || g.busy || g.probeRunning || g.ports.empty()) {
        return;
    }
    if (g.probeThread.joinable()) {
        g.probeThread.join();
    }

    std::vector<std::wstring> paths;
    paths.reserve(g.ports.size());
    for (const auto& port : g.ports) {
        paths.push_back(port.path);
    }

    g.probeRunning = true;
    const HWND targetWindow = g.window;
    g.probeThread = std::thread([paths = std::move(paths), targetWindow]() {
        for (const std::wstring& path : paths) {
            try {
                activetag::ActiveTag candidate;
                candidate.connect(path);
                candidate.disconnect();
                auto* result = new std::wstring(path);
                if (!PostMessageW(
                        targetWindow,
                        WM_ACTIVE_TAG_FOUND,
                        0,
                        reinterpret_cast<LPARAM>(result))) {
                    delete result;
                }
                return;
            } catch (...) {
                // Other COM devices are expected and are ignored.
            }
        }
        PostMessageW(targetWindow, WM_ACTIVE_TAG_PROBE_FINISHED, 0, 0);
    });
}

void refreshPorts() {
    const std::wstring selected = [&]() {
        wchar_t buffer[64]{};
        GetWindowTextW(g.portCombo, buffer, static_cast<int>(std::size(buffer)));
        return std::wstring(buffer);
    }();

    const auto previousPorts = g.ports;
    g.ports = activetag::SerialPort::enumerate();
    SendMessageW(g.portCombo, CB_RESETCONTENT, 0, 0);
    int selection = -1;
    for (int index = 0; index < static_cast<int>(g.ports.size()); ++index) {
        SendMessageW(
            g.portCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(g.ports[index].path.c_str()));
        if (g.ports[index].path == selected) {
            selection = index;
        }
    }
    if (selection < 0 && !g.ports.empty()) {
        selection = 0;
    }
    if (selection >= 0) {
        SendMessageW(g.portCombo, CB_SETCURSEL, selection, 0);
    }

    const bool portsChanged =
        previousPorts.size() != g.ports.size() ||
        !std::equal(
            previousPorts.begin(),
            previousPorts.end(),
            g.ports.begin(),
            [](const activetag::PortInfo& left, const activetag::PortInfo& right) {
                return left.path == right.path;
            });
    if (portsChanged && !g.tag.isConnected()) {
        startAutoProbe();
    }
}

void connectSelectedPort() {
    wchar_t port[64]{};
    GetWindowTextW(g.portCombo, port, static_cast<int>(std::size(port)));
    if (port[0] == L'\0') {
        MessageBoxW(g.window, L"Select a COM port.", kAppTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    setBusy(true);
    try {
        appendLog(L"\r\nConnecting to " + std::wstring(port) + L"...\r\n");
        g.tag.connect(port);
        renderSnapshot(g.tag.read());
    } catch (const std::exception& error) {
        g.tag.disconnect();
        setBusy(false);
        showError(error.what());
    }
}

std::wstring chooseFile(bool save) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g.window;
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
    output << snapshotToJson(g.snapshot).dump(2) << '\n';
    appendLog(L"Config exported: " + path + L"\r\n");
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
        if (!g.tag.isConnected()) {
            throw std::runtime_error("Connect an Active Tag before importing a config.");
        }
        const auto& fields = value.at("configuration").at("fields");
        for (FieldUi& field : g.fields) {
            if (fields.contains(field.id) && fields.at(field.id).is_number_integer()) {
                setFieldNumber(field, fields.at(field.id).get<long long>());
            }
        }
        SendMessageW(g.groupCombo, CB_SETCURSEL, 0, 0);
        g.ledFieldsLocked = false;
        updateFieldEnableState();
        appendLog(L"Config loaded into editor; it has not been written yet: " + path + L"\r\n");
    } catch (const std::exception& error) {
        showError(error.what());
    }
}

std::map<std::string, long long> collectValues() {
    std::map<std::string, long long> values;
    for (const FieldUi& field : g.fields) {
        if (!IsWindowVisible(field.edit)) {
            continue;
        }
        long long value = 0;
        if (!getEditNumber(field.edit, value)) {
            throw std::runtime_error("Invalid numeric value for field [" + field.id + "].");
        }
        values[field.id] = value;
    }
    return values;
}

void saveToTag() {
    if (MessageBoxW(
            g.window,
            L"Changes will be verified and written to the Active Tag flash memory. Continue?",
            L"Confirm write",
            MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    setBusy(true);
    try {
        const auto [saved, changes] = g.tag.apply(collectValues());
        renderSnapshot(saved);
        appendLog(
            L"Saved and verified " + std::to_wstring(changes.size()) + L" changed field(s).\r\n");
        MessageBoxW(g.window, L"Config saved and verified.", kAppTitle, MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& error) {
        setBusy(false);
        showError(error.what());
    }
}

void applyLabelGroup(int selection) {
    if (selection <= 0) {
        g.ledFieldsLocked = false;
        updateFieldEnableState();
        return;
    }
    int uplink = 0;
    const std::array<long long, 8>* ledValues = nullptr;
    if (g.product == ProductType::Camera && selection <= 6) {
        uplink = selection - 1;
        ledValues = &activetag::ActiveTag::labelGroups()[uplink];
    } else if (g.product == ProductType::TalentTrack && selection <= 15) {
        uplink = selection + 5;
        ledValues = &activetag::ActiveTag::talentTrackGroups()[selection - 1];
    } else {
        return;
    }

    for (int led = 0; led < 8; ++led) {
        for (FieldUi& field : g.fields) {
            if (field.id == "D" + std::to_string(led)) {
                setFieldNumber(field, (*ledValues)[led]);
            }
        }
    }
    for (FieldUi& field : g.fields) {
        if (field.id == "2") {
            setFieldNumber(field, uplink);
        }
        if (g.product == ProductType::TalentTrack && field.id == "3") {
            setFieldNumber(field, 20);
        }
        if (g.product == ProductType::TalentTrack && field.id == "4") {
            setFieldNumber(field, 20);
        }
        if (g.product == ProductType::TalentTrack && field.id == "5") {
            setFieldNumber(field, 1);
        }
    }
    g.ledFieldsLocked = true;
    updateFieldEnableState();
}

void createFieldUi(const std::string& id, const wchar_t* title, int x, int row) {
    const int y = 244 + row * 48;
    FieldUi field;
    field.id = id;
    field.isLedId = id.starts_with("D");
    field.label = createControl(L"STATIC", title, SS_LEFT, x, y, 139, 17, 0);
    const DWORD editStyle = WS_BORDER | ES_AUTOHSCROLL |
        (field.isLedId ? 0 : ES_NUMBER);
    field.edit = createControl(
        L"EDIT",
        L"",
        editStyle,
        x + 141,
        y - 3,
        85,
        23,
        IDC_FIRST_FIELD + static_cast<int>(g.fields.size()),
        WS_EX_CLIENTEDGE);
    field.note = createControl(
        L"STATIC",
        L"",
        SS_LEFT,
        x,
        y + 18,
        227,
        15,
        0);
    g.fields.push_back(field);
}

void createUi(HWND window) {
    g.window = window;
    g.normalFont = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.titleFont = CreateFontW(
        -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.tabFont = CreateFontW(
        -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND title = createControl(L"STATIC", kAppTitle, SS_LEFT, 22, 14, 365, 29, 0);
    setFont(title, g.titleFont);
    createControl(
        L"STATIC",
        L"Native Windows USB serial configuration utility",
        SS_LEFT,
        23,
        43,
        405,
        18,
        0);

    createControl(L"STATIC", L"Theme", SS_RIGHT, 733, 20, 48, 18, 0);
    g.themeCombo = createControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST,
        789,
        16,
        122,
        100,
        IDC_THEME);
    SendMessageW(g.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light"));
    SendMessageW(g.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    SendMessageW(g.themeCombo, CB_SETCURSEL, 0, 0);

    createControl(L"STATIC", L"COM Port", SS_LEFT, 23, 76, 65, 18, 0);
    g.portCombo = createControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_VSCROLL,
        88,
        72,
        122,
        207,
        IDC_PORTS);
    createControl(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 218, 71, 68, 25, IDC_REFRESH_PORTS);
    g.connectButton = createControl(L"BUTTON", L"Connect", BS_DEFPUSHBUTTON, 293, 71, 73, 25, IDC_CONNECT);
    g.disconnectButton = createControl(L"BUTTON", L"Disconnect", BS_PUSHBUTTON, 374, 71, 81, 25, IDC_DISCONNECT);
    g.readButton = createControl(L"BUTTON", L"Read Again", BS_PUSHBUTTON, 462, 71, 81, 25, IDC_READ);

    g.deviceInfo = createControl(
        L"STATIC",
        L"No Active Tag connected. New COM ports are probed automatically.",
        SS_LEFT,
        23,
        112,
        846,
        20,
        IDC_DEVICE_INFO);

    createControl(L"STATIC", L"Product", SS_LEFT, 23, 142, 65, 18, 0);
    g.productSelector = createControl(
        kProductSelectorClass,
        L"",
        0,
        88,
        137,
        423,
        34,
        IDC_PRODUCT_SELECTOR);

    createControl(L"STATIC", L"Profile", SS_LEFT, 23, 187, 117, 18, 0);
    g.groupCombo = createControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST,
        142,
        183,
        270,
        350,
        IDC_GROUP);
    populateProfileCombo();

    createControl(L"STATIC", L"LED Active IDs", SS_LEFT, 25, 218, 227, 18, 0);
    createControl(L"STATIC", L"General Settings", SS_LEFT, 285, 218, 227, 18, 0);

    createFieldUi("D0", L"LED 0 Active ID", 25, 0);
    createFieldUi("D1", L"LED 1 Active ID", 25, 1);
    createFieldUi("D2", L"LED 2 Active ID", 25, 2);
    createFieldUi("D3", L"LED 3 Active ID", 25, 3);
    createFieldUi("D4", L"LED 4 Active ID", 25, 4);
    createFieldUi("D5", L"LED 5 Active ID", 25, 5);
    createFieldUi("D6", L"LED 6 Active ID", 25, 6);
    createFieldUi("D7", L"LED 7 Active ID", 25, 7);

    createFieldUi("2", L"Uplink ID", 285, 0);
    createFieldUi("3", L"RF Channel", 285, 1);
    createFieldUi("4", L"LED Brightness", 285, 2);
    createFieldUi("5", L"On While Charging", 285, 3);

    g.exportButton = createControl(L"BUTTON", L"Export Config", BS_PUSHBUTTON, 23, 635, 97, 28, IDC_EXPORT);
    g.importButton = createControl(L"BUTTON", L"Import Config", BS_PUSHBUTTON, 128, 635, 97, 28, IDC_IMPORT);
    g.saveButton = createControl(L"BUTTON", L"Save to Active Tag", BS_DEFPUSHBUTTON, 367, 635, 144, 28, IDC_SAVE);

    createControl(L"STATIC", L"Serial communication log", SS_LEFT, 554, 151, 203, 18, 0);
    g.log = createControl(
        L"EDIT",
        L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        554,
        174,
        357,
        489,
        IDC_LOG,
        WS_EX_CLIENTEDGE);

    g.tag.setLogCallback([](const std::string& message) {
        appendLog(utf8ToWide(message));
    });
    appendLog(L"ActiveTAG Configurator started.");
    appendLog(L"Log file: " + g.logPath.wstring());
    appendLog(L"Waiting for an Active Tag...");
    applyTheme(false);
    setBusy(false);
    refreshPorts();
    SetTimer(window, kPortTimer, kPortScanIntervalMs, nullptr);
}

void drawProductSelector(HWND control, HDC dc) {
    RECT client{};
    GetClientRect(control, &client);
    FillRect(dc, &client, g.backgroundBrush ? g.backgroundBrush : GetSysColorBrush(COLOR_WINDOW));

    RECT frame = client;
    InflateRect(&frame, -1, -1);
    const COLORREF frameColor =
        g.theme == ThemeMode::Dark ? RGB(44, 50, 58) : RGB(238, 242, 247);
    const COLORREF borderColor =
        g.theme == ThemeMode::Dark ? RGB(80, 90, 102) : RGB(194, 204, 216);
    HBRUSH frameBrush = CreateSolidBrush(frameColor);
    HPEN framePen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBrush = SelectObject(dc, frameBrush);
    HGDIOBJ oldPen = SelectObject(dc, framePen);
    RoundRect(dc, frame.left, frame.top, frame.right, frame.bottom, 18, 18);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(framePen);
    DeleteObject(frameBrush);

    constexpr int segmentCount = 3;
    const int gap = 4;
    RECT inner = frame;
    InflateRect(&inner, -4, -4);
    const int segmentWidth = (inner.right - inner.left - gap * (segmentCount - 1)) / segmentCount;
    const int selectedIndex = productIndex(g.product);

    for (int index = 0; index < segmentCount; ++index) {
        RECT segment = inner;
        segment.left = inner.left + index * (segmentWidth + gap);
        segment.right = index == segmentCount - 1 ? inner.right : segment.left + segmentWidth;
        const ProductType product = productFromIndex(index);
        const bool selected = index == selectedIndex;

        if (selected) {
            HBRUSH selectedBrush = CreateSolidBrush(g.accentColor);
            HPEN selectedPen = CreatePen(PS_SOLID, 1, g.accentColor);
            oldBrush = SelectObject(dc, selectedBrush);
            oldPen = SelectObject(dc, selectedPen);
            RoundRect(dc, segment.left, segment.top, segment.right, segment.bottom, 14, 14);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(selectedPen);
            DeleteObject(selectedBrush);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, selected ? g.accentTextColor : g.textColor);
        HGDIOBJ oldFont = SelectObject(dc, selected && g.tabFont ? g.tabFont : g.normalFont);
        DrawTextW(
            dc,
            productLabel(product),
            -1,
            &segment,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
    }
}

int hitTestProductSelector(HWND control, LPARAM lParam) {
    RECT client{};
    GetClientRect(control, &client);
    const int x = GET_X_LPARAM(lParam);
    RECT inner = client;
    InflateRect(&inner, -5, -5);
    if (x < inner.left || x >= inner.right) {
        return productIndex(g.product);
    }
    const int segmentWidth = (inner.right - inner.left) / 3;
    const int relativeX = static_cast<int>(x - inner.left);
    return std::clamp(relativeX / std::max(1, segmentWidth), 0, 2);
}

LRESULT CALLBACK productSelectorProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(control, &paint);
            drawProductSelector(control, dc);
            EndPaint(control, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(control);
            selectProduct(productFromIndex(hitTestProductSelector(control, lParam)));
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        default:
            return DefWindowProcW(control, message, wParam, lParam);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            createUi(window);
            return 0;
        case WM_TIMER:
            if (wParam == kPortTimer && !g.busy) {
                refreshPorts();
            }
            return 0;
        case WM_ERASEBKGND: {
            RECT area{};
            GetClientRect(window, &area);
            FillRect(
                reinterpret_cast<HDC>(wParam),
                &area,
                g.backgroundBrush ? g.backgroundBrush : GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            wchar_t className[16]{};
            GetClassNameW(control, className, static_cast<int>(std::size(className)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, IsWindowEnabled(control) ? g.textColor : g.mutedTextColor);
            if (_wcsicmp(className, L"Edit") == 0) {
                SetBkMode(dc, OPAQUE);
                SetBkColor(dc, g.editColor);
                return reinterpret_cast<LRESULT>(
                    g.editBrush ? g.editBrush : GetSysColorBrush(COLOR_WINDOW));
            }
            return reinterpret_cast<LRESULT>(
                g.backgroundBrush ? g.backgroundBrush : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, g.textColor);
            SetBkColor(dc, g.editColor);
            return reinterpret_cast<LRESULT>(
                g.editBrush ? g.editBrush : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_ACTIVE_TAG_FOUND: {
            std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lParam));
            g.probeRunning = false;
            if (!g.tag.isConnected() && path) {
                SetWindowTextW(g.portCombo, path->c_str());
                appendLog(L"Active Tag detected on " + *path + L".\r\n");
                connectSelectedPort();
            }
            return 0;
        }
        case WM_ACTIVE_TAG_PROBE_FINISHED:
            g.probeRunning = false;
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            switch (id) {
                case IDC_REFRESH_PORTS:
                    refreshPorts();
                    return 0;
                case IDC_CONNECT:
                    connectSelectedPort();
                    return 0;
                case IDC_DISCONNECT:
                    g.tag.disconnect();
                    SetWindowTextW(
                        g.deviceInfo,
                        L"No Active Tag connected. New COM ports are probed automatically.");
                    setBusy(false);
                    appendLog(L"Disconnected.\r\n");
                    return 0;
                case IDC_READ:
                    try {
                        setBusy(true);
                        renderSnapshot(g.tag.read());
                    } catch (const std::exception& error) {
                        setBusy(false);
                        showError(error.what());
                    }
                    return 0;
                case IDC_EXPORT:
                    exportConfig();
                    return 0;
                case IDC_IMPORT:
                    importConfig();
                    return 0;
                case IDC_SAVE:
                    saveToTag();
                    return 0;
                case IDC_GROUP:
                    if (notification == CBN_SELCHANGE) {
                        applyLabelGroup(static_cast<int>(SendMessageW(g.groupCombo, CB_GETCURSEL, 0, 0)));
                    }
                    return 0;
                case IDC_THEME:
                    if (notification == CBN_SELCHANGE) {
                        g.theme = SendMessageW(g.themeCombo, CB_GETCURSEL, 0, 0) == 1
                            ? ThemeMode::Dark
                            : ThemeMode::Light;
                        applyTheme();
                    }
                    return 0;
                default:
                    if (id >= IDC_FIRST_FIELD &&
                        id < IDC_FIRST_FIELD + static_cast<int>(g.fields.size()) &&
                        notification == EN_CHANGE) {
                        FieldUi& field = g.fields[id - IDC_FIRST_FIELD];
                        if (field.isLedId) {
                            long long value = 0;
                            if (getEditNumber(field.edit, value)) {
                                updateLedDecimal(field, value);
                            } else {
                                SetWindowTextW(field.note, L"Decimal: invalid value");
                            }
                        }
                    }
                    return 0;
            }
        }
        case WM_DESTROY:
            KillTimer(window, kPortTimer);
            g.tag.disconnect();
            if (g.probeThread.joinable()) {
                g.probeThread.join();
            }
            if (g.normalFont) {
                DeleteObject(g.normalFont);
            }
            if (g.titleFont) {
                DeleteObject(g.titleFont);
            }
            if (g.tabFont) {
                DeleteObject(g.tabFont);
            }
            appendLog(L"ActiveTAG Configurator stopped.");
            if (g.logFile.is_open()) {
                g.logFile.flush();
                g.logFile.close();
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (!openLogFile()) {
        MessageBoxW(
            nullptr,
            L"ActiveTAG-Configurator.log could not be opened next to the executable. "
            L"Move the application to a writable folder and try again.",
            kAppTitle,
            MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_ACTIVETAG),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_ACTIVETAG),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    WNDCLASSEXW productSelectorClass{};
    productSelectorClass.cbSize = sizeof(productSelectorClass);
    productSelectorClass.hInstance = instance;
    productSelectorClass.lpfnWndProc = productSelectorProc;
    productSelectorClass.lpszClassName = kProductSelectorClass;
    productSelectorClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    productSelectorClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    if (!RegisterClassExW(&productSelectorClass)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        950,
        728,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        g.logFile.close();
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    deleteBrush(g.backgroundBrush);
    deleteBrush(g.panelBrush);
    deleteBrush(g.editBrush);
    return static_cast<int>(message.wParam);
}
