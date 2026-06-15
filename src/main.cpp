#include "active_tag.hpp"
#include "generated/resource.h"
#include "generated/version.hpp"

#include <windows.h>
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
    HWND themeCombo = nullptr;
    HWND deviceInfo = nullptr;
    HWND log = nullptr;
    HFONT normalFont = nullptr;
    HFONT titleFont = nullptr;
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
    ThemeMode theme = ThemeMode::Light;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HBRUSH editBrush = nullptr;
    COLORREF textColor = RGB(28, 32, 36);
    COLORREF mutedTextColor = RGB(83, 91, 99);
    COLORREF backgroundColor = RGB(246, 247, 249);
    COLORREF panelColor = RGB(255, 255, 255);
    COLORREF editColor = RGB(255, 255, 255);
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
    } else {
        g.backgroundColor = RGB(246, 247, 249);
        g.panelColor = RGB(255, 255, 255);
        g.editColor = RGB(255, 255, 255);
        g.textColor = RGB(28, 32, 36);
        g.mutedTextColor = RGB(83, 91, 99);
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

void setEditNumber(HWND edit, long long value) {
    SetWindowTextW(edit, std::to_wstring(value).c_str());
}

bool getEditNumber(HWND edit, long long& value) {
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t* end = nullptr;
    value = wcstoll(buffer, &end, 0);
    return end != buffer && *end == L'\0';
}

void renderSnapshot(const activetag::Snapshot& snapshot) {
    g.snapshot = snapshot;
    const std::wstring group = snapshot.detectedLabelGroup
        ? std::to_wstring(*snapshot.detectedLabelGroup)
        : L"Custom";
    const std::wstring info =
        L"Serial: " + metadataValue(snapshot, "serialNum") +
        L"     Firmware: " + utf8ToWide(snapshot.firmwareVersion) +
        L"     Hardware: " + metadataValue(snapshot, "hardwareRev") +
        L"     COM: " + g.tag.portPath() +
        L"     Label Group: " + group;
    SetWindowTextW(g.deviceInfo, info.c_str());

    for (const FieldUi& fieldUi : g.fields) {
        const auto it = snapshot.fields.find(fieldUi.id);
        const bool available =
            it != snapshot.fields.end() && it->second.supported && it->second.hasNumericValue;
        ShowWindow(fieldUi.label, available ? SW_SHOW : SW_HIDE);
        ShowWindow(fieldUi.edit, available ? SW_SHOW : SW_HIDE);
        ShowWindow(fieldUi.note, available ? SW_SHOW : SW_HIDE);
        if (available) {
            setEditNumber(fieldUi.edit, it->second.numericValue);
            SetWindowTextW(
                fieldUi.note,
                it->second.documented
                    ? utf8ToWide("Field [" + fieldUi.id + "]").c_str()
                    : L"Advanced: not documented for firmware 2.x");
        }
    }

    SendMessageW(
        g.groupCombo,
        CB_SETCURSEL,
        snapshot.detectedLabelGroup ? *snapshot.detectedLabelGroup + 1 : 0,
        0);
    g.ledFieldsLocked = snapshot.detectedLabelGroup.has_value();
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
        for (const FieldUi& field : g.fields) {
            if (fields.contains(field.id) && fields.at(field.id).is_number_integer()) {
                setEditNumber(field.edit, fields.at(field.id).get<long long>());
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
    if (selection > 6) {
        return;
    }
    const int group = selection - 1;
    for (int led = 0; led < 8; ++led) {
        for (const FieldUi& field : g.fields) {
            if (field.id == "D" + std::to_string(led)) {
                setEditNumber(field.edit, activetag::ActiveTag::labelGroups()[group][led]);
            }
        }
    }
    for (const FieldUi& field : g.fields) {
        if (field.id == "2") {
            setEditNumber(field.edit, group);
        }
    }
    g.ledFieldsLocked = true;
    updateFieldEnableState();
}

void createFieldUi(const std::string& id, const wchar_t* title, int column, int row) {
    const int x = 25 + column * 254;
    const int y = 185 + row * 49;
    FieldUi field;
    field.id = id;
    field.isLedId = id.starts_with("D");
    field.label = createControl(L"STATIC", title, SS_LEFT, x, y, 139, 17, 0);
    field.edit = createControl(
        L"EDIT",
        L"",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
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

    createControl(L"STATIC", L"Label Group Profile", SS_LEFT, 23, 151, 117, 18, 0);
    g.groupCombo = createControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST,
        142,
        148,
        210,
        180,
        IDC_GROUP);
    SendMessageW(g.groupCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
    for (int group = 0; group < 6; ++group) {
        const std::wstring name =
            L"CAM" + std::to_wstring(group + 1) +
            L" - Label Group " + std::to_wstring(group);
        SendMessageW(g.groupCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(g.groupCombo, CB_SETCURSEL, 0, 0);

    createFieldUi("2", L"Uplink ID", 0, 0);
    createFieldUi("3", L"RF Channel", 1, 0);
    createFieldUi("4", L"LED Brightness", 0, 1);
    createFieldUi("5", L"On While Charging", 1, 1);
    createFieldUi("D0", L"LED 0 Active ID", 0, 2);
    createFieldUi("D1", L"LED 1 Active ID", 1, 2);
    createFieldUi("D2", L"LED 2 Active ID", 0, 3);
    createFieldUi("D3", L"LED 3 Active ID", 1, 3);
    createFieldUi("D4", L"LED 4 Active ID", 0, 4);
    createFieldUi("D5", L"LED 5 Active ID", 1, 4);
    createFieldUi("D6", L"LED 6 Active ID", 0, 5);
    createFieldUi("D7", L"LED 7 Active ID", 1, 5);

    g.exportButton = createControl(L"BUTTON", L"Export Config", BS_PUSHBUTTON, 23, 551, 97, 28, IDC_EXPORT);
    g.importButton = createControl(L"BUTTON", L"Import Config", BS_PUSHBUTTON, 128, 551, 97, 28, IDC_IMPORT);
    g.saveButton = createControl(L"BUTTON", L"Save to Active Tag", BS_DEFPUSHBUTTON, 367, 551, 144, 28, IDC_SAVE);

    createControl(L"STATIC", L"Serial communication log", SS_LEFT, 554, 151, 203, 18, 0);
    g.log = createControl(
        L"EDIT",
        L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        554,
        174,
        357,
        405,
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

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        950,
        644,
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
