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
#include <QMenu>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QMessageBox>
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
#include <QPainter>
#include <QDebug>
#include <cmath>

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
typedef BOOL (WINAPI *PFN_MagSetInputTransform)(BOOL, const RECT*, const RECT*);
struct MagColorEffect { float transform[5][5]; };
typedef BOOL (WINAPI *PFN_MagSetFullscreenColorEffect)(MagColorEffect*);

static HMODULE g_magDll = nullptr;
static PFN_MagInitialize pMagInitialize = nullptr;
static PFN_MagUninitialize pMagUninitialize = nullptr;
static PFN_MagSetFullscreenTransform pMagSetFullscreenTransform = nullptr;
static PFN_MagSetWindowFilterList pMagSetWindowFilterList = nullptr;
static PFN_MagSetFullscreenColorEffect pMagSetFullscreenColorEffect = nullptr;
static PFN_MagSetInputTransform pMagSetInputTransform = nullptr;

static bool magLoad() {
    if (g_magDll) return pMagInitialize != nullptr;
    g_magDll = LoadLibraryW(L"Magnification.dll");
    if (!g_magDll) { qWarning() << "Magnivo: LoadLibrary(Magnification.dll) failed:" << GetLastError(); return false; }
    pMagInitialize = (PFN_MagInitialize)GetProcAddress(g_magDll, "MagInitialize");
    pMagUninitialize = (PFN_MagUninitialize)GetProcAddress(g_magDll, "MagUninitialize");
    pMagSetFullscreenTransform = (PFN_MagSetFullscreenTransform)GetProcAddress(g_magDll, "MagSetFullscreenTransform");
    pMagSetWindowFilterList = (PFN_MagSetWindowFilterList)GetProcAddress(g_magDll, "MagSetWindowFilterList");
    pMagSetFullscreenColorEffect = (PFN_MagSetFullscreenColorEffect)GetProcAddress(g_magDll, "MagSetFullscreenColorEffect");
    pMagSetInputTransform = (PFN_MagSetInputTransform)GetProcAddress(g_magDll, "MagSetInputTransform");
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
    // Hardcoded: users never type this anymore. Old per-user overrides are
    // ignored so "Check for updates" always hits our own releases.
    return QString::fromLatin1(kMagnivoUpdateRepo);
}
void ZoomMod::setGithubRepo(const QString &) {
    // No-op kept for compat (old settings files may still contain a value).
}

int Theme::load() {
    QSettings s;
    int t = s.value("appearance", Theme::Dark).toInt();
    return (t == Theme::Light) ? Theme::Light : Theme::Dark;
}
void Theme::save(int theme) {
    QSettings s;
    s.setValue("appearance", (theme == Theme::Light) ? Theme::Light : Theme::Dark);
}

#ifdef Q_OS_WIN
static Magnivo *g_inst = nullptr;
static HHOOK g_mouseHook = nullptr;
static HHOOK g_kbHook = nullptr;
static qint64 g_lastToggleMs = 0;
// Tags already shown/dismissed this session so the background monitor doesn't
// nag twice for the same release. Manual "Check for updates" always reports.
static QString g_notifiedTag;
static QString g_dismissedTag;

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
    // Handle vertical + horizontal wheel. Touchpads often emit high-res
    // partial deltas and/or injected Ctrl+wheel for pinch: treat every tick
    // proportionally so zoom-in then zoom-out always returns to start
    // (fixed +/-0.25 steps overshoot to max and feel "stuck").
    bool isWheel = (nCode == HC_ACTION &&
                    (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL) &&
                    g_inst);
    if (isWheel) {
        MSLLHOOKSTRUCT *ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        bool modHeld = isZoomModHeld();
        // When ACTIVE, plain wheel also zooms (that's what ACTIVE means:
        // gestures hijack the wheel until you press F8 again). When OFF,
        // only <Modifier>+wheel zooms so normal scrolling is untouched.
        // Either way we swallow the event so the app behind never does its
        // own per-app zoom (browser 150% etc.) while Magnivo zooms system-wide.
        // That split-brain (app zoomed, Magnivo at 100%) was the old
        // "zoomed in some app but can't zoom out" bug.
        bool armed = g_inst->armed();
        if (modHeld || armed) {
            int delta = (short)HIWORD(ms->mouseData);
            if (delta != 0) {
                Magnivo *inst = g_inst;
                QTimer::singleShot(0, inst, [inst, delta]() {
                    inst->zoomByWheelDelta(delta);
                });
            }
            return 1; // swallow even for tiny deltas: keeps app + Magnivo in sync
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

// ---------------- OptionDropDown ----------------

OptionDropDown::OptionDropDown(QWidget *parent) : QPushButton(parent) {
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, &OptionDropDown::showMenu);
}

void OptionDropDown::addItem(const QString &text, int data) {
    m_items.append({text, data});
    if (m_cur < 0) { m_cur = 0; refreshText(); }
}

int OptionDropDown::findData(int data) const {
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].data == data) return i;
    return -1;
}

int OptionDropDown::currentData() const {
    return (m_cur >= 0 && m_cur < m_items.size()) ? m_items[m_cur].data : -1;
}

int OptionDropDown::currentIndex() const { return m_cur; }

void OptionDropDown::setCurrentIndex(int i) {
    if (i < 0 || i >= m_items.size() || i == m_cur) return;
    m_cur = i;
    refreshText();
    emit currentIndexChanged(m_cur);
}

