#include "Magnivo.h"

#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QCursor>
#include <QTimer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMouseEvent>
#include <QGestureEvent>
#include <QNativeGestureEvent>
#include <QTouchEvent>
#include <QEventPoint>
#include <QLineF>
#include <QAction>
#include <QDateTime>
#include <QSignalBlocker>
#include <QSettings>
#include <QComboBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QVersionNumber>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QUrl>
#include <QIcon>
#include <QPixmap>
#include <QDebug>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Magnification API without needing magnification.h / .lib (missing on MinGW).
// We load Magnification.dll at runtime via GetProcAddress.
#ifndef MW_FILTERMODE_EXCLUDE
#define MW_FILTERMODE_EXCLUDE 0
#endif
typedef BOOL (WINAPI *PFN_MagInitialize)(void);
typedef BOOL (WINAPI *PFN_MagUninitialize)(void);
typedef BOOL (WINAPI *PFN_MagSetFullscreenTransform)(float, int, int);
typedef BOOL (WINAPI *PFN_MagSetWindowFilterList)(HWND, DWORD, int, HWND*);
struct MagColorEffect { float transform[5][5]; };
typedef BOOL (WINAPI *PFN_MagSetFullscreenColorEffect)(MagColorEffect*);

static HMODULE g_magDll = nullptr;
static PFN_MagInitialize pMagInitialize = nullptr;
static PFN_MagUninitialize pMagUninitialize = nullptr;
static PFN_MagSetFullscreenTransform pMagSetFullscreenTransform = nullptr;
static PFN_MagSetWindowFilterList pMagSetWindowFilterList = nullptr;
static PFN_MagSetFullscreenColorEffect pMagSetFullscreenColorEffect = nullptr;

static bool magLoad() {
    if (g_magDll) return pMagInitialize != nullptr;
    g_magDll = LoadLibraryW(L"Magnification.dll");
    if (!g_magDll) { qWarning() << "Magnivo: LoadLibrary(Magnification.dll) failed:" << GetLastError(); return false; }
    pMagInitialize = (PFN_MagInitialize)GetProcAddress(g_magDll, "MagInitialize");
    pMagUninitialize = (PFN_MagUninitialize)GetProcAddress(g_magDll, "MagUninitialize");
    pMagSetFullscreenTransform = (PFN_MagSetFullscreenTransform)GetProcAddress(g_magDll, "MagSetFullscreenTransform");
    pMagSetWindowFilterList = (PFN_MagSetWindowFilterList)GetProcAddress(g_magDll, "MagSetWindowFilterList");
    pMagSetFullscreenColorEffect = (PFN_MagSetFullscreenColorEffect)GetProcAddress(g_magDll, "MagSetFullscreenColorEffect");
    if (!pMagInitialize || !pMagUninitialize || !pMagSetFullscreenTransform)
        qWarning() << "Magnivo: GetProcAddress failed for Mag API";
    return pMagInitialize && pMagUninitialize && pMagSetFullscreenTransform;
}
static BOOL magInitialize() { return (magLoad() && pMagInitialize) ? pMagInitialize() : FALSE; }
static BOOL magUninitialize() { return pMagUninitialize ? pMagUninitialize() : FALSE; }
static BOOL magSetFullscreenTransform(float m, int x, int y) {
    return pMagSetFullscreenTransform ? pMagSetFullscreenTransform(m, x, y) : FALSE;
}
static BOOL magSetWindowFilterList(HWND h, DWORD mode, int c, HWND *l) {
    return pMagSetWindowFilterList ? pMagSetWindowFilterList(h, mode, c, l) : FALSE;
}

// ---------------- Configurable zoom modifier ----------------
static int g_zoomModVk = 0x11; // VK_CONTROL default; loaded from QSettings

