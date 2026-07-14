// taskbar_icon CLI - command-line front-end for the taskbar overlay library.
#include "taskbar/tb_icon.h"
#include "CLI11/CLI11.hpp"

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include <cwctype>
#include <fstream>
#include <sstream>

// ===== Helpers =====

static std::wstring to_wstring(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

static std::string to_string(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static void wprint(const std::wstring& ws) { std::cout << to_string(ws); }
static void wprintln(const std::wstring& ws = L"") { std::cout << to_string(ws) << "\n"; }
static void werrln(const std::wstring& ws) { std::cerr << to_string(ws) << "\n"; }

static std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring strip_ext(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return (dot != std::wstring::npos) ? name.substr(0, dot) : name;
}

static std::wstring dir_of(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? path.substr(0, pos) : L".";
}

static std::wstring make_aumid(HWND hwnd, DWORD pid, const std::wstring& prefix = L"") {
    std::wstring base = prefix.empty()
        ? strip_ext(taskbar::get_process_name(pid))
        : prefix;
    return base + L"." + std::to_wstring((uintptr_t)hwnd);
}

static std::wstring getExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return dir_of(path);
}

// ===== Color Parsing =====

static COLORREF parseColor(const std::wstring& str) {
    if (str.empty()) return RGB(220, 38, 38);

    std::wstring s = str;
    if (s[0] == L'#') s = s.substr(1);

    if (s.length() == 6) {
        try {
            int r = std::stoi(s.substr(0, 2), nullptr, 16);
            int g = std::stoi(s.substr(2, 2), nullptr, 16);
            int b = std::stoi(s.substr(4, 2), nullptr, 16);
            return RGB(r, g, b);
        } catch (...) {}
    }

    if (s.find(L',') != std::wstring::npos) {
        try {
            size_t pos = 0;
            int r = std::stoi(s, &pos); pos++;
            int g = std::stoi(s.substr(pos), &pos); pos++;
            int b = std::stoi(s.substr(pos));
            return RGB(r, g, b);
        } catch (...) {}
    }

    static const struct { const wchar_t* name; COLORREF color; } named[] = {
        { L"red",    RGB(220, 38, 38) },
        { L"green",  RGB(34, 197, 94) },
        { L"blue",   RGB(59, 130, 246) },
        { L"orange", RGB(249, 115, 22) },
        { L"yellow", RGB(234, 179, 8) },
        { L"purple", RGB(168, 85, 247) },
        { L"pink",   RGB(236, 72, 153) },
        { L"black",  RGB(31, 41, 55) },
        { L"white",  RGB(255, 255, 255) },
    };
    std::wstring lower = to_lower(s);
    for (auto& n : named) {
        if (lower == n.name) return n.color;
    }

    return RGB(220, 38, 38);
}

// ===== INI config file (no-param mode) =====

struct ConfigRule {
    std::wstring process_name;
    std::wstring title;
    std::wstring icon;
    std::wstring text;
    std::wstring color;
    std::wstring desc;
};

static std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<ConfigRule> parseConfigFile(const std::wstring& path) {
    std::wifstream file(path);
    if (!file.is_open()) return {};

    file.imbue(std::locale("en_US.UTF-8"));

    std::vector<ConfigRule> rules;
    ConfigRule current;
    bool in_section = false;

    std::wstring line;
    while (std::getline(file, line)) {
        std::wstring t = trim(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;

        if (t[0] == L'[') {
            if (in_section) rules.push_back(current);
            size_t end = t.find(L']');
            current = ConfigRule{};
            current.process_name = (end != std::wstring::npos)
                ? trim(t.substr(1, end - 1)) : trim(t.substr(1));
            in_section = true;
            continue;
        }

        if (!in_section) continue;

        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = to_lower(trim(t.substr(0, eq)));
        std::wstring val = trim(t.substr(eq + 1));

        if (key == L"title")       current.title = val;
        else if (key == L"icon")   current.icon = val;
        else if (key == L"text")   current.text = val;
        else if (key == L"color")  current.color = val;
        else if (key == L"desc")   current.desc = val;
    }
    if (in_section) rules.push_back(current);
    return rules;
}

// Create a badge from the first character of a window's title.
// Returns nullptr if title is empty (after trimming) or GDI+ creation fails.
static HICON createAutoTextBadge(HWND hwnd, COLORREF color) {
    std::wstring wt = trim(taskbar::get_window_title(hwnd));
    if (wt.empty()) return nullptr;
    return taskbar::create_badge_icon(std::wstring(1, wt[0]), color);
}

// Resolve overlay icon for one window: icon(file) > text > auto-text (first
// char of the window's real title). Emits failure diagnostics; nullptr = skip.
static HICON resolveIconForHwnd(const ConfigRule& rule,
                                const std::wstring& baseDir,
                                HWND hwnd) {
    COLORREF color = parseColor(rule.color.empty() ? L"#DC2626" : rule.color);

    if (!rule.icon.empty()) {
        std::wstring path = rule.icon;
        if (path.size() >= 2 && path[1] != L':') {
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                path = baseDir + L"\\" + path;
            }
        }
        HICON icon = taskbar::load_icon_from_file(path, 0, 32);
        if (!icon) {
            werrln(L"[ERR]  " + rule.process_name
                   + L" - failed to load icon: " + path);
        }
        return icon;
    }
    if (!rule.text.empty()) {
        return taskbar::create_badge_icon(rule.text, color);
    }

    // Auto-text: first char of the window's real title.
    HICON icon = createAutoTextBadge(hwnd, color);
    if (!icon) {
        std::wstring wt = trim(taskbar::get_window_title(hwnd));
        if (wt.empty())
            wprintln(L"[SKIP] " + rule.process_name + L" - window has empty title");
        else
            werrln(L"[ERR]  " + rule.process_name + L" - failed to create badge");
    }
    return icon;
}

static int cmdApplyConfig(const std::wstring& iniPath) {
    auto rules = parseConfigFile(iniPath);
    if (rules.empty()) {
        werrln(L"No rules found in config: " + iniPath);
        return 1;
    }

    wprintln(L"Loaded " + std::to_wstring(rules.size()) + L" rule(s) from config.");
    wprintln();

    std::wstring baseDir = dir_of(iniPath);

    // Phase 1: Clear existing overlays for all processes mentioned in the config.
    {
        std::set<std::wstring> procNames;
        for (const auto& rule : rules) {
            if (!rule.process_name.empty()) {
                procNames.insert(to_lower(rule.process_name));
            }
        }
        for (const auto& pname : procNames) {
            auto hwnds = taskbar::find_window(L"", pname);
            for (HWND hwnd : hwnds) {
                taskbar::clear_overlay_by_hwnd(hwnd);
            }
        }
        wprintln(L"Cleared existing overlays for "
                 + std::to_wstring(procNames.size()) + L" process(es).");
        wprintln();
    }

    int applied = 0;
    int skipped = 0;
    std::set<HWND> ungrouped;

    for (const auto& rule : rules) {
        // title = * matches all instances; other values are substring filters.
        std::wstring titleFilter = (rule.title == L"*") ? L"" : rule.title;
        auto hwnds = taskbar::find_window(titleFilter, rule.process_name);

        if (hwnds.empty()) {
            wprintln(L"[SKIP] " + rule.process_name + L" - no matching window");
            skipped++;
            continue;
        }

        for (HWND hwnd : hwnds) {
            if (ungrouped.insert(hwnd).second) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                taskbar::set_app_user_model_id(hwnd, make_aumid(hwnd, pid));
            }

            HICON icon = resolveIconForHwnd(rule, baseDir, hwnd);
            if (!icon) {
                skipped++;
                continue;
            }

            HRESULT hr = taskbar::set_overlay_by_hwnd(hwnd, icon, rule.desc);
            taskbar::destroy_icon(icon);

            if (SUCCEEDED(hr)) {
                wprintln(L"[OK]   " + rule.process_name + L" -> "
                         + taskbar::get_window_title(hwnd));
                applied++;
            } else {
                werrln(L"[ERR]  " + rule.process_name
                       + L" - SetOverlayIcon failed (0x"
                       + std::to_wstring(hr) + L")");
                skipped++;
            }
        }
    }

    wprintln(L"\nApplied: " + std::to_wstring(applied)
             + L", Skipped: " + std::to_wstring(skipped));
    return (applied > 0) ? 0 : 1;
}