void OptionDropDown::setMenuStyleSheet(const QString &st) {
    m_menuStyle = st;
}

void OptionDropDown::refreshText() {
    // Glyph arrow drawn as text: visible in every theme, no image assets.
    QString t = (m_cur >= 0 && m_cur < m_items.size()) ? m_items[m_cur].text : QString();
    setText(t + QString::fromUtf8("   \u25be")); // ▾
}

void OptionDropDown::showMenu() {
    QMenu menu(this);
    if (!m_menuStyle.isEmpty()) menu.setStyleSheet(m_menuStyle);
    menu.setMinimumWidth(width());
    for (int i = 0; i < m_items.size(); ++i) {
        QAction *a = menu.addAction(m_items[i].text);
        a->setCheckable(true);
        a->setChecked(i == m_cur);
        a->setData(i);
    }
    QAction *picked = menu.exec(mapToGlobal(QPoint(0, height() + 2)));
    if (picked) setCurrentIndex(picked->data().toInt());
}

// ---------------- ControlPanel ----------------

ControlPanel::ControlPanel(QWidget *parent) : QWidget(parent), m_theme(Theme::load()) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setWindowIcon(QIcon(":/logo.png"));
    setAttribute(Qt::WA_StyledBackground, true);
    // Translucent window + painted rounded rect (see paintEvent): the only way
    // to get smooth anti-aliased round corners on a frameless window. The
    // 1-bit setMask() clip made them look chopped; stylesheet backgrounds
    // alone paint square corners.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    grabGesture(Qt::PinchGesture);

    auto *main = new QVBoxLayout(this);
    // Top margin 0 + right margin 0: the title bar sits flush against the
    // top window edge and the X sits right on the border, like a real bar.
    main->setContentsMargins(10, 0, 0, 10);
    main->setSpacing(8);

    // --- Title bar: app icon + name on the left, gear + X on the right ---
    auto *titleBar = new QHBoxLayout();
    titleBar->setSpacing(6);
    auto *iconLabel = new QLabel(this);
    QPixmap iconPx(":/logo.png");
    if (!iconPx.isNull())
        iconLabel->setPixmap(iconPx.scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    iconLabel->setFixedSize(20, 32);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true); // drag-through
    m_titleName = new QLabel("Magnivo", this);
    m_titleName->setObjectName("titleName");
    m_titleName->setAttribute(Qt::WA_TransparentForMouseEvents, true); // drag-through
    m_settingsBtn = new QPushButton(QString::fromUtf8("\u2699"), this); // gear icon
    m_settingsBtn->setObjectName("titleBtn");
    m_settingsBtn->setFixedSize(36, 28);
    m_settingsBtn->setCursor(Qt::PointingHandCursor);
    m_settingsBtn->setToolTip("Settings, updates and about");
    m_closeBtn = new QPushButton(QString::fromUtf8("\u2715"), this); // X on the border
    m_closeBtn->setObjectName("closeBtn");
    m_closeBtn->setFixedSize(44, 32);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setToolTip("Close - screen goes back to normal");
    titleBar->addWidget(iconLabel);
    titleBar->addWidget(m_titleName);
    titleBar->addStretch(1);
    titleBar->addWidget(m_settingsBtn, 0, Qt::AlignVCenter);
    titleBar->addWidget(m_closeBtn, 0, Qt::AlignTop);

    auto *armRow = new QHBoxLayout();
    armRow->setContentsMargins(0, 0, 10, 0);
    armRow->setSpacing(8);
    m_armBtn = new QPushButton("ACTIVATE (F8)", this);
    m_armBtn->setCheckable(true);
    m_armBtn->setChecked(false);
    m_armBtn->setMinimumHeight(48);
    m_armBtn->setCursor(Qt::PointingHandCursor);
    m_armBtn->setToolTip("Arm gestures.\nOFF: hold the zoom key + wheel to zoom (auto-arms).\nACTIVE: wheel / pinch zooms anywhere, F8 stops.");
    armRow->addWidget(m_armBtn, 1);

    auto *botRow = new QHBoxLayout();
    botRow->setContentsMargins(0, 0, 10, 0);
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

    main->addLayout(titleBar);
    main->addLayout(armRow);
    main->addLayout(botRow);

    connect(minusBtn, &QPushButton::clicked, this, &ControlPanel::zoomOutClicked);
    connect(plusBtn, &QPushButton::clicked, this, &ControlPanel::zoomInClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &ControlPanel::closeClicked);
    connect(m_armBtn, &QPushButton::toggled, this, &ControlPanel::armToggled);
    connect(m_settingsBtn, &QPushButton::clicked, this, &ControlPanel::settingsClicked);

    auto *esc = new QAction(this);
    esc->setShortcut(Qt::Key_Escape);
    connect(esc, &QAction::triggered, this, &ControlPanel::closeClicked);
    addAction(esc);

    setFixedSize(300, 156);
    applyTheme(m_theme);
    setArmed(false);
}

