// Daemon: system-tray resident window monitor.
//
// Event-driven architecture:
//   SetWinEventHook(EVENT_OBJECT_SHOW) → WinEventProc → PostMessage →
//   daemon.enqueueNewWindow → pending queue → timer → tryApplyOverlay.
//
// INI changes detected via FindFirstChangeNotification, not polling.
#include "taskbar/daemon.h"
#include "taskbar/config.h"
#include "taskbar/tb_icon.h"

#include <windows.h>
#include <shellapi.h>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace taskbar {

// ===================================================================
//  Constants (anonymous ns)
// ===================================================================

namespace {
    constexpr UINT WM_TRAYICON   = WM_APP + 1;
    constexpr UINT WM_NEW_WINDOW = WM_APP + 2;
    constexpr UINT TRAY_ID       = 1;
    constexpr UINT TIMER_QUEUE   = 1;
    constexpr UINT QUEUE_MS      = 1000;
    constexpr UINT DELAY_MS      = 1000;
    constexpr int  MAX_RETRIES   = 3;
    constexpr UINT MENU_REAPPLY  = 1;
    constexpr UINT MENU_RESET    = 2;
    constexpr UINT MENU_EXIT     = 3;

    constexpr const wchar_t* MSG_WND_CLASS = L"TbIconDaemonMsg";
    constexpr const wchar_t* MUTEX_NAME    = L"tb_icon_daemon_mutex_v1";
}

// ===================================================================
//  Daemon — owns all state and logic.  Exposed via runDaemon().
// ===================================================================

struct PendingEntry {
    HWND hwnd;
    ULONGLONG detectedAt;
    int retries;
};

// Forward decls for free-function callbacks (defined after Daemon).
class Daemon;
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
LRESULT CALLBACK msgWndProc(HWND, UINT, WPARAM, LPARAM);

Daemon* g_daemon = nullptr;   // set while running
HWND    g_msgWnd = nullptr;   // for WinEventProc → PostMessage

class Daemon {
public:
    int run(const std::wstring& iniPath);

    // Called by free-function callbacks.
    friend LRESULT CALLBACK ::taskbar::msgWndProc(HWND, UINT, WPARAM, LPARAM);

private:
    // ---- state ----
    std::vector<ConfigRule> rules_;
    std::wstring baseDir_;
    std::wstring iniPath_;
    FILETIME iniLastWrite_ = {};
    HANDLE editorProcess_  = nullptr;
    std::unordered_set<HWND> ungrouped_;
    std::unordered_map<HWND, PendingEntry> pending_;
    HWND msgWindow_ = nullptr;
    HANDLE mutex_   = nullptr;
    bool   alreadyRunning_ = false;  // set by acquireSingleton
    NOTIFYICONDATAW nid_ = {};
    HICON trayIcon_     = nullptr;
    bool  trayIconOwned_ = false;

    // ---- lifecycle ----
    bool acquireSingleton();
    bool loadConfig();
    HWND createMessageWindow();
    bool createTrayIcon();
    void runMessageLoop(HANDLE hIniChange);
    void cleanup(HWINEVENTHOOK, HANDLE);

    // ---- rule matching ----
    bool procInRules(HWND) const;
    const ConfigRule* findRule(HWND) const;

    // ---- queue & overlay ----
    void enqueueNewWindow(HWND);
    void processPendingQueue();
    bool tryApplyOverlay(HWND, PendingEntry&, ULONGLONG now);

    // ---- tray actions (called from msgWndProc) ----
    void onTrayRClick();
    void onTrayDblClick();
    void onReApply();
    void onResetAll();

    // ---- INI reload ----
    bool tryReloadIni();
    static bool getFileWriteTime(const std::wstring&, FILETIME&);
};

// ===================================================================
//  Lifecycle
// ===================================================================

bool Daemon::acquireSingleton() {
    mutex_ = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    if (!mutex_) {
        MessageBoxW(nullptr, L"Failed to create mutex.", L"tb_icon",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"tb_icon daemon is already running.",
                    L"tb_icon", MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        alreadyRunning_ = true;
        return false;
    }
    return true;
}

bool Daemon::loadConfig() {
    auto rules = parseConfigFile(iniPath_);
    if (rules.empty()) {
        MessageBoxW(nullptr, (L"No rules found in config:\n" + iniPath_).c_str(),
                    L"tb_icon", MB_OK | MB_ICONWARNING);
        return false;
    }
    rules_   = std::move(rules);
    baseDir_ = cfgDirOf(iniPath_);
    getFileWriteTime(iniPath_, iniLastWrite_);
    return true;
}

HWND Daemon::createMessageWindow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        // Forward to msgWndProc defined below.
        return ::taskbar::msgWndProc(h, m, w, l);
    };
    wc.hInstance     = hInst;
    wc.lpszClassName = MSG_WND_CLASS;
    if (!RegisterClassExW(&wc)) return nullptr;

    HWND hwnd = CreateWindowExW(0, MSG_WND_CLASS, L"", 0,
                                 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, hInst, this);
    if (!hwnd) UnregisterClassW(MSG_WND_CLASS, hInst);
    return hwnd;
}

