// Implementation of config parsing and rule application.
#include "taskbar/config.h"
#include "taskbar/tb_icon.h"

#include <windows.h>
#include <fstream>
#include <algorithm>
#include <set>
#include <cwctype>

namespace taskbar {

// ===== String utilities =====

std::wstring cfgToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::wstring cfgTrim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring cfgDirOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? path.substr(0, pos) : L".";
}

std::wstring cfgStripExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return (dot != std::wstring::npos) ? name.substr(0, dot) : name;
}

// ===== Color parsing =====

COLORREF parseConfigColor(const std::wstring& str) {
    if (str.empty()) return RGB(220, 38, 38); // default red

    std::wstring s = str;
    if (s[0] == L'#') s = s.substr(1);

    // #RRGGBB
    if (s.length() == 6) {
        try {
            int r = std::stoi(s.substr(0, 2), nullptr, 16);
            int g = std::stoi(s.substr(2, 2), nullptr, 16);
            int b = std::stoi(s.substr(4, 2), nullptr, 16);
            return RGB(r, g, b);
        } catch (...) {}
    }

    // R,G,B
    if (s.find(L',') != std::wstring::npos) {
        try {
            size_t pos = 0;
            int r = std::stoi(s, &pos); pos++;
            int g = std::stoi(s.substr(pos), &pos); pos++;
            int b = std::stoi(s.substr(pos));
            return RGB(r, g, b);
        } catch (...) {}
    }

    // Named colors
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
    std::wstring lower = cfgToLower(s);
    for (auto& n : named) {
        if (lower == n.name) return n.color;
    }

    return RGB(220, 38, 38); // fallback red
}

// ===== INI config parsing =====

std::vector<ConfigRule> parseConfigFile(const std::wstring& path) {
    std::wifstream file(path);
    if (!file.is_open()) return {};

    file.imbue(std::locale("en_US.UTF-8"));

    std::vector<ConfigRule> rules;
    ConfigRule current;
    bool in_section = false;

    std::wstring line;
    while (std::getline(file, line)) {
        std::wstring t = cfgTrim(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;

        if (t[0] == L'[') {
            if (in_section) rules.push_back(current);
            size_t end = t.find(L']');
            current = ConfigRule{};
            current.process_name = (end != std::wstring::npos)
                ? cfgTrim(t.substr(1, end - 1)) : cfgTrim(t.substr(1));
            in_section = true;
            continue;
        }

        if (!in_section) continue;

        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = cfgToLower(cfgTrim(t.substr(0, eq)));
        std::wstring val = cfgTrim(t.substr(eq + 1));

        if (key == L"title")       current.title = val;
        else if (key == L"icon")   current.icon = val;
        else if (key == L"text")   current.text = val;
        else if (key == L"color")  current.color = val;
        else if (key == L"desc")   current.desc = val;
    }
    if (in_section) rules.push_back(current);
    return rules;
}

// ===== Icon resolution =====

namespace {

// Create a badge from the first character of a window's title.
// Returns nullptr if title is empty (after trimming) or creation fails.
HICON createAutoTextBadge(HWND hwnd, COLORREF color) {
    std::wstring wt = cfgTrim(get_window_title(hwnd));
    if (wt.empty()) return nullptr;
    return create_badge_icon(std::wstring(1, wt[0]), color);
}

} // anonymous namespace

std::wstring makeAumid(HWND hwnd, DWORD pid, const std::wstring& prefix) {
    std::wstring base = prefix.empty()
        ? cfgStripExt(get_process_name(pid))
        : prefix;
    return base + L"." + std::to_wstring((uintptr_t)hwnd);
}

HICON resolveIconForHwnd(const ConfigRule& rule,
                          const std::wstring& baseDir,
                          HWND hwnd) {
    COLORREF color = parseConfigColor(rule.color.empty() ? L"#DC2626" : rule.color);

    // Priority 1: icon file
    if (!rule.icon.empty()) {
        std::wstring path = rule.icon;
        if (path.size() >= 2 && path[1] != L':') {
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                path = baseDir + L"\\" + path;
            }
        }
        return load_icon_from_file(path, 0, 32);
    }

    // Priority 2: text badge
    if (!rule.text.empty()) {
        return create_badge_icon(rule.text, color);
    }

    // Priority 3: auto-text (first char of window title)
    return createAutoTextBadge(hwnd, color);
}

// ===== Rule application =====

int applyRulesToWindows(const std::vector<ConfigRule>& rules,
                        const std::wstring& baseDir,
                        LogFn infoLog,
                        LogFn errLog) {
    if (rules.empty()) return 0;

    auto log = [&](const std::wstring& msg, bool isErr = false) {
        if (isErr && errLog) errLog(msg);
        else if (infoLog) infoLog(msg);
    };

    // Clear existing overlays for all processes mentioned in the config.
    {
        std::set<std::wstring> procNames;
        for (const auto& rule : rules) {
            if (!rule.process_name.empty()) {
                procNames.insert(cfgToLower(rule.process_name));
            }
        }
        for (const auto& pname : procNames) {
            auto hwnds = find_window(L"", pname);
            for (HWND hwnd : hwnds) {
                clear_overlay_by_hwnd(hwnd);
            }
        }
    }

    int applied = 0;
    int skipped = 0;
    std::set<HWND> ungrouped;

    for (const auto& rule : rules) {
        std::wstring titleFilter = (rule.title == L"*") ? L"" : rule.title;
        auto hwnds = find_window(titleFilter, rule.process_name);

        if (hwnds.empty()) {
            log(L"[SKIP] " + rule.process_name + L" - no matching window");
            skipped++;
            continue;
        }

        for (HWND hwnd : hwnds) {
            // Ungroup: set unique AppUserModelID (once per window)
            if (ungrouped.insert(hwnd).second) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                set_app_user_model_id(hwnd, makeAumid(hwnd, pid));
            }

            HICON icon = resolveIconForHwnd(rule, baseDir, hwnd);
            if (!icon) {
                skipped++;
                continue;
            }

            HRESULT hr = set_overlay_by_hwnd(hwnd, icon, rule.desc);
            destroy_icon(icon);

            if (SUCCEEDED(hr)) {
                log(L"[OK]   " + rule.process_name + L" -> " + get_window_title(hwnd));
                applied++;
            } else {
                log(L"[ERR]  " + rule.process_name + L" - SetOverlayIcon failed (0x"
                    + std::to_wstring(hr) + L")", true);
                skipped++;
            }
        }
    }

    if (infoLog) {
        infoLog(L"\nApplied: " + std::to_wstring(applied) + L", Skipped: " + std::to_wstring(skipped));
    }

    return applied;
}

} // namespace taskbar