void ControlPanel::applyTheme(int theme) {
    m_theme = (theme == Theme::Light) ? Theme::Light : Theme::Dark;
    // NOTE: the panel fill/border are painted in paintEvent (smooth round
    // corners), not via stylesheet, so there is no ControlPanel{...} rule.
    if (m_theme == Theme::Light) {
        m_bgColor = QColor("#f4f4f5");
        m_borderColor = QColor("#d4d4d8");
        setStyleSheet("QLabel{color:#18181b;font-size:16px;font-weight:bold;}"
                      "QLabel#titleName{color:#18181b;font-size:13px;font-weight:bold;}"
                      "QPushButton{background:#e4e4e7;color:#18181b;border:1px solid #d4d4d8;border-radius:8px;font-size:16px;}"
                      "QPushButton:hover{background:#d4d4d8;border-color:#a1a1aa;}"
                      "QPushButton:disabled{background:#f4f4f5;color:#a1a1aa;border:1px solid #e4e4e7;}"
                      "QPushButton#titleBtn{background:transparent;border:none;border-radius:6px;font-size:18px;color:#52525b;}"
                      "QPushButton#titleBtn:hover{background:#d4d4d8;color:#18181b;}"
                      "QPushButton#closeBtn{background:transparent;border:none;border-top-right-radius:11px;font-size:13px;font-weight:bold;color:#52525b;}"
                      "QPushButton#closeBtn:hover{background:#e81123;color:white;}");
    } else {
        m_bgColor = QColor("#1e1e1e");
        m_borderColor = QColor("#555555");
        setStyleSheet("QLabel{color:white;font-size:16px;font-weight:bold;}"
                      "QLabel#titleName{color:white;font-size:13px;font-weight:bold;}"
                      "QPushButton{background:#333;color:white;border:1px solid #666;border-radius:8px;font-size:16px;}"
                      "QPushButton:hover{background:#444;}"
                      "QPushButton:disabled{background:#222;color:#777;border:1px solid #444;}"
                      "QPushButton#titleBtn{background:transparent;border:none;border-radius:6px;font-size:18px;color:#b0b0b0;}"
                      "QPushButton#titleBtn:hover{background:#444;color:white;}"
                      "QPushButton#closeBtn{background:transparent;border:none;border-top-right-radius:11px;font-size:13px;font-weight:bold;color:#b0b0b0;}"
                      "QPushButton#closeBtn:hover{background:#e81123;color:white;}");
    }
    refreshArmButton();
    update(); // repaint the rounded background
}

void ControlPanel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setBrush(m_bgColor);
    p.setPen(QPen(m_borderColor, 1));
    p.drawRoundedRect(r, 12, 12);
}

void ControlPanel::refreshArmButton() {
    bool armed = m_armBtn && m_armBtn->isChecked();
    if (armed) {
        m_armBtn->setText("ACTIVE - pinch to zoom");
        if (m_theme == Theme::Light)
            m_armBtn->setStyleSheet("QPushButton{background:#16a34a;color:white;border:1px solid #15803d;border-radius:8px;font-size:13px;font-weight:bold;}"
                                   "QPushButton:hover{background:#15803d;}");
        else
            m_armBtn->setStyleSheet("QPushButton{background:#1d5c2e;color:white;border:1px solid #4caf50;border-radius:8px;font-size:13px;font-weight:bold;}"
                                   "QPushButton:hover{background:#257a3c;}");
    } else {
        m_armBtn->setText("ACTIVATE (F8)");
        if (m_theme == Theme::Light)
            m_armBtn->setStyleSheet("QPushButton{background:#e4e4e7;color:#18181b;border:1px solid #d4d4d8;border-radius:8px;font-size:13px;}"
                                   "QPushButton:hover{background:#d4d4d8;border-color:#a1a1aa;}");
        else
            m_armBtn->setStyleSheet("QPushButton{background:#333;color:white;border:1px solid #666;border-radius:8px;font-size:13px;}"
                                   "QPushButton:hover{background:#444;}");
    }
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
    if (!armed)
        m_label->setText("OFF");
    refreshArmButton(); // text + colors for current theme
}

void ControlPanel::setModifierName(const QString &modName) {
    m_modName = modName;
    m_armBtn->setToolTip(QString("Arm gestures.\nOFF: hold %1 + wheel anywhere to zoom (auto-arms).\n"
                                "ACTIVE: wheel / pinch zooms anywhere. F8 or Ctrl+Alt+M toggles.").arg(m_modName));
}