bool Daemon::createTrayIcon() {
    HICON icon = create_badge_icon(L"T", RGB(59, 130, 246));
    trayIconOwned_ = (icon != nullptr);
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    trayIcon_ = icon;

    nid_.cbSize           = sizeof(NOTIFYICONDATAW);
    nid_.hWnd             = msgWindow_;
    nid_.uID              = TRAY_ID;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon            = icon;
    wcscpy_s(nid_.szTip, L"tb_icon daemon");

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) return false;
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    return true;
}

void Daemon::runMessageLoop(HANDLE hIniChange) {
    HANDLE handles[2];
    for (;;) {
        DWORD n = 0;
        handles[n++] = hIniChange;
        if (editorProcess_) handles[n++] = editorProcess_;

        DWORD wr = MsgWaitForMultipleObjects(
            n, handles, FALSE, INFINITE, QS_ALLINPUT);
        DWORD idx = wr - WAIT_OBJECT_0;

        if (idx == 0) {                          // INI changed
            if (!editorProcess_) tryReloadIni();
            FindNextChangeNotification(hIniChange);
        } else if (idx == 1 && editorProcess_) { // editor closed
            CloseHandle(editorProcess_);
            editorProcess_ = nullptr;
            tryReloadIni();
        } else if (wr == WAIT_OBJECT_0 + n) {    // window messages
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

void Daemon::cleanup(HWINEVENTHOOK hWinEvent, HANDLE hIniChange) {
    KillTimer(msgWindow_, TIMER_QUEUE);
    if (hWinEvent)  UnhookWinEvent(hWinEvent);
    if (hIniChange) FindCloseChangeNotification(hIniChange);
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    if (trayIcon_ && trayIconOwned_)
        DestroyIcon(trayIcon_);
    if (editorProcess_) CloseHandle(editorProcess_);
    DestroyWindow(msgWindow_);
    UnregisterClassW(MSG_WND_CLASS, GetModuleHandleW(nullptr));
    if (mutex_) CloseHandle(mutex_);
}

// ===================================================================
//  Rule matching
// ===================================================================

bool Daemon::procInRules(HWND hwnd) const {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name = cfgToLower(get_process_name(pid));
    for (const auto& r : rules_) {
        if (cfgToLower(r.process_name) == name) return true;
    }
    return false;
}

const ConfigRule* Daemon::findRule(HWND hwnd) const {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name = cfgToLower(get_process_name(pid));
    for (const auto& r : rules_) {
        if (cfgToLower(r.process_name) != name) continue;
        std::wstring filter = (r.title == L"*") ? L"" : r.title;
        if (filter.empty()) return &r;
        std::wstring title = get_window_title(hwnd);
        if (title.empty()) continue;
        if (cfgToLower(title).find(cfgToLower(filter)) != std::wstring::npos)
            return &r;
    }
    return nullptr;
}

// ===================================================================
//  Queue & overlay
// ===================================================================

void Daemon::enqueueNewWindow(HWND h) {
    if (!IsWindow(h) || !IsWindowVisible(h)) return;
    if (GetWindow(h, GW_OWNER) != nullptr) return;
    if (procInRules(h))
        pending_[h] = PendingEntry{ h, GetTickCount64(), 0 };
}

bool Daemon::tryApplyOverlay(HWND h, PendingEntry& e, ULONGLONG now) {
    if (!IsWindow(h)) return true;

    const ConfigRule* rule = findRule(h);
    if (!rule) {
        if (e.retries < MAX_RETRIES) { e.retries++; e.detectedAt = now; }
        else return true;
        return false;
    }

    // Ungroup: each window gets its own taskbar button.
    if (ungrouped_.insert(h).second) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        set_app_user_model_id(h, makeAumid(h, pid));
    }

    HICON icon = resolveIconForHwnd(*rule, baseDir_, h);
    if (!icon) {
        if (e.retries < MAX_RETRIES) { e.retries++; e.detectedAt = now; }
        else return true;
        return false;
    }

    HRESULT hr = set_overlay_by_hwnd(h, icon, rule->desc);
    destroy_icon(icon);

    if (SUCCEEDED(hr)) return true;
    if (e.retries < MAX_RETRIES) { e.retries++; e.detectedAt = now; }
    else return true;
    return false;
}

void Daemon::processPendingQueue() {
    ULONGLONG now = GetTickCount64();
    std::vector<HWND> done;
    for (auto& [h, e] : pending_) {
        if (now - e.detectedAt < DELAY_MS) continue;
        if (tryApplyOverlay(h, e, now)) done.push_back(h);
    }
    for (HWND h : done) pending_.erase(h);
}

// ===================================================================
//  Tray actions
// ===================================================================

void Daemon::onTrayRClick() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_REAPPLY, L"Re-apply");
    AppendMenuW(menu, MF_STRING, MENU_RESET,   L"Reset all");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_EXIT,    L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(msgWindow_);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                  pt.x, pt.y, 0, msgWindow_, nullptr);
    DestroyMenu(menu);
}

