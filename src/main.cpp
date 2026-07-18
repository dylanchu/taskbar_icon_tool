// taskbar_icon CLI - command-line front-end for the taskbar overlay library.
#include "taskbar/tb_icon.h"
#include "taskbar/config.h"
#include "taskbar/daemon.h"
#include "CLI11/CLI11.hpp"

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

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

static std::wstring getExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return taskbar::cfgDirOf(path);
}

// ===== Config-file mode (shared with daemon via taskbar:: namespace) =====

static int cmdApplyConfig(const std::wstring& iniPath) {
    auto rules = taskbar::parseConfigFile(iniPath);
    if (rules.empty()) {
        werrln(L"No rules found in config: " + iniPath);
        return 1;
    }

    wprintln(L"Loaded " + std::to_wstring(rules.size()) + L" rule(s) from config.");
    wprintln();

    std::wstring baseDir = taskbar::cfgDirOf(iniPath);
    auto infoLog = [](const std::wstring& s) { wprintln(s); };
    auto errLog = [](const std::wstring& s) { werrln(s); };

    int applied = taskbar::applyRulesToWindows(rules, baseDir, infoLog, errLog);
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
    COLORREF color = taskbar::parseConfigColor(to_wstring(v.color.empty() ? "red" : v.color));
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
    COLORREF color = taskbar::parseConfigColor(to_wstring(v.color.empty() ? "red" : v.color));
    int applied = 0, failed = 0;

    for (HWND hwnd : hwnds) {
        HICON iconForHwnd = hIcon;

        // Auto-text: first char of each window's real title.
        if (autoText) {
            std::wstring wt = taskbar::cfgTrim(taskbar::get_window_title(hwnd));
            if (!wt.empty()) {
                iconForHwnd = taskbar::create_badge_icon(std::wstring(1, wt[0]), color);
            }
            if (!iconForHwnd) {
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
        std::wstring appId = taskbar::makeAumid(hwnd, pid, customPrefix);

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
    // No arguments: daemon mode (system tray resident).
    // Hide the console window — when launched from Explorer a new console
    // is created; when launched from cmd.exe this hides the parent console
    // only if we're the sole process attached.
    if (argc < 2) {
        HWND hCon = GetConsoleWindow();
        if (hCon) {
            DWORD procs;
            if (GetConsoleProcessList(&procs, 1) <= 1)
                ShowWindow(hCon, SW_HIDE);
        }
        std::wstring exeDir = getExeDir();
        std::wstring iniPath = exeDir + L"\\tb_icon.ini";
        return taskbar::runDaemon(iniPath);
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // "once" subcommand: apply config rules and exit
    // Handled before CLI11 to keep it simple.
    if (argc >= 2 && wcscmp(argv[1], L"once") == 0) {
        taskbar::scoped_com com;
        std::wstring iniPath;
        // Optional: tb_icon once <path-to-ini>
        if (argc >= 3) {
            iniPath = argv[2];
        } else {
            iniPath = getExeDir() + L"\\tb_icon.ini";
        }
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