bool ControlPanel::event(QEvent *e) {
    // Pinch directly over the panel (touchscreen / precision touchpad).
    // Pinch anywhere else never reaches us: Windows delivers bare gestures to
    // the window under the fingers (e.g. Desktop), so out there zooming goes
    // through the global wheel hook instead - <Modifier>+wheel when OFF,
    // plain wheel too when ACTIVE (see Magnivo hooks).
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

QString SettingsDialog::menuStyle(int theme) {
    if (theme == Theme::Light)
        return QString("QMenu{background:white;color:#18181b;border:1px solid #d4d4d8;padding:4px;}"
                       "QMenu::item{padding:7px 26px 7px 12px;background:transparent;}"
                       "QMenu::item:selected{background:#2563eb;color:white;}"
                       "QMenu::indicator{width:14px;height:14px;}");
    return QString("QMenu{background:#2d2d2d;color:white;border:1px solid #555;padding:4px;}"
                   "QMenu::item{padding:7px 26px 7px 12px;background:transparent;}"
                   "QMenu::item:selected{background:#3b82f6;color:white;}"
                   "QMenu::indicator{width:14px;height:14px;}");
}

QString SettingsDialog::settingsStyle(int theme) {
    if (theme == Theme::Light) {
        return QString(
            "SettingsDialog{background:#f4f4f5;}"
            "QTabWidget::pane{border:1px solid #d4d4d8;border-radius:8px;background:white;}"
            "QTabBar::tab{background:#e4e4e7;color:#3f3f46;padding:8px 18px;margin-right:4px;border-top-left-radius:8px;border-top-right-radius:8px;font-size:13px;}"
            "QTabBar::tab:selected{background:white;color:#09090b;font-weight:bold;}"
            "QLabel{color:#18181b;font-size:13px;}"
            "QLabel#title{font-size:15px;font-weight:bold;}"
            "QLabel#hint{color:#71717a;font-size:12px;}"
            "QLabel#subtle{color:#52525b;font-size:12px;}"
            "QProgressBar{background:#e4e4e7;border:1px solid #d4d4d8;border-radius:6px;text-align:center;color:#18181b;font-size:12px;}"
            "QProgressBar::chunk{background:#2563eb;border-radius:4px;}"
            "QPushButton{background:#e4e4e7;color:#18181b;border:1px solid #d4d4d8;border-radius:6px;padding:8px 16px;font-size:13px;}"
            "QPushButton:hover{background:#d4d4d8;border-color:#a1a1aa;}"
            "QPushButton:disabled{background:#f4f4f5;color:#a1a1aa;border:1px solid #e4e4e7;}"
            "QPushButton#primary{background:#2563eb;color:white;border:1px solid #2563eb;font-weight:bold;}"
            "QPushButton#primary:hover{background:#1d4ed8;border-color:#1d4ed8;}"
            "QPushButton#primary:disabled{background:#bfdbfe;border-color:#bfdbfe;color:#eff6ff;}"
            "QDialogButtonBox QPushButton{background:#e4e4e7;color:#18181b;border:1px solid #d4d4d8;border-radius:6px;padding:8px 22px;font-size:13px;min-width:80px;}"
            "QDialogButtonBox QPushButton:hover{background:#d4d4d8;border-color:#71717a;}"
            "QDialogButtonBox QPushButton[text=\"OK\"]{background:#2563eb;color:white;border:1px solid #2563eb;font-weight:bold;}"
            "QDialogButtonBox QPushButton[text=\"OK\"]:hover{background:#1d4ed8;border-color:#1e40af;}");
    }
    // Dark (default, matches the control panel).
    return QString(
        "SettingsDialog{background:#1e1e1e;}"
        "QTabWidget::pane{border:1px solid #444;border-radius:8px;background:#252526;}"
        "QTabBar::tab{background:#2d2d2d;color:#b0b0b0;padding:8px 18px;margin-right:4px;border-top-left-radius:8px;border-top-right-radius:8px;font-size:13px;}"
        "QTabBar::tab:selected{background:#252526;color:white;font-weight:bold;}"
        "QLabel{color:#e8e8e8;font-size:13px;}"
        "QLabel#title{font-size:15px;font-weight:bold;color:white;}"
        "QLabel#hint{color:#a1a1aa;font-size:12px;}"
        "QLabel#subtle{color:#b0b0b0;font-size:12px;}"
        "QProgressBar{background:#333;border:1px solid #555;border-radius:6px;text-align:center;color:#e8e8e8;font-size:12px;}"
        "QProgressBar::chunk{background:#3b82f6;border-radius:4px;}"
        "QPushButton{background:#333;color:white;border:1px solid #555;border-radius:6px;padding:8px 16px;font-size:13px;}"
        "QPushButton:hover{background:#444;border-color:#888;}"
        "QPushButton:disabled{background:#252526;color:#777;border:1px solid #444;}"
        "QPushButton#primary{background:#2563eb;color:white;border:1px solid #2563eb;font-weight:bold;}"
        "QPushButton#primary:hover{background:#3b82f6;border-color:#3b82f6;}"
        "QPushButton#primary:disabled{background:#333;border-color:#444;color:#777;}"
        "QDialogButtonBox QPushButton{background:#333;color:white;border:1px solid #555;border-radius:6px;padding:8px 22px;font-size:13px;min-width:80px;}"
        "QDialogButtonBox QPushButton:hover{background:#444;border-color:#888;}"
        "QDialogButtonBox QPushButton[text=\"OK\"]{background:#2563eb;color:white;border:1px solid #2563eb;font-weight:bold;}"
        "QDialogButtonBox QPushButton[text=\"OK\"]:hover{background:#3b82f6;border-color:#60a5fa;}");
}

QString SettingsDialog::messageBoxStyle(int theme) {
    if (theme == Theme::Light)
        return QString("QMessageBox{background:#ffffff;} QLabel{color:#18181b;font-size:13px;}"
                       "QPushButton{background:#e4e4e7;color:#18181b;border:1px solid #d4d4d8;border-radius:6px;padding:8px 22px;min-width:80px;}"
                       "QPushButton:hover{background:#d4d4d8;border-color:#71717a;}");
    return QString("QMessageBox{background:#252526;} QLabel{color:#e8e8e8;font-size:13px;}"
                   "QPushButton{background:#333;color:white;border:1px solid #555;border-radius:6px;padding:8px 22px;min-width:80px;}"
                   "QPushButton:hover{background:#444;border-color:#888;}");
}

void SettingsDialog::applyTheme(int theme) {
    if (theme != Theme::Light) theme = Theme::Dark;
    setStyleSheet(settingsStyle(theme));
    const QString ms = menuStyle(theme);
    if (m_modBox) m_modBox->setMenuStyleSheet(ms);
    if (m_themeBox) m_themeBox->setMenuStyleSheet(ms);
}

void SettingsDialog::onThemeChanged(int) {
    int theme = m_themeBox ? m_themeBox->currentData() : Theme::Dark;
    applyTheme(theme);
}

SettingsDialog::SettingsDialog(QWidget *parent, int initialTab) : QDialog(parent) {
    setWindowTitle("Magnivo Settings");
    setWindowIcon(QIcon(":/logo.png"));
    setModal(true);

    // ---- Theme (dark default) + OK/Cancel hover feedback ----
    applyTheme(Theme::load());

    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);
    m_tabs = new QTabWidget(this);
    mainLay->addWidget(m_tabs);

    // --- General tab: zoom key + appearance ---
    auto *general = new QWidget(this);
    auto *glay = new QVBoxLayout(general);
    glay->setContentsMargins(20, 20, 20, 20);
    glay->setSpacing(12);
    auto *title = new QLabel("Zoom key", general);
    title->setObjectName("title");
    auto *desc = new QLabel("Hold this key and use the mouse wheel or touchpad scroll anywhere to zoom.", general);
    desc->setWordWrap(true);
    auto *modRow = new QHBoxLayout();
    auto *modLabel = new QLabel("Zoom key + wheel:", general);
    modLabel->setMinimumWidth(130);
    m_modBox = new OptionDropDown(general);
    m_modBox->setMinimumWidth(170);
    m_modBox->addItem("Ctrl", ZoomMod::Ctrl);
    m_modBox->addItem("Alt", ZoomMod::Alt);
    m_modBox->addItem("Shift", ZoomMod::Shift);
    m_modBox->addItem("Win", ZoomMod::Win);
    int cur = ZoomMod::load();
    int idx = m_modBox->findData(cur);
    if (idx >= 0) m_modBox->setCurrentIndex(idx);
    m_modBox->setToolTip("Hold this key + mouse wheel anywhere to zoom");
    modRow->addWidget(modLabel);
    modRow->addWidget(m_modBox);
    modRow->addStretch(1);
    auto *themeRow = new QHBoxLayout();
    auto *themeLabel = new QLabel("Appearance:", general);
    themeLabel->setMinimumWidth(130);
    m_themeBox = new OptionDropDown(general);
    m_themeBox->setMinimumWidth(170);
    m_themeBox->addItem("Dark", Theme::Dark);
    m_themeBox->addItem("Light", Theme::Light);
    int curTheme = Theme::load();
    int tidx = m_themeBox->findData(curTheme);
    if (tidx >= 0) m_themeBox->setCurrentIndex(tidx);
    m_themeBox->setToolTip("Dark or light settings window");
    themeRow->addWidget(themeLabel);
    themeRow->addWidget(m_themeBox);
    themeRow->addStretch(1);
    auto *hint = new QLabel("On the desktop: hold the zoom key + two-finger scroll, "
                            "or press ACTIVATE (F8) first and then scroll / pinch with no key.\n"
                            "Bare pinch only works when ACTIVE (or right over the Magnivo panel): "
                            "when OFF it is ignored so you never zoom by accident.\n"
                            "Press F8 again to get normal scrolling back.", general);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    glay->addWidget(title);
    glay->addWidget(desc);
    glay->addLayout(modRow);
    glay->addLayout(themeRow);
    glay->addWidget(hint);
    glay->addStretch(1);
    m_tabs->addTab(general, "General");

    // --- Update tab: one button, clear status ---
    auto *update = new QWidget(this);
    auto *ulay = new QVBoxLayout(update);
    ulay->setContentsMargins(20, 20, 20, 20);
    ulay->setSpacing(10);
    m_status = new QLabel("Press \"Check for updates\" to see if a new version is available.", update);
    m_status->setWordWrap(true);
    m_status->setStyleSheet("QLabel{font-size:14px;font-weight:bold;}");
    m_detail = new QLabel(QString("Current version: v%1").arg(QString::fromLatin1(kMagnivoVersion)), update);
    m_detail->setWordWrap(true);
    m_detail->setObjectName("subtle");
    m_bar = new QProgressBar(update);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(true);
    m_stats = new QLabel("", update);
    m_stats->setObjectName("subtle");

    auto *btnRow = new QHBoxLayout();
    m_checkBtn = new QPushButton("Check for updates", update);
    m_checkBtn->setObjectName("primary");
    m_checkBtn->setCursor(Qt::PointingHandCursor);
    m_checkBtn->setMinimumHeight(36);
    m_installBtn = new QPushButton("Install && Restart", update);
    m_installBtn->setCursor(Qt::PointingHandCursor);
    m_installBtn->setMinimumHeight(36);
    m_installBtn->setEnabled(false);
    btnRow->addWidget(m_checkBtn);
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
    alay->setContentsMargins(20, 20, 20, 20);
    alay->setSpacing(8);
    auto *logoLabel = new QLabel(about);
    QPixmap logoPx(":/logo.png");
    if (!logoPx.isNull())
        logoLabel->setPixmap(logoPx.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignHCenter);
    alay->addWidget(logoLabel);
    auto *label = new QLabel(about);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setText(
        QString("<h2>Magnivo v%1</h2>"
                "<p>Fullscreen magnifier with touchpad / touchscreen gestures.</p>"
                "<p><b>Use:</b> hold <b>%2 + wheel</b> anywhere to zoom at the cursor, "
                "or press <b>ACTIVATE (F8)</b> then use the wheel / pinch with no keys. "
                "<b>+ / -</b> zoom in steps.</p>"
                "<p><b>Desktop tip:</b> a bare pinch with no key only zooms while ACTIVE "
                "(or directly over the Magnivo panel). When OFF it is ignored on purpose, "
                "so use <b>%2 + scroll</b> there.</p>"
                "<p>Updates are checked automatically and in the <b>Update</b> tab.</p>")
            .arg(QString::fromLatin1(kMagnivoVersion), ZoomMod::name(ZoomMod::load())));
    alay->addWidget(label);
    alay->addStretch(1);
    m_tabs->addTab(about, "About");

    m_nam = new QNetworkAccessManager(this);
    m_timer = new QElapsedTimer();

    connect(m_checkBtn, &QPushButton::clicked, this, &SettingsDialog::startCheck);
    connect(m_installBtn, &QPushButton::clicked, this, &SettingsDialog::onInstallClicked);
    connect(m_tabs, &QTabWidget::currentChanged, this, &SettingsDialog::onTabChanged);
    connect(m_themeBox, &OptionDropDown::currentIndexChanged,
            this, &SettingsDialog::onThemeChanged);
    applyTheme(Theme::load()); // again now that the dropdowns exist (menu style)

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setCursor(Qt::PointingHandCursor);
    buttons->button(QDialogButtonBox::Cancel)->setCursor(Qt::PointingHandCursor);
    buttons->button(QDialogButtonBox::Ok)->setToolTip("Save and close");
    buttons->button(QDialogButtonBox::Cancel)->setToolTip("Close without saving");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(buttons);

    setMinimumSize(480, 420);
    m_tabs->setCurrentIndex(qBound(0, initialTab, 2));
    if (m_tabs->currentIndex() == UpdateTab)
        QTimer::singleShot(0, this, &SettingsDialog::startCheck);
}

