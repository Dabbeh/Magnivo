#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
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

    // Single instance: a second launch (e.g. via the Ctrl+Alt+G Start Menu
    // shortcut hotkey while already running) summons the existing panel
    // instead of starting a rival magnifier that would fight over the
    // fullscreen transform + global hooks.
    const QString ipcName = QString::fromLatin1("MagnivoSingleInstance");
    QLocalSocket probe;
    probe.connectToServer(ipcName);
    if (probe.waitForConnected(300)) {
        probe.write("summon");
        probe.waitForBytesWritten(1000);
        probe.disconnectFromServer();
        return 0;
    }

    Magnivo mag;
    QLocalServer ipc;
    // Stale pipe after a crash: take the name back so relaunch always works.
    QLocalServer::removeServer(ipcName);
    if (ipc.listen(ipcName)) {
        QObject::connect(&ipc, &QLocalServer::newConnection, [&mag, &ipc]() {
            while (QLocalSocket *c = ipc.nextPendingConnection()) {
                QObject::connect(c, &QLocalSocket::readyRead, [&mag, c]() {
                    c->readAll(); // "summon" - the only command
                    mag.summon();
                    c->disconnectFromServer();
                });
                QObject::connect(c, &QLocalSocket::disconnected,
                                c, &QLocalSocket::deleteLater);
            }
        });
    }
    // If listen() failed (a frozen older instance holds the name) we still
    // run: better a working magnifier than a silent refusal to start.
    mag.show();

    int rc = app.exec();
    // Magnivo destructor resets MagSetFullscreenTransform(1.0)
    // so the screen is back to normal on X / Esc close.
    return rc;
}
