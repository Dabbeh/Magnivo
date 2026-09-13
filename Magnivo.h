#pragma once

#include <QWidget>
#include <QLabel>
#include <QDialog>
#include <QColor>
#include <QPushButton>
#include <QList>

// App version shown in About + used by the in-app updater to compare
// against the latest GitHub release tag (e.g. "v1.1.0").
static const char *kMagnivoVersion = "1.1.1";

// GitHub repo used by the updater. Hardcoded so users never have to type it.
// Releases in this repo are checked on startup + via Settings > Update.
static const char *kMagnivoUpdateRepo = "Dabbeh/Magnivo";

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
// NOTE: update repo is now hardcoded (kMagnivoUpdateRepo). These helpers stay
// only so old settings files don't break; the Settings UI no longer shows them.
QString githubRepo();       // returns kMagnivoUpdateRepo (legacy override ignored)
void setGithubRepo(const QString &repo);
}

// Settings appearance (Settings dialog + its popups). Dark by default to
// match the control panel; Light available in General tab. Persisted.
namespace Theme {
const int Dark  = 0;
const int Light = 1;
int load();          // read from QSettings, default Dark
void save(int theme);
}

// Integer rect in physical screen pixels. Caches the last-applied
// magnification source/destination rects (kept portable: no windows.h here;
// converted to RECT at the call site in Magnivo.cpp).
struct MagRect {
    int l = 0, t = 0, r = 0, b = 0;
    bool operator==(const MagRect &o) const { return l == o.l && t == o.t && r == o.r && b == o.b; }
    bool operator!=(const MagRect &o) const { return !(*this == o); }
};

// Button-style dropdown used in Settings instead of QComboBox: the value plus
// a text arrow that is always visible in both themes, opening a popup menu.
class OptionDropDown : public QPushButton {
    Q_OBJECT
public:
    explicit OptionDropDown(QWidget *parent = nullptr);
    void addItem(const QString &text, int data);
    int findData(int data) const;
    int currentData() const;
    int currentIndex() const;
    void setCurrentIndex(int i);
    void setMenuStyleSheet(const QString &st);

signals:
    void currentIndexChanged(int index);

private:
    struct Item { QString text; int data; };
    void refreshText();
    void showMenu();
    QList<Item> m_items;
    int m_cur = -1;
    QString m_menuStyle;
};

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
    void applyTheme(int theme); // dark/light panel, follows Settings > Appearance

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
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    void refreshArmButton(); // restyle ACTIVATE for current theme + armed state
    QLabel *m_label = nullptr;
    QLabel *m_titleName = nullptr;
    class QPushButton *m_armBtn = nullptr;
    class QPushButton *m_closeBtn = nullptr;
    class QPushButton *m_settingsBtn = nullptr;
    int m_theme = 0; // Theme::Dark; applied in constructor via applyTheme()
    QColor m_bgColor = QColor("#1e1e1e"); // panel fill, painted in paintEvent
    QColor m_borderColor = QColor("#555555");
    QPoint m_dragPos;
    bool m_touchActive = false;
    double m_touchLastDist = 0.0;
    QString m_modName = "Ctrl";
};

// Unified settings: General (modifier) | Update (in-app updater) | About.
// Opened via the single gear icon on the control panel. Kept intentionally
// simple: one control per tab, big status text, OK/Cancel with hover feedback.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum Tab { GeneralTab = 0, UpdateTab = 1, AboutTab = 2 };
    explicit SettingsDialog(QWidget *parent = nullptr, int initialTab = GeneralTab);
    int selectedModifier() const;
    int selectedTheme() const;
    static QString messageBoxStyle(int theme); // matching popup stylesheet (also used by startup monitor)

private slots:
    void onThemeChanged(int index);

private slots:
    // In-app updater (Update tab): checks GitHub releases for
    // kMagnivoUpdateRepo, shows "You're up to date" or pops up
    // "New update available" with Update/Cancel. Update auto-downloads
    // with progress, then enables Install & Restart.
    void startCheck();
    void onCheckFinished();
    void startDownload();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onInstallClicked();
    void onTabChanged(int index);

private:
    void setStatus(const QString &t);
    void showUpdateAvailablePopup(const QString &tag, const QString &notes);
    void applyTheme(int theme); // live preview: swaps the whole dialog stylesheet
    static QString settingsStyle(int theme); // full dialog stylesheet, dark or light
    static QString menuStyle(int theme); // popup-menu stylesheet for the dropdowns
    static QString fmtSize(qint64 bytes);
    static QString fmtSpeed(double bytesPerSec);

    class QTabWidget *m_tabs = nullptr;
    // General tab
    OptionDropDown *m_modBox = nullptr;
    OptionDropDown *m_themeBox = nullptr;
    // Update tab
    QLabel *m_status = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_stats = nullptr;
    class QProgressBar *m_bar = nullptr;
    class QPushButton *m_checkBtn = nullptr;
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
    void zoomByWheelDelta(int wheelDelta); // smooth proportional wheel/pinch step
    void setArmed(bool armed);
    void toggleArmed();
    void openSettings();
    void openSettingsTab(int tab); // 0=General, 1=Update, 2=About
    void checkForUpdates(); // compat: opens Settings on Update tab
    void showAbout();       // compat: opens Settings on About tab

private slots:
    void tick();
    void scheduleAutoUpdateCheck();
    void onAutoUpdateCheckFinished();

private:
    // Starts at 100% so ACTIVATE arms gestures WITHOUT zooming.
    // Old default was 2.0f which made every activation jump to 200%.
    float m_zoom = 1.0f;
    bool m_armed = false; // starts OFF so stray gestures never zoom
    ControlPanel *m_panel = nullptr;
    class QTimer *m_timer = nullptr;
    bool m_magOk = false;
    bool m_transformActive = false; // true once we actually zoomed (for safe reset)
    // Last applied transform: skip redundant driver calls from the 60fps
    // tick when the cursor didn't move, and only refresh the touch/pen
    // input mapping when the view rect actually changes.
    float m_lastMag = 0.0f;
    int m_lastXOff = 0, m_lastYOff = 0;
    int m_sameCount = 0; // ticks since the view last changed (self-heal ~1x/s)
    bool m_inputOn = false; // MagSetInputTransform currently active
    bool m_inputWarned = false; // logged the UIAccess limitation once
    MagRect m_lastSrc, m_lastDst;
    class QDialog *m_settingsDlg = nullptr; // open settings window (if any)
    void applyWindowFilter(); // keep our own windows unmagnified + clickable
    void updateInputTransform(bool wantOn, const MagRect &src, const MagRect &dst);
    qint64 m_tickCount = 0; // watchdog counter for hook reinstall
    void applyTransform();
    void installGlobalHooks();
    void uninstallGlobalHooks();
    void ensureHooksInstalled(); // watchdog: reinstall if Windows dropped them
    bool gestureAllowed() const; // armed OR zoom-modifier held
    // Background update monitor (startup check with popup on new release).
    class QNetworkAccessManager *m_updateNam = nullptr;
    class QNetworkReply *m_updateReply = nullptr;
};