void SettingsDialog::onTabChanged(int index) {
    if (index == UpdateTab && !m_updateChecked)
        startCheck();
}

int SettingsDialog::selectedModifier() const {
    return m_modBox ? m_modBox->currentData() : ZoomMod::Ctrl;
}

int SettingsDialog::selectedTheme() const {
    int t = m_themeBox ? m_themeBox->currentData() : Theme::Dark;
    return (t == Theme::Light) ? Theme::Light : Theme::Dark;
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
    m_status->setToolTip("");
    m_checkBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    m_bar->setValue(0);
    m_stats->setText("");
    m_downloadUrl.clear();
    m_latestTag.clear();

    QString repo = QString::fromLatin1(kMagnivoUpdateRepo);
    QUrl url(QString("https://api.github.com/repos/%1/releases/latest").arg(repo));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Magnivo-Updater");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (m_checkReply) { m_checkReply->abort(); m_checkReply->deleteLater(); }
    m_checkReply = m_nam->get(req);
    connect(m_checkReply, &QNetworkReply::finished, this, &SettingsDialog::onCheckFinished);
}

void SettingsDialog::showUpdateAvailablePopup(const QString &tag, const QString &notes) {
    QString text = QString("A new version of Magnivo is available.\n\nYou have v%1, latest is %2.%3\n\nUpdate now?")
                       .arg(QString::fromLatin1(kMagnivoVersion), tag,
                            notes.isEmpty() ? QString() : "\n\n" + notes);
    QMessageBox box(this);
    box.setWindowTitle("Magnivo update available");
    box.setStyleSheet(messageBoxStyle(selectedTheme()));
    box.setText(text);
    box.setInformativeText("Choose Update to download it now, or Cancel to stay on this version.");
    QPushButton *updateBtn = box.addButton("Update", QMessageBox::AcceptRole);
    QPushButton *cancelBtn = box.addButton("Cancel", QMessageBox::RejectRole);
    updateBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    box.setDefaultButton(updateBtn);
    box.exec();
    if (box.clickedButton() == updateBtn) {
        startDownload(); // user chose Update -> download straight away
    } else {
#ifdef Q_OS_WIN
        g_dismissedTag = tag; // don't nag again for this version
#endif
    }
}