// ===== Command handlers =====
// Each subcommand owns its own vars struct to avoid field bleed between commands.

// Shared window resolution: pname > pid > hwnd_str. Returns empty vector if none match.
static std::vector<HWND> resolveWindows(const std::string& pname,
                                         const std::string& title,
                                         int pid,
                                         const std::string& hwndStr) {
    if (!pname.empty())
        return taskbar::find_window(to_wstring(title), to_wstring(pname));
    if (pid != 0) {
        HWND hwnd = taskbar::find_window_by_pid((DWORD)pid);
        if (hwnd) return {hwnd};
    }
    if (!hwndStr.empty()) {
        try {
            HWND hwnd = (HWND)(uintptr_t)std::stoull(hwndStr, nullptr, 0);
            if (hwnd) return {hwnd};
        } catch (...) {}
    }
    return {};
}

struct SetVars {
    std::string pname, title, hwnd_str;
    int pid = 0;
    std::string file, text, color, desc;
    int index = 0;
    int size = 32;
};

struct ClearVars {
    std::string pname, title, hwnd_str;
    int pid = 0;
};

struct UngroupVars {
    std::string pname;
    std::string app_id;
};

struct RegroupVars {
    std::string pname;
};

struct ListVars {
    std::string pname;
};

// Resolve a single shared icon for file/text mode. Caller must guarantee that
// at least one of file or text is set (auto-text path uses createAutoTextBadge).
static HICON resolveIcon(const SetVars& v) {
    if (!v.file.empty())
        return taskbar::load_icon_from_file(to_wstring(v.file), v.index, v.size);
    // v.text is non-empty by caller contract.
    COLORREF color = parseColor(to_wstring(v.color.empty() ? "red" : v.color));
    return taskbar::create_badge_icon(to_wstring(v.text), color);
}