static bool isZoomModHeld() {
    int vk = g_zoomModVk;
    if (vk == 0x5B) { // Win: either side
        return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#endif

// ---------------- ZoomMod settings helpers (shared Win/non-Win) ----------------
// NOTE: app was renamed Magnify -> Magnivo. Settings used to live under
// org/app "Magnify"/"Magnify"; fall back to that location once so existing
// users keep their modifier key + repo after upgrading.
static QVariant oldMagnifyValue(const QString &key) {
    QSettings oldS("Magnify", "Magnify");
    return oldS.value(key);
}
int ZoomMod::load() {
    QSettings s;
    QVariant v = s.value("zoomModifierVk");
    if (v.isNull() || !v.isValid())
        v = oldMagnifyValue("zoomModifierVk");
    int vk = v.isNull() ? ZoomMod::Ctrl : v.toInt();
    if (vk != Ctrl && vk != Alt && vk != Shift && vk != Win)
        vk = Ctrl;
    return vk;
}
void ZoomMod::save(int vk) {
    QSettings s;
    s.setValue("zoomModifierVk", vk);
}
QString ZoomMod::name(int vk) {
    if (vk == Alt) return "Alt";
    if (vk == Shift) return "Shift";
    if (vk == Win) return "Win";
    return "Ctrl";
}
QString ZoomMod::githubRepo() {
    QSettings s;
    QString repo = s.value("githubRepo", "").toString().trimmed();
    if (repo.isEmpty())
        repo = oldMagnifyValue("githubRepo").toString().trimmed();
    return repo;
}
void ZoomMod::setGithubRepo(const QString &repo) {
    QSettings s;
    s.setValue("githubRepo", repo.trimmed());
}

#ifdef Q_OS_WIN
static Magnivo *g_inst = nullptr;
static HHOOK g_mouseHook = nullptr;
static HHOOK g_kbHook = nullptr;
static qint64 g_lastToggleMs = 0;

static void requestToggle() {
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - g_lastToggleMs < 400) return; // debounce auto-repeat
    g_lastToggleMs = now;
    if (g_inst) {
        Magnivo *inst = g_inst;
        QTimer::singleShot(0, inst, [inst]() { inst->toggleArmed(); });
    }
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && g_inst) {
        MSLLHOOKSTRUCT *ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        // <Modifier>+wheel anywhere = intentional zoom, always allowed.
        // This is the global gesture that works while other apps have focus.
        if (isZoomModHeld()) {
            short delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
            Magnivo *inst = g_inst;
            QTimer::singleShot(0, inst, [inst, delta]() {
                if (delta > 0) inst->zoomIn();
                else inst->zoomOut();
            });
            return 1; // swallow so the app behind doesn't also zoom
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK KbProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT *kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // F8 toggles activation anywhere
        if (kb->vkCode == VK_F8) {
            requestToggle();
            return 1;
        }
        // Ctrl+Alt+M toggles activation anywhere
        if (kb->vkCode == 'M') {
            bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
            bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
            if (ctrl && alt) {
                requestToggle();
                return 1;
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}
#endif

// ---------------- ControlPanel ----------------

ControlPanel::ControlPanel(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowIcon(QIcon(":/logo.png"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    grabGesture(Qt::PinchGesture);
    setStyleSheet("ControlPanel{background:#1e1e1e;border:1px solid #555;border-radius:10px;}"
                  "QLabel{color:white;font-size:16px;font-weight:bold;}"
                  "QPushButton{background:#333;color:white;border:1px solid #666;border-radius:8px;font-size:16px;}"
                  "QPushButton:hover{background:#444;}"
                  "QPushButton:disabled{background:#222;color:#777;border:1px solid #444;}");

    auto *main = new QVBoxLayout(this);
    main->setContentsMargins(8, 8, 8, 8);
    main->setSpacing(8);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);
    m_armBtn = new QPushButton("ACTIVATE (F8)", this);
    m_armBtn->setCheckable(true);
    m_armBtn->setChecked(false);
    m_armBtn->setMinimumHeight(48);
    m_armBtn->setCursor(Qt::PointingHandCursor);
    m_armBtn->setToolTip("Arm gestures. When OFF, swipes are ignored so you never zoom by accident.\nToggle with F8 or Ctrl+Alt+M anywhere, or hold Ctrl while pinching.");
    auto *closeBtn = new QPushButton("X", this);
    closeBtn->setMinimumSize(48, 48);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip("Close - screen goes back to normal");
    closeBtn->setStyleSheet("QPushButton{background:#a33;color:white;border-radius:8px;}"
                            "QPushButton:hover{background:#c44;}");
    topRow->addWidget(m_armBtn, 1);
    topRow->addWidget(closeBtn);

    auto *botRow = new QHBoxLayout();
    botRow->setSpacing(8);
    auto *minusBtn = new QPushButton("-", this);
    m_label = new QLabel("OFF", this);
    auto *plusBtn = new QPushButton("+", this);
    for (auto *b : {minusBtn, plusBtn}) {
        b->setMinimumSize(48, 48);
        b->setCursor(Qt::PointingHandCursor);
    }
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setMinimumWidth(72);
    m_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    minusBtn->setToolTip("Zoom out (auto-activates)");
    plusBtn->setToolTip("Zoom in (auto-activates)");
    botRow->addWidget(minusBtn);
    botRow->addWidget(m_label, 1);
    botRow->addWidget(plusBtn);

    auto *menuRow = new QHBoxLayout();
    menuRow->setSpacing(8);
    auto *versionLabel = new QLabel(QString("v%1").arg(QString::fromLatin1(kMagnivoVersion)), this);
    versionLabel->setStyleSheet("QLabel{color:#888;font-size:11px;font-weight:normal;}");
    versionLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto *settingsBtn = new QPushButton(QString::fromUtf8("\u2699"), this); // gear icon
    settingsBtn->setMinimumSize(48, 32);
    settingsBtn->setMaximumWidth(48);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setToolTip("Settings, updates and about");
    settingsBtn->setStyleSheet("QPushButton{font-size:20px;padding-bottom:2px;}");
    menuRow->addWidget(versionLabel);
    menuRow->addStretch(1);
    menuRow->addWidget(settingsBtn);

    main->addLayout(topRow);
    main->addLayout(botRow);
    main->addLayout(menuRow);

    connect(minusBtn, &QPushButton::clicked, this, &ControlPanel::zoomOutClicked);
    connect(plusBtn, &QPushButton::clicked, this, &ControlPanel::zoomInClicked);
    connect(closeBtn, &QPushButton::clicked, this, &ControlPanel::closeClicked);
    connect(m_armBtn, &QPushButton::toggled, this, &ControlPanel::armToggled);
    connect(settingsBtn, &QPushButton::clicked, this, &ControlPanel::settingsClicked);

    auto *esc = new QAction(this);
    esc->setShortcut(Qt::Key_Escape);
    connect(esc, &QAction::triggered, this, &ControlPanel::closeClicked);
    addAction(esc);

    setFixedSize(300, 160);
    setArmed(false);
}

void ControlPanel::setZoom(float zoom) {
    if (!m_armBtn->isChecked())
        return; // keep showing OFF while disarmed
    m_label->setText(QString::number(int(zoom * 100)) + "%");
}

void ControlPanel::setArmed(bool armed) {
    // block to avoid armToggled -> Magnivo::setArmed recursion loop
    const QSignalBlocker blocker(m_armBtn);
    m_armBtn->setChecked(armed);
    if (armed) {
        m_armBtn->setText("ACTIVE - pinch to zoom");
        m_armBtn->setStyleSheet("QPushButton{background:#1d5c2e;color:white;border:1px solid #4caf50;border-radius:8px;font-size:16px;font-weight:bold;}"
                               "QPushButton:hover{background:#257a3c;}");
    } else {
        m_armBtn->setText("ACTIVATE (F8)");
        // Same look as the +/- buttons (see ControlPanel stylesheet)
        m_armBtn->setStyleSheet("QPushButton{background:#333;color:white;border:1px solid #666;border-radius:8px;font-size:16px;}"
                               "QPushButton:hover{background:#444;}");
        m_label->setText("OFF");
    }
}

void ControlPanel::setModifierName(const QString &modName) {
    m_modName = modName;
    m_armBtn->setToolTip(QString("Arm gestures. When OFF, swipes are ignored so you never zoom by accident.\n"
                                "Toggle with F8 or Ctrl+Alt+M anywhere, or hold %1 while pinching/wheeling.").arg(m_modName));
}

bool ControlPanel::event(QEvent *e) {
    // Pinch over the panel. Pinch anywhere else is handled globally
    // via <Modifier>+wheel hook (see Magnivo hooks) to avoid blocking other apps.
    if (e->type() == QEvent::NativeGesture) {
        auto *nge = static_cast<QNativeGestureEvent*>(e);
        if (nge->gestureType() == Qt::ZoomNativeGesture) {
            emit pinchDelta(nge->value());
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::Gesture) {
        auto *ge = static_cast<QGestureEvent*>(e);
        if (QGesture *g = ge->gesture(Qt::PinchGesture)) {
            auto *pinch = static_cast<QPinchGesture*>(g);
            if (pinch->state() == Qt::GestureUpdated) {
                double last = pinch->lastScaleFactor(), cur = pinch->scaleFactor();
                if (last > 0 && cur > 0) {
                    double f = cur / last;
                    if (f > 0.5 && f < 2.0) emit pinchScale(f);
                }
            }
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate || e->type() == QEvent::TouchEnd) {
        auto *te = static_cast<QTouchEvent*>(e);
        const auto pts = te->points();
        if (pts.size() == 2) {
            double dist = QLineF(pts[0].position(), pts[1].position()).length();
            if (!m_touchActive) { m_touchActive = true; m_touchLastDist = dist; }
            else if (m_touchLastDist > 0 && dist > 0) {
                double f = dist / m_touchLastDist;
                if (f > 0.5 && f < 2.0 && qAbs(f - 1.0) > 0.002) emit pinchScale(f);
                m_touchLastDist = dist;
            }
        } else { m_touchActive = false; m_touchLastDist = 0; }
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void ControlPanel::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        m_dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
    QWidget::mousePressEvent(e);
}

void ControlPanel::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::LeftButton)
        move(e->globalPosition().toPoint() - m_dragPos);
    QWidget::mouseMoveEvent(e);
}

// ---------------- SettingsDialog (General | Update | About) ----------------

SettingsDialog::SettingsDialog(QWidget *parent, int initialTab) : QDialog(parent) {
    setWindowTitle("Magnivo Settings");
    setWindowIcon(QIcon(":/logo.png"));
    setModal(true);
    auto *mainLay = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    mainLay->addWidget(m_tabs);

    // --- General tab: modifier + repo ---
    auto *general = new QWidget(this);
    auto *form = new QFormLayout(general);

    m_modBox = new QComboBox(general);
    m_modBox->addItem("Ctrl", ZoomMod::Ctrl);
    m_modBox->addItem("Alt", ZoomMod::Alt);
    m_modBox->addItem("Shift", ZoomMod::Shift);
    m_modBox->addItem("Win", ZoomMod::Win);
    int cur = ZoomMod::load();
    int idx = m_modBox->findData(cur);
    if (idx >= 0) m_modBox->setCurrentIndex(idx);
    m_modBox->setToolTip("Hold this key + mouse wheel anywhere to zoom");

    m_repoEdit = new QLineEdit(ZoomMod::githubRepo(), general);
    m_repoEdit->setPlaceholderText("owner/repo  e.g. octocat/Hello-World");
    m_repoEdit->setToolTip("GitHub repo used by Update checker (releases)");

    form->addRow("Zoom modifier + wheel:", m_modBox);
    form->addRow("GitHub repo:", m_repoEdit);

    auto *hint = new QLabel("Hold the modifier + wheel anywhere to zoom.\nPinch gestures also require it unless ACTIVE.", general);
    hint->setWordWrap(true);
    form->addRow(hint);
    m_tabs->addTab(general, "General");

    // --- Update tab: in-app updater ---
    auto *update = new QWidget(this);
    auto *ulay = new QVBoxLayout(update);
    m_status = new QLabel("Press Check to look for updates.", update);
    m_status->setWordWrap(true);
    m_detail = new QLabel(QString("Current version: v%1").arg(QString::fromLatin1(kMagnivoVersion)), update);
    m_detail->setWordWrap(true);
    m_bar = new QProgressBar(update);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_stats = new QLabel("", update);

    auto *btnRow = new QHBoxLayout();
    m_checkBtn = new QPushButton("Check", update);
    m_downloadBtn = new QPushButton("Download", update);
    m_installBtn = new QPushButton("Install && Restart", update);
    m_downloadBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    btnRow->addWidget(m_checkBtn);
    btnRow->addWidget(m_downloadBtn);
    btnRow->addWidget(m_installBtn);
    btnRow->addStretch(1);

    ulay->addWidget(m_status);
    ulay->addWidget(m_detail);
    ulay->addWidget(m_bar);
    ulay->addWidget(m_stats);
    ulay->addLayout(btnRow);
    ulay->addStretch(1);
    m_tabs->addTab(update, "Update");

    // --- About tab ---
    auto *about = new QWidget(this);
    auto *alay = new QVBoxLayout(about);
    auto *logoLabel = new QLabel(about);
    QPixmap logoPx(":/logo.png");
    if (!logoPx.isNull())
        logoLabel->setPixmap(logoPx.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignHCenter);
    alay->addWidget(logoLabel);
    auto *label = new QLabel(about);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setText(
        QString("<h2>Magnivo v%1</h2>"
                "<p>A fullscreen screen magnifier like the built-in Windows screen magnifier, "
                "but with easy <b>touchpad / touchscreen gestures</b> for laptops.</p>"
                "<p><b>How it works:</b></p>"
                "<ul>"
                "<li>Uses the Windows <b>Magnification API</b> fullscreen transform "
                "(GPU, no flicker, no screen capture).</li>"
                "<li><b>Follow-cursor:</b> the zoomed view is recentered on your "
                "cursor ~60 times/sec, clamped so you never see black edges.</li>"
                "<li><b>Controls:</b> ACTIVATE button or <b>F8</b> / <b>Ctrl+Alt+M</b> "
                "to arm, <b>+/-</b> buttons to zoom, "
                "<b>%2+wheel anywhere</b> to zoom at the cursor, "
                "pinch over the panel on touchscreens.</li>"
                "<li>The control panel excludes itself from magnification so it "
                "stays small and clickable while everything else is zoomed.</li>"
                "</ul>"
                "<p>Tip: remap the wheel key in <b>General</b>, check the <b>Update</b> "
                "tab for new GitHub releases.</p>")
            .arg(QString::fromLatin1(kMagnivoVersion), ZoomMod::name(ZoomMod::load())));
    alay->addWidget(label);
    alay->addStretch(1);
    m_tabs->addTab(about, "About");

    m_nam = new QNetworkAccessManager(this);
    m_timer = new QElapsedTimer();

    connect(m_checkBtn, &QPushButton::clicked, this, &SettingsDialog::startCheck);
    connect(m_downloadBtn, &QPushButton::clicked, this, &SettingsDialog::startDownload);
    connect(m_installBtn, &QPushButton::clicked, this, &SettingsDialog::onInstallClicked);
    connect(m_tabs, &QTabWidget::currentChanged, this, &SettingsDialog::onTabChanged);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(buttons);

    setMinimumSize(460, 400);
    m_tabs->setCurrentIndex(qBound(0, initialTab, 2));
    if (m_tabs->currentIndex() == UpdateTab)
        QTimer::singleShot(0, this, &SettingsDialog::startCheck);
}

void SettingsDialog::onTabChanged(int index) {
    if (index == UpdateTab && !m_updateChecked)
        startCheck();
}

int SettingsDialog::selectedModifier() const {
    return m_modBox->currentData().toInt();
}
QString SettingsDialog::selectedRepo() const {
    return m_repoEdit->text().trimmed();
}

// ---------------- Updater logic (lives in SettingsDialog::UpdateTab) ----------------

void SettingsDialog::setStatus(const QString &t) {
    m_status->setText(t);
}

QString SettingsDialog::fmtSize(qint64 bytes) {
    double b = double(bytes);
    if (b < 1024) return QString("%1 B").arg(bytes);
    if (b < 1024 * 1024) return QString("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    if (b < 1024 * 1024 * 1024) return QString("%1 MB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString SettingsDialog::fmtSpeed(double bytesPerSec) {
    if (bytesPerSec <= 0) return "--";
    return fmtSize(qint64(bytesPerSec)) + "/s";
}

void SettingsDialog::startCheck() {
    m_updateChecked = true;
    setStatus("Checking for updates...");
    m_checkBtn->setEnabled(false);
    m_downloadBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    m_bar->setValue(0);
    m_stats->setText("");
    m_downloadUrl.clear();
    m_latestTag.clear();

    // Prefer the repo currently typed in General tab so users don't have
    // to press OK first before checking.
    QString repo = m_repoEdit ? m_repoEdit->text().trimmed() : QString();
    if (repo.isEmpty())
        repo = ZoomMod::githubRepo();
    if (repo.isEmpty()) {
        setStatus("No GitHub repo set. Enter owner/repo in the General tab first.");
        m_checkBtn->setEnabled(true);
        return;
    }
    QUrl url(QString("https://api.github.com/repos/%1/releases/latest").arg(repo));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Magnivo-Updater");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (m_checkReply) { m_checkReply->abort(); m_checkReply->deleteLater(); }
    m_checkReply = m_nam->get(req);
    connect(m_checkReply, &QNetworkReply::finished, this, &SettingsDialog::onCheckFinished);
}

void SettingsDialog::onCheckFinished() {
    m_checkBtn->setEnabled(true);
    QNetworkReply *r = m_checkReply;
    m_checkReply = nullptr;
    if (!r) return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
        setStatus(QString("Check failed: %1").arg(r->errorString()));
        return;
    }
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r->readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus("Check failed: bad response from GitHub.");
        return;
    }
    QJsonObject obj = doc.object();
    QString tag = obj.value("tag_name").toString().trimmed(); // e.g. "v1.1.0"
    if (tag.isEmpty()) {
        setStatus("Check failed: release has no tag_name.");
        return;
    }
    m_latestTag = tag;
    QString cleanLatest = tag.startsWith('v') || tag.startsWith('V') ? tag.mid(1) : tag;
    QVersionNumber cur = QVersionNumber::fromString(QString::fromLatin1(kMagnivoVersion));
    QVersionNumber lat = QVersionNumber::fromString(cleanLatest);
    QString body = obj.value("body").toString();
    if (body.length() > 220) body = body.left(220) + "...";
    m_detail->setText(QString("Current: v%1   •   Latest: %2%3")
                          .arg(QString::fromLatin1(kMagnivoVersion), tag,
                               body.isEmpty() ? "" : "\n" + body));
    if (!lat.isNull() && !cur.isNull() && lat <= cur) {
        setStatus("You're up to date.");
        return;
    }
    // Pick a downloadable asset: prefer .exe, then .msi, then .zip
    QJsonArray assets = obj.value("assets").toArray();
    QString bestUrl, bestName;
    qint64 bestSize = 0;
    auto findExt = [&](const QString &ext) -> bool {
        for (const QJsonValue &v : assets) {
            QJsonObject a = v.toObject();
            QString nm = a.value("name").toString();
            if (nm.endsWith(ext, Qt::CaseInsensitive)) {
                bestUrl = a.value("browser_download_url").toString();
                bestName = nm;
                bestSize = qint64(a.value("size").toDouble());
                if (!bestUrl.isEmpty()) return true;
            }
        }
        return false;
    };
    if (!findExt(".exe") && !findExt(".msi") && !findExt(".zip")) {
        // fallback: first asset with a URL
        for (const QJsonValue &v : assets) {
            QJsonObject a = v.toObject();
            QString u = a.value("browser_download_url").toString();
            if (!u.isEmpty()) {
                bestUrl = u;
                bestName = a.value("name").toString();
                bestSize = qint64(a.value("size").toDouble());
                break;
            }
        }
    }
    if (bestUrl.isEmpty()) {
        setStatus(QString("Update %1 found, but it has no downloadable file.").arg(tag));
        return;
    }
    m_downloadUrl = bestUrl;
    m_fileName = bestName.isEmpty() ? QString("Magnivo-%1-update").arg(tag) : bestName;
    m_assetSize = bestSize;
    setStatus(QString("Update available: %1 (%2)").arg(tag, bestName));
    m_stats->setText(bestSize > 0 ? QString("Size: %1").arg(fmtSize(bestSize)) : "");
    m_downloadBtn->setEnabled(true);
}

void SettingsDialog::startDownload() {
    if (m_downloadUrl.isEmpty()) return;
    m_downloadBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    m_bar->setValue(0);
    m_stats->setText("Starting...");

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    QDir().mkpath(dir);
    m_savePath = dir + "/" + m_fileName;
    if (m_file) { m_file->deleteLater(); m_file = nullptr; }
    m_file = new QFile(m_savePath, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus("Cannot write file: " + m_savePath);
        m_file->deleteLater(); m_file = nullptr;
        m_downloadBtn->setEnabled(true);
        return;
    }
    QNetworkRequest req{QUrl(m_downloadUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, "Magnivo-Updater");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (m_dlReply) { m_dlReply->abort(); m_dlReply->deleteLater(); }
    m_dlReply = m_nam->get(req);
    m_timer->start();
    m_lastReceived = 0;
    m_lastMs = 0;
    m_speed = 0;
    connect(m_dlReply, &QNetworkReply::downloadProgress, this, &SettingsDialog::onDownloadProgress);
    connect(m_dlReply, &QNetworkReply::finished, this, &SettingsDialog::onDownloadFinished);
    connect(m_dlReply, &QIODevice::readyRead, this, [this]() {
        if (m_file && m_dlReply) m_file->write(m_dlReply->readAll());
    });
    setStatus("Downloading " + m_fileName + "...");
}

void SettingsDialog::onDownloadProgress(qint64 received, qint64 total) {
    qint64 nowMs = m_timer->elapsed();
    qint64 dBytes = received - m_lastReceived;
    qint64 dMs = nowMs - m_lastMs;
    if (dMs > 300 && dBytes >= 0) {
        m_speed = dBytes * 1000.0 / qMax<qint64>(1, dMs);
        m_lastReceived = received;
        m_lastMs = nowMs;
    }
    qint64 effTotal = total > 0 ? total : m_assetSize;
    if (effTotal > 0)
        m_bar->setValue(int(double(received) * 100.0 / double(effTotal)));
    m_stats->setText(QString("%1 / %2  •  %3")
                         .arg(fmtSize(received),
                              effTotal > 0 ? fmtSize(effTotal) : "?",
                              fmtSpeed(m_speed)));
}

void SettingsDialog::onDownloadFinished() {
    QNetworkReply *r = m_dlReply;
    m_dlReply = nullptr;
    if (!r) return;
    // flush any remaining bytes
    if (m_file) {
        m_file->write(r->readAll());
        m_file->close();
    }
    bool ok = (r->error() == QNetworkReply::NoError);
    QString err = r->errorString();
    r->deleteLater();
    if (!ok) {
        setStatus(QString("Download failed: %1").arg(err));
        m_downloadBtn->setEnabled(true);
        return;
    }
    QFileInfo fi(m_savePath);
    m_bar->setValue(100);
    m_stats->setText(QString("%1 downloaded").arg(fmtSize(fi.size())));
    setStatus("Download complete: " + fi.fileName() + "\nSaved to: " + m_savePath);
    m_installBtn->setEnabled(true);
    m_downloadBtn->setEnabled(false);
}

void SettingsDialog::onInstallClicked() {
    if (m_savePath.isEmpty() || !QFile::exists(m_savePath)) return;
    setStatus("Launching installer...");
    // Run detached so it survives our quit (needed to replace the running exe).
    if (!QProcess::startDetached(m_savePath, {})) {
        // fallback: open via shell
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_savePath));
    }
    qApp->quit();
}

// ---------------- Magnivo ----------------

Magnivo::Magnivo(QObject *parent) : QObject(parent) {
    m_panel = new ControlPanel();

#ifdef Q_OS_WIN
    g_zoomModVk = ZoomMod::load();
#endif

    // Pinch is gated: ignored unless armed or modifier held (see gestureAllowed).
    // This is the fix for "wrong gestures all the time".
    connect(m_panel, &ControlPanel::pinchDelta, this, [this](double delta){
        if (!gestureAllowed()) return;
        setZoom(m_zoom + float(delta) * 1.5f);
    });
    connect(m_panel, &ControlPanel::pinchScale, this, [this](double factor){
        if (!gestureAllowed()) return;
        setZoom(m_zoom * float(factor));
    });
    // Buttons / wheel are intentional, so they auto-activate.
    connect(m_panel, &ControlPanel::zoomInClicked, this, &Magnivo::zoomIn);
    connect(m_panel, &ControlPanel::zoomOutClicked, this, &Magnivo::zoomOut);
    connect(m_panel, &ControlPanel::armToggled, this, &Magnivo::setArmed);
    connect(m_panel, &ControlPanel::closeClicked, qApp, &QApplication::quit);
    connect(m_panel, &ControlPanel::settingsClicked, this, &Magnivo::openSettings);
    m_panel->setModifierName(ZoomMod::name(ZoomMod::load()));

#ifdef Q_OS_WIN
    m_magOk = magInitialize();
    qDebug() << "Magnivo: MagInitialize ok =" << m_magOk << "err =" << (m_magOk ? 0 : (int)GetLastError());
    if (m_magOk && pMagSetFullscreenColorEffect) {
        // Explicit identity color matrix. On some drivers the default
        // color effect is black (all zeros) -> entire screen goes black.
        MagColorEffect identity = {{
            {1,0,0,0,0},
            {0,1,0,0,0},
            {0,0,1,0,0},
            {0,0,0,1,0},
            {0,0,0,0,1}
        }};
        BOOL cok = pMagSetFullscreenColorEffect(&identity);
        qDebug() << "Magnivo: SetFullscreenColorEffect(identity) =" << (int)cok;
    }
#else
    m_magOk = false;
#endif

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &Magnivo::tick);
    m_timer->start(16); // 60fps transform update, no capture so no flicker

    installGlobalHooks();
    m_panel->setZoom(m_zoom);
    m_panel->setArmed(false);
    // NOTE: no applyTransform() here on purpose. Screen stays untouched
    // until the user arms with ACTIVATE/F8/<Mod>+wheel. This avoids
    // black-on-launch.
}

Magnivo::~Magnivo() {
#ifdef Q_OS_WIN
    // Back to normal only if we actually zoomed.
    if (m_magOk && m_transformActive) {
        magSetFullscreenTransform(1.0f, 0, 0);
        m_transformActive = false;
    }
    if (m_magOk) magUninitialize();
#endif
    uninstallGlobalHooks();
    delete m_panel;
}

void Magnivo::show() {
    QScreen *scr = QGuiApplication::primaryScreen();
    QRect geo = scr ? scr->geometry() : QRect(0, 0, 1920, 1080);
    m_panel->move(geo.right() - 320, geo.bottom() - 240);
    m_panel->show();

#ifdef Q_OS_WIN
    // Keep our own control panel unmagnified so it stays small/clickable
    // while everything else is zoomed fullscreen. Best-effort: if this
    // fails we still zoom, panel just gets magnified too.
    if (m_magOk && pMagSetWindowFilterList) {
        HWND hwndPanel = reinterpret_cast<HWND>(m_panel->winId());
        BOOL fok = magSetWindowFilterList(nullptr, MW_FILTERMODE_EXCLUDE, 1, &hwndPanel);
        qDebug() << "Magnivo: SetWindowFilterList(exclude panel) =" << (int)fok << "err =" << (fok ? 0 : (int)GetLastError());
    }
#endif
    // no transform here - stays normal until armed
}

void Magnivo::setZoom(float z) {
    z = qBound(1.0f, z, 8.0f);
    // Zooming out to (or staying at) 100% while OFF should NOT arm.
    // This prevents ACTIVATE from ever jumping to 200% and stops
    // pinch-out at minimum from needlessly arming.
    if (z <= 1.001f && !m_armed) {
        m_zoom = 1.0f;
        return;
    }
    m_zoom = z;
    // Explicit zoom action auto-arms (user intent is clear).
    if (!m_armed) setArmed(true);
    else { m_panel->setZoom(m_zoom); applyTransform(); }
}

void Magnivo::zoomIn()  { setZoom(m_zoom + 0.25f); }
void Magnivo::zoomOut() {
    if (m_zoom <= 1.001f && !m_armed) return; // already OFF at 100%
    setZoom(m_zoom - 0.25f);
    if (m_zoom <= 1.001f) setArmed(false);
}

void Magnivo::setArmed(bool armed) {
    if (m_armed == armed) { applyTransform(); return; }
    m_armed = armed;
    if (!armed) {
        // Reset so the NEXT manual activation starts at 100% (no jump).
        // Wheel/+/- auto-arm path sets m_zoom BEFORE calling setArmed(true),
        // so resetting only on disarm never clobbers an intentional zoom.
        m_zoom = 1.0f;
    }
    m_panel->setArmed(armed);
    if (armed) m_panel->setZoom(m_zoom);
    applyTransform();
}

void Magnivo::toggleArmed() { setArmed(!m_armed); }

void Magnivo::openSettings() {
    openSettingsTab(SettingsDialog::GeneralTab);
}

void Magnivo::openSettingsTab(int tab) {
    SettingsDialog dlg(m_panel, tab);
    if (dlg.exec() == QDialog::Accepted) {
        int vk = dlg.selectedModifier();
        ZoomMod::save(vk);
        ZoomMod::setGithubRepo(dlg.selectedRepo());
#ifdef Q_OS_WIN
        g_zoomModVk = vk;
#endif
        m_panel->setModifierName(ZoomMod::name(vk));
    }
}

void Magnivo::checkForUpdates() {
    openSettingsTab(SettingsDialog::UpdateTab);
}

void Magnivo::showAbout() {
    openSettingsTab(SettingsDialog::AboutTab);
}

bool Magnivo::gestureAllowed() const {
#ifdef Q_OS_WIN
    return m_armed || isZoomModHeld();
#else
    return m_armed;
#endif
}

void Magnivo::tick() {
    // No hide/show, no grabWindow here - that's what caused the blinking.
    // We just move the GPU transform so the cursor stays centered.
    applyTransform();
}

void Magnivo::applyTransform() {
#ifdef Q_OS_WIN
    if (!m_magOk) return;
    if (!m_armed) {
        // Only reset once - spamming MagSetFullscreenTransform(1.0) at 60fps
        // is wasteful. m_transformActive tracks whether a zoom is live.
        if (m_transformActive) {
            magSetFullscreenTransform(1.0f, 0, 0);
            m_transformActive = false;
        }
        return;
    }
    POINT pt; GetCursorPos(&pt);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return;
    float mag = m_zoom < 1.0f ? 1.0f : m_zoom;
    // Armed at 100% = "listen for gestures but don't zoom": leave the screen
    // untouched instead of spamming an identity transform at 60fps.
    if (mag <= 1.001f) {
        if (m_transformActive) {
            magSetFullscreenTransform(1.0f, 0, 0);
            m_transformActive = false;
        }
        return;
    }
    // IMPORTANT: xOffset/yOffset are in UNMAGNIFIED coords, relative to the
    // top-left of the primary monitor (see MagSetFullscreenTransform docs).
    // The source rect shown fullscreen is [xOff, xOff + sw/mag].
    // To keep the cursor centered: xOff = cx - (sw/mag)/2.
    // Old code used magnified coords (sw/2 - mag*cx) which pushed the
    // source rect far off-screen -> black screen, or clamped to (0,0)
    // which looked like "zoom to top-left".
    int viewW = int(sw / mag);
    int viewH = int(sh / mag);
    if (viewW < 1) viewW = 1;
    if (viewH < 1) viewH = 1;
    int xOff = int(double(pt.x) - viewW / 2.0);
    int yOff = int(double(pt.y) - viewH / 2.0);
    // Clamp so source rect stays inside the desktop -> never show black
    // space past the edges. Valid range: [0, sw - sw/mag].
    int xMax = sw - viewW;
    int yMax = sh - viewH;
    if (xMax < 0) xMax = 0;
    if (yMax < 0) yMax = 0;
    xOff = qBound(0, xOff, xMax);
    yOff = qBound(0, yOff, yMax);
    if (magSetFullscreenTransform(mag, xOff, yOff))
        m_transformActive = true;
#endif
}

void Magnivo::installGlobalHooks() {
#ifdef Q_OS_WIN
    g_inst = this;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
#endif
}

void Magnivo::uninstallGlobalHooks() {
#ifdef Q_OS_WIN
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    if (g_inst == this) g_inst = nullptr;
#endif
}