void SettingsDialog::onCheckFinished() {
    m_checkBtn->setEnabled(true);
    QNetworkReply *r = m_checkReply;
    m_checkReply = nullptr;
    if (!r) return;
    // Keep copies for the tooltip/debug BEFORE deleteLater, but never show them.
    QNetworkReply::NetworkError errCode = r->error();
    int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString techDetail = QString("Update check failed: %1 (code %2, http %3)")
                             .arg(r->errorString()).arg(int(errCode)).arg(httpCode);
    r->deleteLater();
    if (errCode != QNetworkReply::NoError) {
        qDebug() << "Magnivo:" << techDetail;
        QString friendly;
        if (httpCode == 404)
            friendly = QString("No releases published yet, so you're up to date.");
        else if (httpCode == 403 || httpCode == 429)
            friendly = QString("GitHub is busy right now. Try again in a bit.");
        else if (errCode == QNetworkReply::HostNotFoundError ||
                 errCode == QNetworkReply::TimeoutError ||
                 errCode == QNetworkReply::ConnectionRefusedError ||
                 errCode == QNetworkReply::UnknownNetworkError)
            friendly = QString("Couldn't reach the update server. Check your internet and try again.");
        else
            friendly = QString("Couldn't check for updates. Try again in a bit.");
        setStatus(friendly);
        m_status->setToolTip(techDetail);
        m_detail->setText(QString("Current version: v%1").arg(QString::fromLatin1(kMagnivoVersion)));
        return;
    }
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r->readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus("Couldn't check for updates (bad response). Try again later.");
        return;
    }
    QJsonObject obj = doc.object();
    QString tag = obj.value("tag_name").toString().trimmed(); // e.g. "v1.1.0"
    if (tag.isEmpty()) {
        // No releases published yet -> we are trivially on the latest.
        setStatus("You're up to date.");
        m_detail->setText(QString("Current version: v%1  •  Latest version: v%1").arg(QString::fromLatin1(kMagnivoVersion)));
        return;
    }
    m_latestTag = tag;
    QString cleanLatest = tag.startsWith('v') || tag.startsWith('V') ? tag.mid(1) : tag;
    QVersionNumber cur = QVersionNumber::fromString(QString::fromLatin1(kMagnivoVersion));
    QVersionNumber lat = QVersionNumber::fromString(cleanLatest);
    QString body = obj.value("body").toString().trimmed();
    QString shortNotes = body;
    if (shortNotes.length() > 220) shortNotes = shortNotes.left(220) + "...";
    m_detail->setText(QString("Current version: v%1   •   Latest version: %2%3")
                          .arg(QString::fromLatin1(kMagnivoVersion), tag,
                               shortNotes.isEmpty() ? "" : "\n" + shortNotes));
    if (!lat.isNull() && !cur.isNull() && lat <= cur) {
        setStatus("You're up to date. You have the latest version.");
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
        setStatus(QString("Update %1 is available, but it has no downloadable file.").arg(tag));
        return;
    }
    m_downloadUrl = bestUrl;
    m_fileName = bestName.isEmpty() ? QString("Magnivo-%1-update").arg(tag) : bestName;
    m_assetSize = bestSize;
    setStatus(QString("New update available: %1").arg(tag));
    m_stats->setText(bestSize > 0 ? QString("Size: %1").arg(fmtSize(bestSize)) : "");