static int cmdSetOverlay(const SetVars& v) {
    auto hwnds = resolveWindows(v.pname, v.title, v.pid, v.hwnd_str);
    if (hwnds.empty()) {
        werrln(L"Error: No matching window found.");
        return 1;
    }

    bool autoText = v.file.empty() && v.text.empty();
    HICON hIcon = nullptr;

    // File/text mode: resolve one shared icon upfront.
    if (!autoText) {
        hIcon = resolveIcon(v);
        if (!hIcon) {
            werrln(L"Error: Failed to load or create icon.");
            return 1;
        }
    }

    std::wstring desc = to_wstring(v.desc);
    COLORREF color = parseColor(to_wstring(v.color.empty() ? "red" : v.color));
    int applied = 0, failed = 0;

    for (HWND hwnd : hwnds) {
        HICON iconForHwnd = hIcon;

        // Auto-text: first char of each window's real title.
        if (autoText) {
            iconForHwnd = createAutoTextBadge(hwnd, color);
            if (!iconForHwnd) {
                std::wstring wt = trim(taskbar::get_window_title(hwnd));
                if (wt.empty())
                    wprintln(L"[SKIP] window has empty title");
                else
                    werrln(L"[ERR] failed to create badge for: "
                           + taskbar::get_window_title(hwnd));
                failed++;
                continue;
            }
        }

        HRESULT hr = taskbar::set_overlay_by_hwnd(hwnd, iconForHwnd, desc);

        if (autoText && iconForHwnd) taskbar::destroy_icon(iconForHwnd);

        if (SUCCEEDED(hr)) {
            wprintln(L"[OK] Overlay set for: " + taskbar::get_window_title(hwnd));
            applied++;
        } else {
            werrln(L"[ERR] " + taskbar::get_window_title(hwnd)
                   + L" - failed (0x" + std::to_wstring(hr) + L")");
            failed++;
        }
    }

    if (!autoText && hIcon) taskbar::destroy_icon(hIcon);

    if (hwnds.size() > 1) {
        wprintln(L"\nApplied: " + std::to_wstring(applied)
                 + L", Failed: " + std::to_wstring(failed));
    }
    return (applied > 0) ? 0 : 1;
}

static int cmdClearOverlay(const ClearVars& v) {
    auto hwnds = resolveWindows(v.pname, v.title, v.pid, v.hwnd_str);
    if (hwnds.empty()) {
        werrln(L"Error: No matching window found.");
        return 1;
    }

    int cleared = 0, failed = 0;
    for (HWND hwnd : hwnds) {
        HRESULT hr = taskbar::clear_overlay_by_hwnd(hwnd);
        if (SUCCEEDED(hr)) {
            wprintln(L"[OK] Cleared overlay for: " + taskbar::get_window_title(hwnd));
            cleared++;
        } else {
            werrln(L"[ERR] " + taskbar::get_window_title(hwnd)
                   + L" - failed (0x" + std::to_wstring(hr) + L")");
            failed++;
        }
    }

    if (hwnds.size() > 1) {
        wprintln(L"\nCleared: " + std::to_wstring(cleared)
                 + L", Failed: " + std::to_wstring(failed));
    }
    return (cleared > 0) ? 0 : 1;
}

