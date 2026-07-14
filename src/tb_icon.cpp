// Implementation of taskbar overlay and grouping API.
#include "taskbar/tb_icon.h"

#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "propsys.lib")

namespace taskbar {

// ===== Lifecycle =====

scoped_com::scoped_com() {
    m_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}
scoped_com::~scoped_com() {
    if (m_ok) CoUninitialize();
}

// ===== Internal helpers =====

namespace {

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// RAII wrapper for GDI+ lifecycle.
class gdiplus_scope {
public:
    gdiplus_scope() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&m_token, &input, nullptr);
    }
    ~gdiplus_scope() {
        Gdiplus::GdiplusShutdown(m_token);
    }
private:
    ULONG_PTR m_token;
};

// EnumWindows callback data: collect top-level windows for a PID.
struct PidEnumData {
    DWORD pid;
    std::vector<HWND> hwnds;
};

BOOL CALLBACK enum_by_pid_proc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<PidEnumData*>(lParam);
    DWORD window_pid = 0;
    GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid != data->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    data->hwnds.push_back(hwnd);
    return TRUE;
}

// EnumWindows callback data: collect all windows matching title + process name.
struct FindData {
    std::wstring title_needle;   // case-insensitive substring, empty = any
    std::wstring proc_needle;    // case-insensitive substring, empty = any
    std::vector<HWND> results;
};

BOOL CALLBACK find_proc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindData*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    // Title filter
    if (!data->title_needle.empty()) {
        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 512);
        if (to_lower(title).find(to_lower(data->title_needle)) == std::wstring::npos) {
            return TRUE;
        }
    }

    // Process name filter
    if (!data->proc_needle.empty()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (to_lower(get_process_name(pid)) != to_lower(data->proc_needle)) {
            return TRUE;
        }
    }

    data->results.push_back(hwnd);
    return TRUE;
}

} // anonymous namespace

// ===== Window discovery =====

std::vector<HWND> list_windows_by_pid(DWORD pid) {
    PidEnumData data{ pid, {} };
    EnumWindows(enum_by_pid_proc, reinterpret_cast<LPARAM>(&data));
    return data.hwnds;
}

HWND find_window_by_pid(DWORD pid) {
    auto hwnds = list_windows_by_pid(pid);
    if (hwnds.empty()) return nullptr;

    // Priority 1: WS_EX_APPWINDOW (explicitly shown in taskbar)
    for (auto hwnd : hwnds) {
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW)
            return hwnd;
    }
    // Priority 2: has title, not a tool window
    for (auto hwnd : hwnds) {
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        if (GetWindowTextLengthW(hwnd) > 0) return hwnd;
    }
    // Priority 3: any window with a title
    for (auto hwnd : hwnds) {
        if (GetWindowTextLengthW(hwnd) > 0) return hwnd;
    }
    return hwnds[0];
}

std::vector<HWND> find_window(const std::wstring& title,
                               const std::wstring& process_name) {
    FindData data{ title, process_name, {} };
    EnumWindows(find_proc, reinterpret_cast<LPARAM>(&data));
    return data.results;
}

std::wstring get_window_title(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"(untitled)";
    std::wstring result(len + 1, L'\0');
    GetWindowTextW(hwnd, result.data(), static_cast<int>(result.size()));
    result.resize(len);
    return result;
}

std::wstring get_process_name(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"<unknown>";
    wchar_t name[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, name, &size) != 0;
    CloseHandle(h);
    if (!ok) return L"<unknown>";
    std::wstring path(name);
    size_t pos = path.find_last_of(L"\\/");
    return pos != std::wstring::npos ? path.substr(pos + 1) : path;
}

// ===== Icon creation =====

HICON load_icon_from_file(const std::wstring& path, int index, int size) {
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = to_lower(path.substr(dot + 1));
    }

    if (ext == L"exe" || ext == L"dll" || ext == L"icl") {
        HICON h_large = nullptr, h_small = nullptr;
        UINT count = ExtractIconExW(path.c_str(), index, &h_large, &h_small, 1);
        if (count == 0 || count == static_cast<UINT>(static_cast<ULONG_PTR>(-1))) {
            return nullptr;
        }
        HICON result = (size <= 16 && h_small) ? h_small : (h_large ? h_large : h_small);
        if (result == h_large && h_small) DestroyIcon(h_small);
        if (result == h_small && h_large) DestroyIcon(h_large);
        return result;
    }

    return static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                          size, size, LR_LOADFROMFILE));
}