#ifdef Q_OS_WIN
    // If the startup monitor already asked about this exact version and the
    // user pressed Update, skip the second popup and download immediately.
    if (!g_notifiedTag.isEmpty() && g_notifiedTag == tag) {
        g_notifiedTag.clear();
        startDownload();
        return;
    }
#endif
    showUpdateAvailablePopup(tag, shortNotes);
}

void SettingsDialog::startDownload() {
    if (m_downloadUrl.isEmpty()) return;
    m_checkBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    m_bar->setValue(0);
    m_stats->setText("Starting download...");

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    QDir().mkpath(dir);
    m_savePath = dir + "/" + m_fileName;
    if (m_file) { m_file->deleteLater(); m_file = nullptr; }
    m_file = new QFile(m_savePath, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus("Couldn't save the download here: " + m_savePath);
        m_file->deleteLater(); m_file = nullptr;
        m_checkBtn->setEnabled(true);
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
    QString techDlDetail = r->errorString();
    r->deleteLater();
    if (!ok) {
        qDebug() << "Magnivo: download failed:" << techDlDetail;
        setStatus("Download didn't finish. Check your connection and press \"Check for updates\" to try again.");
        m_status->setToolTip(QString("Download failed: %1").arg(techDlDetail));
        m_checkBtn->setEnabled(true);
        return;
    }
    QFileInfo fi(m_savePath);
    m_bar->setValue(100);
    m_stats->setText(QString("%1 downloaded").arg(fmtSize(fi.size())));
    setStatus("Download complete. Press \"Install && Restart\" to update.");
    m_installBtn->setEnabled(true);
    m_checkBtn->setEnabled(true);
    m_installBtn->setFocus();
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

    // Background update monitor: check once shortly after startup. If a new
    // release exists, pop up "Update or Cancel". Silent when up to date.
    QTimer::singleShot(4000, this, &Magnivo::scheduleAutoUpdateCheck);
}

Magnivo::~Magnivo() {
#ifdef Q_OS_WIN
    if (m_updateReply) { m_updateReply->abort(); m_updateReply->deleteLater(); m_updateReply = nullptr; }
#endif
#ifdef Q_OS_WIN
    // Back to normal only if we actually zoomed.
    if (m_magOk && m_transformActive) {
        magSetFullscreenTransform(1.0f, 0, 0);
        if (pMagSetInputTransform) pMagSetInputTransform(FALSE, nullptr, nullptr);
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

void Magnivo::zoomByWheelDelta(int wheelDelta) {
    // Smooth proportional step so a pinch/scroll in followed by the same
    // amount out lands back where it started. One classic notch (120)
    // is ~15%: 1.00 -> 1.15 -> 1.32 ... up to 8x. High-res touchpad
    // deltas (e.g. 10-30) give tiny smooth steps instead of full jumps.
    if (wheelDelta == 0) return;
    double steps = double(wheelDelta) / 120.0;
    double factor = std::pow(1.15, steps);
    if (!(factor > 0.0) || !(factor < 100.0)) return; // paranoia for bad input
    float target = float(double(m_zoom) * factor);
    // Wheel-out all the way to 100% means OFF (screen back to normal).
    // Wheel-in from OFF auto-arms via setZoom().
    if (target <= 1.001f) {
        if (!m_armed && m_zoom <= 1.001f) return; // already OFF, nothing to do
        setZoom(1.0f);
        setArmed(false);
        return;
    }
    setZoom(target);
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
        int theme = dlg.selectedTheme();
        Theme::save(theme);
        m_panel->applyTheme(theme);
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
    // Watchdog: Windows silently drops low-level hooks if our thread ever
    // blocks past LowLevelHooksTimeout. If a handle went null, reinstall so
    // <Mod>+wheel never mysteriously stops working until restart.
    ++m_tickCount;
    if ((m_tickCount % 300) == 0) // ~every 5s at 60fps
        ensureHooksInstalled();
}

void Magnivo::scheduleAutoUpdateCheck() {
    if (m_updateNam == nullptr)
        m_updateNam = new QNetworkAccessManager(this);
    if (m_updateReply) { m_updateReply->abort(); m_updateReply->deleteLater(); m_updateReply = nullptr; }
    QUrl url(QString("https://api.github.com/repos/%1/releases/latest")
                 .arg(QString::fromLatin1(kMagnivoUpdateRepo)));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Magnivo-Updater");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_updateReply = m_updateNam->get(req);
    connect(m_updateReply, &QNetworkReply::finished, this, &Magnivo::onAutoUpdateCheckFinished);
}

void Magnivo::onAutoUpdateCheckFinished() {
    QNetworkReply *r = m_updateReply;
    m_updateReply = nullptr;
    if (!r) return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) return; // silent when offline
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r->readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return;
    QString tag = doc.object().value("tag_name").toString().trimmed();
    if (tag.isEmpty()) return;
    QString clean = tag.startsWith('v') || tag.startsWith('V') ? tag.mid(1) : tag;
    QVersionNumber cur = QVersionNumber::fromString(QString::fromLatin1(kMagnivoVersion));
    QVersionNumber lat = QVersionNumber::fromString(clean);
    if (cur.isNull() || lat.isNull() || lat <= cur) return; // up to date: stay silent
#ifdef Q_OS_WIN
    if (!g_dismissedTag.isEmpty() && g_dismissedTag == tag) return; // user said Cancel
    if (!g_notifiedTag.isEmpty() && g_notifiedTag == tag) return; // already asked
    g_notifiedTag = tag;
#endif
    QString body = doc.object().value("body").toString().trimmed();
    if (body.length() > 240) body = body.left(240) + "...";
    QMessageBox box(m_panel);
    box.setWindowTitle("Magnivo update available");
    box.setStyleSheet(SettingsDialog::messageBoxStyle(Theme::load()));
    box.setText(QString("You have an update available.\n\nInstalled: v%1   •   Latest: %2%3\n\nUpdate now?")
                    .arg(QString::fromLatin1(kMagnivoVersion), tag,
                         body.isEmpty() ? QString() : "\n\n" + body));
    box.setInformativeText("Choose Update to download it, or Cancel to stay on this version.");
    QPushButton *updateBtn = box.addButton("Update", QMessageBox::AcceptRole);
    QPushButton *cancelBtn = box.addButton("Cancel", QMessageBox::RejectRole);
    updateBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    box.setDefaultButton(updateBtn);
    box.exec();
    if (box.clickedButton() == updateBtn) {
        openSettingsTab(SettingsDialog::UpdateTab); // auto-downloads (see g_notifiedTag)
    } else {
#ifdef Q_OS_WIN
        g_dismissedTag = tag;
        g_notifiedTag.clear();
#endif
    }
}

void Magnivo::applyTransform() {
#ifdef Q_OS_WIN
    if (!m_magOk) return;
    auto resetView = []() {
        magSetFullscreenTransform(1.0f, 0, 0);
        // Touch/pen taps are mapped into the magnified view while zoomed;
        // switch that mapping off again so taps land 1:1 when normal.
        if (pMagSetInputTransform) pMagSetInputTransform(FALSE, nullptr, nullptr);
    };
    if (!m_armed) {
        // Only reset once - spamming MagSetFullscreenTransform(1.0) at 60fps
        // is wasteful. m_transformActive tracks whether a zoom is live.
        if (m_transformActive) {
            resetView();
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
            resetView();
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
    if (magSetFullscreenTransform(mag, xOff, yOff)) {
        m_transformActive = true;
        // Map touch/pen input into the zoomed view: without this a tap on the
        // VISIBLE (magnified) X/minimize/file lands at the raw unmagnified
        // point instead, so taps miss while zoomed (mouse is unaffected, it
        // only needs this for pen/touch). Near the screen center the error is
        // tiny, which is why taps sometimes seemed to work and sometimes not.
        if (pMagSetInputTransform) {
            RECT src{ xOff, yOff, xOff + viewW, yOff + viewH };
            RECT dst{ 0, 0, sw, sh };
            pMagSetInputTransform(TRUE, &src, &dst);
        }
    }
#endif
}

void Magnivo::installGlobalHooks() {
#ifdef Q_OS_WIN
    g_inst = this;
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_kbHook)
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    qDebug() << "Magnivo: hooks installed. mouse =" << (void*)g_mouseHook
             << "kb =" << (void*)g_kbHook << "err =" << (int)GetLastError();
#endif
}

void Magnivo::ensureHooksInstalled() {
#ifdef Q_OS_WIN
    // Only reinstall handles that actually went null. (Windows can silently
    // drop a low-level hook after a timeout; reinstalling restores zoom.)
    if (g_inst != this) g_inst = this;
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_kbHook)
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