static int cmdUngroup(const UngroupVars& v) {
    std::wstring procName = to_wstring(v.pname);
    std::wstring customPrefix = to_wstring(v.app_id);
    auto hwnds = taskbar::find_window(L"", procName);

    int set = 0, skipped = 0;
    for (HWND hwnd : hwnds) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring appId = make_aumid(hwnd, pid, customPrefix);

        HRESULT hr = taskbar::set_app_user_model_id(hwnd, appId);
        if (SUCCEEDED(hr)) {
            wprintln(L"[OK] " + appId + L" -> " + taskbar::get_window_title(hwnd));
            set++;
        } else {
            werrln(L"[ERR] " + taskbar::get_window_title(hwnd)
                   + L" - failed (0x" + std::to_wstring(hr) + L")");
            skipped++;
        }
    }

    wprintln(L"\nUngrouped: " + std::to_wstring(set)
             + L", Skipped: " + std::to_wstring(skipped));
    return (set > 0) ? 0 : 1;
}

static int cmdRegroup(const RegroupVars& v) {
    std::wstring procName = to_wstring(v.pname);
    auto hwnds = taskbar::find_window(L"", procName);

    int cleared = 0, alreadyDefault = 0;
    for (HWND hwnd : hwnds) {
        std::wstring oldAppId = taskbar::get_app_user_model_id(hwnd);
        HRESULT hr = taskbar::set_app_user_model_id(hwnd, L"");
        if (SUCCEEDED(hr)) {
            if (oldAppId.empty()) {
                alreadyDefault++;
            } else {
                wprintln(L"[OK] Cleared \"" + oldAppId
                         + L"\" for: " + taskbar::get_window_title(hwnd));
                cleared++;
            }
        } else {
            werrln(L"[ERR] " + taskbar::get_window_title(hwnd)
                   + L" - failed (0x" + std::to_wstring(hr) + L")");
            alreadyDefault++;
        }
    }

    wprintln(L"\nCleared: " + std::to_wstring(cleared)
             + L", Already default: " + std::to_wstring(alreadyDefault));
    return (cleared > 0) ? 0 : 1;
}

static int cmdList(const ListVars& v) {
    std::wstring filterPname = to_wstring(v.pname);

    std::vector<HWND> hwnds = filterPname.empty()
        ? taskbar::find_window()
        : taskbar::find_window(L"", filterPname);

    if (hwnds.empty()) {
        if (filterPname.empty())
            wprintln(L"No visible top-level windows found.");
        else
            wprintln(L"No windows for process \"" + filterPname + L"\".");
        return 0;
    }

    wprintln();
    wprintln(L"HWND            PID     Process                  AUMID                        Title");
    wprintln(L"--------------  ------  -----------------------  ----------------             ----------------");

    for (HWND hwnd : hwnds) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring procName = taskbar::get_process_name(pid);
        std::wstring title = taskbar::get_window_title(hwnd);
        std::wstring aumid = taskbar::get_app_user_model_id(hwnd);

        if (procName.length() > 23) procName = procName.substr(0, 22) + L"\u2026";
        if (aumid.length() > 30) aumid = aumid.substr(0, 29) + L"\u2026";
        if (aumid.empty()) aumid = L"-";

        std::wostringstream ss;
        ss << L"0x" << std::hex << std::setfill(L'0') << std::setw(12)
           << (uintptr_t)hwnd
           << std::dec << std::setfill(L' ')
           << L"  " << std::setw(6) << pid
           << L"  " << std::left << std::setw(23) << procName
           << L"  " << std::setw(30) << aumid
           << L"  " << title
           << std::right;
        wprintln(ss.str());
    }

    wprintln(L"\n" + std::to_wstring(hwnds.size()) + L" window(s) found.");
    return 0;
}

