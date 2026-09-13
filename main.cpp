#include <QApplication>
#include "Magnivo.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The embedded manifest already declares PerMonitorV2, but enforce it here
// too (before any HWND exists) so the Mag API always sees physical pixels
// even if the manifest is ever stripped. Loaded dynamically to avoid
// depending on a new Windows SDK at compile time.
static void enablePerMonitorV2DpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        PFN_SetCtx setCtx = (PFN_SetCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (setCtx((HANDLE)-4)) return;
            // Already set via manifest: also fine, fall through silently.
            if (GetLastError() == ERROR_ACCESS_DENIED) return;
        }
    }
    // Legacy fallback (Vista..8.1): at least system-DPI aware.
    SetProcessDPIAware();
}
#endif

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    enablePerMonitorV2DpiAwareness();
#endif
    QApplication app(argc, argv);
    app.setApplicationName("Magnivo");
    app.setOrganizationName("Magnivo");

    Magnivo mag;
    mag.show();

    int rc = app.exec();
    // Magnivo destructor resets MagSetFullscreenTransform(1.0)
    // so the screen is back to normal on X / Esc close.
    return rc;
}
