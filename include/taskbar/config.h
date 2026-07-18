// taskbar/config.h - Shared config parsing and rule application.
//
// Used by both the CLI front-end (main.cpp) and the daemon (daemon.cpp).
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

namespace taskbar {

// One rule from a [section] in tb_icon.ini.
struct ConfigRule {
    std::wstring process_name;  // section name: exact process name match
    std::wstring title;         // window title filter (substring, "*" = wildcard)
    std::wstring icon;          // icon file path (.ico/.exe/.dll)
    std::wstring text;          // text badge (mutually exclusive with icon)
    std::wstring color;         // badge color: #RRGGBB, R,G,B, or name
    std::wstring desc;          // accessibility description
};

// Parse a tb_icon.ini file. Returns empty vector if file cannot be opened
// or contains no valid sections.
std::vector<ConfigRule> parseConfigFile(const std::wstring& path);

// Resolve an overlay icon for a window from a config rule.
// Priority: icon(file) > text > auto-text (first char of window title).
// Returns nullptr if icon cannot be created. Caller owns the HICON.
HICON resolveIconForHwnd(const ConfigRule& rule,
                          const std::wstring& baseDir,
                          HWND hwnd);

// Log callback type for applyRulesToWindows.
using LogFn = std::function<void(const std::wstring&)>;

// Apply all rules to currently-open windows.
// - Clears existing overlays for every process mentioned in the rules.
// - Iterates rules, finds matching windows, applies overlay + ungrouping.
// Returns count of successfully applied overlays.
int applyRulesToWindows(const std::vector<ConfigRule>& rules,
                        const std::wstring& baseDir,
                        LogFn infoLog = nullptr,
                        LogFn errLog = nullptr);

// Parse a color string (#RRGGBB, "R,G,B", or named color).
COLORREF parseConfigColor(const std::wstring& str);

// Build an AppUserModelID for ungrouping: ProcessName.HWND_hex.
// prefix overrides the process-name portion (empty = auto-detect).
std::wstring makeAumid(HWND hwnd, DWORD pid, const std::wstring& prefix = L"");

// String utilities (exposed for use by daemon).
std::wstring cfgToLower(std::wstring s);
std::wstring cfgTrim(const std::wstring& s);
std::wstring cfgDirOf(const std::wstring& path);
std::wstring cfgStripExt(const std::wstring& name);

} // namespace taskbar