// ===== Main: CLI11 subcommand tree =====

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // No arguments: read tb_icon.ini from exe directory and apply all rules.
    if (argc < 2) {
        taskbar::scoped_com com;
        std::wstring exeDir = getExeDir();
        std::wstring iniPath = exeDir + L"\\tb_icon.ini";
        return cmdApplyConfig(iniPath);
    }

    // Convert wchar_t argv to UTF-8 for CLI11.
    std::vector<std::string> argsUtf8;
    std::vector<char*> argsChar;
    argsChar.push_back((char*)"tb_icon");
    for (int i = 1; i < argc; i++) {
        argsUtf8.push_back(to_string(argv[i]));
    }
    for (auto& s : argsUtf8) {
        argsChar.push_back(s.data());
    }
    int argcUtf8 = (int)argsChar.size();

    taskbar::scoped_com com;

    CLI::App app{"Taskbar Icon Tool - Ungroup/Regroup taskbar buttons; Add/Remove overlay badges"};
    app.require_subcommand(1);

    // CLI11's App::callback takes std::function<void()> — return values are
    // silently dropped. Use a shared exitCode variable to propagate results.
    int exitCode = 0;

    // --- set-overlay ---
    SetVars setV;
    auto* setCmd = app.add_subcommand("set-overlay", "Add an overlay badge to the taskbar button(s)");
    auto* set_pname = setCmd->add_option("--pname", setV.pname, "Process name (exact, case-insensitive). Alone: batch; with --title: dual filter");
    auto* set_title = setCmd->add_option("--title", setV.title, "Window title (partial, case-insensitive). Requires --pname");
    set_title->needs(set_pname);
    setCmd->add_option("--pid", setV.pid, "Target process ID (single window)");
    setCmd->add_option("--hwnd", setV.hwnd_str, "Direct window handle (decimal or 0x-hex)");
    setCmd->add_option("--file", setV.file, "Icon file (.ico, .exe, .dll)");
    setCmd->add_option("--index", setV.index, "Icon index in .exe/.dll (default: 0)");
    setCmd->add_option("--size", setV.size, "Icon size in pixels (default: 32)");
    setCmd->add_option("--text", setV.text, "Generate a text badge (e.g. \"5\", \"99+\"). Omit with --file for auto-text: first char of window title");
    setCmd->add_option("--color", setV.color, "Badge color (applies to --text and auto-text; default: red)");
    setCmd->add_option("--desc", setV.desc, "Accessibility description");
    setCmd->callback([&]() { exitCode = cmdSetOverlay(setV); });

    // --- clear-overlay ---
    ClearVars clrV;
    auto* clrCmd = app.add_subcommand("clear-overlay", "Remove the overlay badge(s)");
    auto* clr_pname = clrCmd->add_option("--pname", clrV.pname, "Process name (exact, case-insensitive). Alone: batch-clear all; with --title: dual filter");
    auto* clr_title = clrCmd->add_option("--title", clrV.title, "Window title (requires --pname)");
    clr_title->needs(clr_pname);
    clrCmd->add_option("--pid", clrV.pid, "Process ID (single window)");
    clrCmd->add_option("--hwnd", clrV.hwnd_str, "Direct window handle");
    clrCmd->callback([&]() { exitCode = cmdClearOverlay(clrV); });

    // --- ungroup ---
    UngroupVars ngV;
    auto* ngCmd = app.add_subcommand("ungroup", "Set AppUserModelID to ungroup taskbar buttons");
    ngCmd->add_option("pname", ngV.pname, "Process name (exact, case-insensitive)")->required();
    ngCmd->add_option("--app_id", ngV.app_id, "Custom AUMID prefix (default: ProcessName.HWND)");
    ngCmd->callback([&]() { exitCode = cmdUngroup(ngV); });

    // --- regroup ---
    RegroupVars rgV;
    auto* rgCmd = app.add_subcommand("regroup", "Reset AppUserModelID (undo ungroup)");
    rgCmd->add_option("pname", rgV.pname, "Process name (exact, case-insensitive)")->required();
    rgCmd->callback([&]() { exitCode = cmdRegroup(rgV); });

    // --- list ---
    ListVars lsV;
    auto* lsCmd = app.add_subcommand("list", "List taskbar-eligible windows");
    lsCmd->add_option("pname", lsV.pname, "Process name filter (exact, case-insensitive)");
    lsCmd->callback([&]() { exitCode = cmdList(lsV); });

    // Parse
    try {
        app.parse(argcUtf8, argsChar.data());
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    return exitCode;
}