HICON create_badge_icon(const std::wstring& text, COLORREF bg_color,
                         COLORREF text_color, int size) {
    gdiplus_scope gdi;

    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0)); // transparent

    // Filled circle
    Gdiplus::SolidBrush bg(Gdiplus::Color(
        255, GetRValue(bg_color), GetGValue(bg_color), GetBValue(bg_color)));
    Gdiplus::REAL pad = 0.5f;
    graphics.FillEllipse(&bg, pad, pad,
                         static_cast<Gdiplus::REAL>(size) - 1 - pad * 2,
                         static_cast<Gdiplus::REAL>(size) - 1 - pad * 2);

    // Text
    if (!text.empty()) {
        Gdiplus::REAL font_size;
        if (text.length() == 1)      font_size = size * 0.62f;
        else if (text.length() == 2) font_size = size * 0.48f;
        else                         font_size = size * 0.36f;

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, font_size, Gdiplus::FontStyleBold,
                           Gdiplus::UnitPixel);
        Gdiplus::SolidBrush fg(Gdiplus::Color(
            255, GetRValue(text_color), GetGValue(text_color), GetBValue(text_color)));

        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::RectF rect(0, 0, static_cast<Gdiplus::REAL>(size),
                             static_cast<Gdiplus::REAL>(size));
        graphics.DrawString(text.c_str(), -1, &font, rect, &format, &fg);
    }

    HICON icon = nullptr;
    return (bitmap.GetHICON(&icon) == Gdiplus::Ok) ? icon : nullptr;
}

void destroy_icon(HICON& icon) {
    if (icon) {
        DestroyIcon(icon);
        icon = nullptr;
    }
}

// ===== Overlay (ITaskbarList3::SetOverlayIcon) =====

HRESULT set_overlay_by_hwnd(HWND hwnd, HICON icon, const std::wstring& description) {
    if (!hwnd || !IsWindow(hwnd)) return E_INVALIDARG;

    ITaskbarList3* p_taskbar = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&p_taskbar));
    if (FAILED(hr) || !p_taskbar) return hr;

    hr = p_taskbar->HrInit();
    if (FAILED(hr)) {
        p_taskbar->Release();
        return hr;
    }

    hr = p_taskbar->SetOverlayIcon(hwnd, icon,
        description.empty() ? nullptr : description.c_str());

    p_taskbar->Release();
    return hr;
}

HRESULT clear_overlay_by_hwnd(HWND hwnd) {
    return set_overlay_by_hwnd(hwnd, nullptr, L"");
}

HRESULT set_overlay_by_pid(DWORD pid, HICON icon, const std::wstring& description) {
    HWND hwnd = find_window_by_pid(pid);
    if (!hwnd) return E_FAIL;
    return set_overlay_by_hwnd(hwnd, icon, description);
}

HRESULT clear_overlay_by_pid(DWORD pid) {
    HWND hwnd = find_window_by_pid(pid);
    if (!hwnd) return E_FAIL;
    return clear_overlay_by_hwnd(hwnd);
}

HRESULT set_overlay_by_title(const std::wstring& title,
                              const std::wstring& process_name,
                              HICON icon, const std::wstring& description) {
    auto hwnds = find_window(title, process_name);
    if (hwnds.empty()) return E_FAIL;
    return set_overlay_by_hwnd(hwnds[0], icon, description);
}

HRESULT clear_overlay_by_title(const std::wstring& title,
                                 const std::wstring& process_name) {
    auto hwnds = find_window(title, process_name);
    if (hwnds.empty()) return E_FAIL;
    return clear_overlay_by_hwnd(hwnds[0]);
}

// ===== AppUserModelID (taskbar grouping control) =====

HRESULT set_app_user_model_id(HWND hwnd, const std::wstring& appId) {
    if (!hwnd || !IsWindow(hwnd)) return E_INVALIDARG;

    IPropertyStore* pProps = nullptr;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&pProps));
    if (FAILED(hr) || !pProps) return hr;

    PROPVARIANT pv;
    if (appId.empty()) {
        // Clear the property
        PropVariantInit(&pv);
        hr = pProps->SetValue(PKEY_AppUserModel_ID, pv);
        PropVariantClear(&pv);
    } else {
        hr = InitPropVariantFromString(appId.c_str(), &pv);
        if (SUCCEEDED(hr)) {
            hr = pProps->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }
    }

    pProps->Commit();
    pProps->Release();
    return hr;
}

std::wstring get_app_user_model_id(HWND hwnd) {
    std::wstring result;
    if (!hwnd || !IsWindow(hwnd)) return result;

    IPropertyStore* pProps = nullptr;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&pProps));
    if (FAILED(hr) || !pProps) return result;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = pProps->GetValue(PKEY_AppUserModel_ID, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        result = pv.pwszVal;
    }
    PropVariantClear(&pv);
    pProps->Release();
    return result;
}

} // namespace taskbar