void Daemon::onTrayDblClick() {
    if (editorProcess_) {
        AllowSetForegroundWindow(GetProcessId(editorProcess_));
        return;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd   = msgWindow_;
    sei.lpVerb = L"open";
    sei.lpFile = iniPath_.c_str();
    sei.nShow  = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess)
        editorProcess_ = sei.hProcess;
}

void Daemon::onReApply() {
    ungrouped_.clear();
    pending_.clear();
    applyRulesToWindows(rules_, baseDir_);
}

void Daemon::onResetAll() {
    // Collect every process name mentioned in the rules.
    for (const auto& r : rules_) {
        auto hwnds = find_window(L"", r.process_name);
        for (HWND h : hwnds) {
            clear_overlay_by_hwnd(h);       // remove overlay
            set_app_user_model_id(h, L"");  // regroup
        }
    }
    ungrouped_.clear();
    pending_.clear();
}

// ===================================================================
//  INI reload
// ===================================================================

bool Daemon::getFileWriteTime(const std::wstring& path, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        return false;
    ft = attr.ftLastWriteTime;
    return true;
}

bool Daemon::tryReloadIni() {
    FILETIME ft = {};
    if (!getFileWriteTime(iniPath_, ft)) return false;
    if (CompareFileTime(&ft, &iniLastWrite_) == 0) return false;

    auto rules = parseConfigFile(iniPath_);
    if (rules.empty()) return false;

    iniLastWrite_ = ft;
    rules_ = std::move(rules);
    applyRulesToWindows(rules_, baseDir_);
    ungrouped_.clear();
    pending_.clear();
    return true;
}

// ===================================================================
//  Daemon::run  (the real entry point)
// ===================================================================

int Daemon::run(const std::wstring& iniPath) {
    iniPath_ = iniPath;

    if (!acquireSingleton()) return alreadyRunning_ ? 0 : 1;
    scoped_com com;

    if (!loadConfig())            { CloseHandle(mutex_); return 1; }
    applyRulesToWindows(rules_, baseDir_);

    msgWindow_ = createMessageWindow();
    if (!msgWindow_)              { CloseHandle(mutex_); return 1; }
    g_daemon   = this;
    g_msgWnd   = msgWindow_;

    if (!createTrayIcon())        { cleanup(nullptr, nullptr); return 1; }

    HWINEVENTHOOK hWinEvent = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    HANDLE hIniChange = FindFirstChangeNotificationW(
        cfgDirOf(iniPath_).c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    SetTimer(msgWindow_, TIMER_QUEUE, QUEUE_MS, nullptr);

    runMessageLoop(hIniChange);
    cleanup(hWinEvent, hIniChange);
    return 0;
}

// ===================================================================
//  Free-function callbacks (Win32 requires plain functions)
// ===================================================================

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!IsWindowVisible(hwnd)) return;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return;
    PostMessageW(g_msgWnd, WM_NEW_WINDOW, reinterpret_cast<WPARAM>(hwnd), 0);
}

LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Daemon* e = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        e = static_cast<Daemon*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
        return 0;
    }
    e = reinterpret_cast<Daemon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!e) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_NEW_WINDOW:
        e->enqueueNewWindow(reinterpret_cast<HWND>(wParam));
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_QUEUE) e->processPendingQueue();
        return 0;
    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:      e->onTrayRClick();    return 0;
        case WM_LBUTTONDBLCLK:  e->onTrayDblClick();  return 0;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case MENU_REAPPLY: e->onReApply();         return 0;
        case MENU_RESET:   e->onResetAll();        return 0;
        case MENU_EXIT:    DestroyWindow(hwnd);    return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ===================================================================
//  runDaemon  (public entry point)
// ===================================================================

int runDaemon(const std::wstring& iniPath) {
    Daemon d;
    return d.run(iniPath);
}

} // namespace taskbar
