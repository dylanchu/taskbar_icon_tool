// taskbar/daemon.h - System-tray resident daemon for tb_icon.
#pragma once

#include <string>

namespace taskbar {

// Run the daemon. Reads config from iniPath, applies to existing windows,
// then stays resident in the system tray, monitoring for new windows and
// applying matching rules.
// Returns 0 on clean shutdown, non-zero on error.
int runDaemon(const std::wstring& iniPath);

} // namespace taskbar
