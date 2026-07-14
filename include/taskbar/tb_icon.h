// taskbar/tb_icon.h - Public API for taskbar overlay icons and grouping control.
//
// Usage:
//   1. Link against target `tb_icon_lib` (CMake).
//   2. Call taskbar::scoped_com com; once per thread that uses these APIs.
//   3. Use set_overlay_*() to apply an overlay; clear_overlay_*() to remove.
//   4. Use set_app_user_model_id() to control taskbar grouping.
//
// The taskbar makes an internal copy of the overlay icon, so the HICON
// passed to set_overlay_*() can be destroyed immediately after the call.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace taskbar {

// ===== Lifecycle =====

// RAII wrapper for CoInitializeEx / CoUninitialize.
// Required on any thread that calls the overlay APIs below.
struct scoped_com {
    scoped_com();
    ~scoped_com();
    scoped_com(const scoped_com&) = delete;
    scoped_com& operator=(const scoped_com&) = delete;
private:
    bool m_ok = false;
};

// ===== Window discovery =====

// Find the main taskbar-eligible window for a process.
HWND find_window_by_pid(DWORD pid);

// Find all visible top-level windows matching the given filters.
// - title: case-insensitive partial match on window title. Empty = any.
// - process_name: case-insensitive exact match on process name. Empty = any.
// If both are empty, returns all visible top-level windows.
std::vector<HWND> find_window(const std::wstring& title = L"",
                               const std::wstring& process_name = L"");

// List all visible top-level (unowned) windows for a process.
std::vector<HWND> list_windows_by_pid(DWORD pid);

// Get window title.
std::wstring get_window_title(HWND hwnd);

// Get process image name (e.g. "Notepad2_x64.exe") by PID.
std::wstring get_process_name(DWORD pid);

// ===== Icon creation =====

// Load an icon from a file (.ico, .exe, .dll, .icl).
// Returns HICON (caller owns it) or nullptr on failure.
HICON load_icon_from_file(const std::wstring& path,
                           int index = 0, int size = 32);

// Create a badge icon: filled circle with text drawn on top.
// Returns HICON (caller owns it) or nullptr on failure.
HICON create_badge_icon(const std::wstring& text = L"",
                         COLORREF bg_color = RGB(220, 38, 38),
                         COLORREF text_color = RGB(255, 255, 255),
                         int size = 32);

// Destroy an icon handle and set it to nullptr. Safe to call on nullptr.
void destroy_icon(HICON& icon);

// ===== Overlay (ITaskbarList3::SetOverlayIcon) =====

// Set overlay icon on a window's taskbar button.
// hwnd: target window (any process).
// icon: overlay icon, or nullptr to clear.
// description: accessibility description (may be empty).
// Returns S_OK on success, error HRESULT on failure.
HRESULT set_overlay_by_hwnd(HWND hwnd, HICON icon,
                             const std::wstring& description = L"");

// Clear overlay icon from a window.
HRESULT clear_overlay_by_hwnd(HWND hwnd);

// ===== AppUserModelID (taskbar grouping control) =====

// Set the AppUserModelID on a window to control taskbar grouping.
// Windows with different AUMIDs are shown as separate taskbar buttons
// instead of grouped together. Set to empty string to reset to default.
// Returns S_OK on success, error HRESULT on failure.
HRESULT set_app_user_model_id(HWND hwnd, const std::wstring& appId);

// Get the current AppUserModelID of a window.
// Returns empty string if not set.
std::wstring get_app_user_model_id(HWND hwnd);

// Convenience: find window by PID, then set overlay.
// Returns S_OK on success, E_FAIL if no window found for PID, or error HRESULT.
HRESULT set_overlay_by_pid(DWORD pid, HICON icon,
                            const std::wstring& description = L"");

// Convenience: find window by PID, then clear overlay.
HRESULT clear_overlay_by_pid(DWORD pid);

// Convenience: find window by title (+optional process_name), then set overlay.
// Returns S_OK on success, E_FAIL if no matching window, or error HRESULT.
HRESULT set_overlay_by_title(const std::wstring& title,
                              const std::wstring& process_name,
                              HICON icon,
                              const std::wstring& description = L"");

// Convenience: find window by title (+optional process_name), then clear overlay.
HRESULT clear_overlay_by_title(const std::wstring& title,
                                 const std::wstring& process_name = L"");

} // namespace taskbar
