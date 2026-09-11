#pragma once

#include <QWidget>
#include <QLabel>
#include <QDialog>

// App version shown in About + used by the in-app updater to compare
// against the latest GitHub release tag (e.g. "v1.1.0").
static const char *kMagnivoVersion = "1.0.0";

// Virtual-key codes for the configurable zoom modifier (Windows).
// Kept as plain ints so the header stays portable (no windows.h here).
// VK_CONTROL=0x11, VK_MENU(Alt)=0x12, VK_SHIFT=0x10, VK_LWIN=0x5B
namespace ZoomMod {
const int Ctrl  = 0x11;
const int Alt   = 0x12;
const int Shift = 0x10;
const int Win   = 0x5B;
int load();                 // read from QSettings, default Ctrl
void save(int vk);          // persist to QSettings
QString name(int vk);       // "Ctrl" / "Alt" / "Shift" / "Win"
QString githubRepo();       // "owner/repo" from QSettings (editable in Settings)
void setGithubRepo(const QString &repo);
}

// Small control bar. This is the ONLY Qt window now.
// Fullscreen zoom itself is done by Windows Magnification API (GPU, no flicker).
// Gestures over this panel + <Modifier>+wheel anywhere + F8 hotkey control zoom.
class ControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlPanel(QWidget *parent = nullptr);

public slots:
    void setZoom(float zoom);
    void setArmed(bool armed);
    void setModifierName(const QString &modName);

signals:
    void pinchDelta(double delta);  // touchpad swipe in/out over panel
    void pinchScale(double factor); // touchscreen pinch over panel
    void zoomInClicked();
    void zoomOutClicked();
    void closeClicked();
    void armToggled(bool armed);
    void settingsClicked(); // single gear icon - opens tabbed dialog
    // NOTE: updateClicked/aboutClicked removed - Update+About are now
    // tabs inside SettingsDialog.

protected:
    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    QLabel *m_label = nullptr;
    class QPushButton *m_armBtn = nullptr;
    QPoint m_dragPos;
    bool m_touchActive = false;
    double m_touchLastDist = 0.0;
    QString m_modName = "Ctrl";
};

// Unified settings: General (modifier + repo) | Update (in-app updater) | About.
// Opened via the single gear icon on the control panel. This replaces the old
// separate Settings / Update / About buttons.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum Tab { GeneralTab = 0, UpdateTab = 1, AboutTab = 2 };
    explicit SettingsDialog(QWidget *parent = nullptr, int initialTab = GeneralTab);
    int selectedModifier() const;
    QString selectedRepo() const;

private slots:
    // In-app updater (Update tab): checks GitHub releases, downloads with
    // progress + speed, then runs installer. Says "You're up to date" if current.
    void startCheck();
    void onCheckFinished();
    void startDownload();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onInstallClicked();
    void onTabChanged(int index);

private:
    void setStatus(const QString &t);
    static QString fmtSize(qint64 bytes);
    static QString fmtSpeed(double bytesPerSec);

    class QTabWidget *m_tabs = nullptr;
    // General tab
    class QComboBox *m_modBox = nullptr;
    class QLineEdit *m_repoEdit = nullptr;
    // Update tab
    QLabel *m_status = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_stats = nullptr;
    class QProgressBar *m_bar = nullptr;
    class QPushButton *m_checkBtn = nullptr;
    class QPushButton *m_downloadBtn = nullptr;
    class QPushButton *m_installBtn = nullptr;
    class QNetworkAccessManager *m_nam = nullptr;
    class QNetworkReply *m_checkReply = nullptr;
    class QNetworkReply *m_dlReply = nullptr;
    class QFile *m_file = nullptr;
    class QElapsedTimer *m_timer = nullptr;
    QString m_downloadUrl;
    QString m_fileName;
    QString m_savePath;
    QString m_latestTag;
    qint64 m_assetSize = 0;
    qint64 m_lastReceived = 0;
    qint64 m_lastMs = 0;
    double m_speed = 0.0;
    bool m_updateChecked = false;
};

// Owns fullscreen magnification + zoom level + armed state + follow-cursor timer.
// Close / disarm resets MagSetFullscreenTransform(1.0) so screen goes back to normal.
class Magnivo : public QObject {
    Q_OBJECT
public:
    explicit Magnivo(QObject *parent = nullptr);
    ~Magnivo();
    void show();
    float zoom() const { return m_zoom; }
    bool armed() const { return m_armed; }

public slots:
    void setZoom(float z);
    void zoomIn();
    void zoomOut();
    void setArmed(bool armed);
    void toggleArmed();
    void openSettings();
    void openSettingsTab(int tab); // 0=General, 1=Update, 2=About
    void checkForUpdates(); // compat: opens Settings on Update tab
    void showAbout();       // compat: opens Settings on About tab

private slots:
    void tick();

private:
    // Starts at 100% so ACTIVATE arms gestures WITHOUT zooming.
    // Old default was 2.0f which made every activation jump to 200%.
    float m_zoom = 1.0f;
    bool m_armed = false; // starts OFF so stray gestures never zoom
    ControlPanel *m_panel = nullptr;
    class QTimer *m_timer = nullptr;
    bool m_magOk = false;
    bool m_transformActive = false; // true once we actually zoomed (for safe reset)
    void applyTransform();
    void installGlobalHooks();
    void uninstallGlobalHooks();
    bool gestureAllowed() const; // armed OR zoom-modifier held
};
