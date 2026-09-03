// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  BigScreen front-end entry point.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// M2 milestone: reuse the real PrismLauncher core in-process. Application is
// constructed with --no-window so MainWindow never appears; the instance
// list, launching, and the running instance's console are read straight off
// APPLICATION->instances() / BaseInstance::getLaunchTask() every frame
// (immediate-mode UI — no signal subscriptions needed just to redraw).
// Accounts/settings/mod browsing are still M3+.

#include <SDL.h>
#include <SDL_opengl.h>

#include <QTimer>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include "GS/Renderers/Common/GSTexture.h"
#include "ImGui/ImGuiFullscreen.h"
#include "IconsPromptFont.h"
#include "Input/InputManager.h"
#include "gui/GuiManager.h"

#include "Application.h"
#include "BuildConfig.h"
#include "DesktopServices.h"
#include "InstanceList.h"
#include "QObjectPtr.h"
#include "common/EmuFolders.h"
#include "core/BigScreenLaunchController.h"
#include "core/DialogHelpers.h"
#include "icons/IconList.h"
#include "launch/LaunchTask.h"
#include "launch/LogModel.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "settings/SettingsObject.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <qrencode.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <unordered_map>

using namespace ImGuiFullscreen;

namespace {

// A plain tr() call in this file would use "main" as its context (this
// translation unit has no enclosing QObject class), which has no
// translations of its own. Wherever a BigScreen string is copied verbatim
// (or near-verbatim — see individual call sites) from an existing desktop
// widget, this looks it up under THAT widget's own translation context
// instead, so BigScreen picks up the same translation the desktop UI
// already has for that wording, rather than always falling back to
// English. `context` is the class named in the owning .ui file's <class>
// tag (not necessarily the same as the page's .cpp filename — e.g. Java
// settings live in JavaSettingsWidget.ui, not JavaPage.ui) or the
// enclosing C++ class for a plain tr() call. Qt's own '&' mnemonic markers
// (e.g. "&Launch") are part of the exact source string translations are
// keyed on, so they're passed through here and stripped separately (see
// StripMnemonic) — BigScreen has no keyboard-mnemonic underlining to
// preserve them for.
QString TR(const char* context, const char* sourceText)
{
    return QCoreApplication::translate(context, sourceText);
}

QString MW(const char* sourceText)
{
    return TR("MainWindow", sourceText);
}

// Removes a Qt '&' mnemonic marker for display — none of the strings this
// is used on contain a literal "&&", so a plain removal is sufficient.
QString StripMnemonic(QString text)
{
    return text.remove(QChar('&'));
}

// Cosmetic-only, applied *after* TR()/MW() resolve: several desktop labels
// this reuses are trailing-colon field labels (e.g. "Username:") from a
// form layout BigScreen doesn't have (title + separate summary line
// instead) — stripping the colon here doesn't touch the source text TR()
// looks up, so it doesn't affect whether the lookup succeeds.
QString TrimTrailingColon(QString text)
{
    if (text.endsWith(QChar(':')))
        text.chop(1);
    return text;
}

enum class Screen { Landing, Instances, Console, Accounts, AccountLogin, Settings, InstanceSettings, Quit };

Screen g_screen = Screen::Landing;
bool g_wantsQuit = false;

// A single deferred action, run once at the very top of the next frame (see
// RunPendingAction(), called from the frame lambda before SDL_PollEvent —
// i.e. before ImGui::NewFrame(), not after it). Exists because
// BigScreenDialogs::Choose/InputString block by pumping full frames
// (ImGui::NewFrame()...Render()) in a loop until the dialog resolves — safe
// to call from outside any frame, but calling them directly from a Settings
// draw function (DrawChoiceSetting/DrawTextSetting/DrawMemorySetting) would
// mean blocking *from inside* the very frame that's already mid-NewFrame(),
// recursively re-entering the frame lambda before its own Render() ran and
// corrupting ImGui's state — exactly the hang the nested-QEventLoop version
// of these dialogs had (see DialogHelpers.h). So those draw functions don't
// call the dialog helpers directly; they stash a closure here instead, and
// it actually runs one frame later, from a call site with no open
// NewFrame()/Render() pair on the stack.
std::function<void()> g_pendingAction;

// Switching g_screen alone isn't enough: ImGuiFullscreen only grabs gamepad/
// keyboard nav focus for the first focusable widget when a focus reset is
// queued (Initialize() queues exactly one, for the very first screen —
// nothing queues another on every later transition, which without this
// leaves nav with no anchor to move from after the first screen change, so
// D-pad/stick input looks like it stopped doing anything).
void SetScreen(Screen screen)
{
    g_screen = screen;
    QueueResetFocus(FocusResetType::WindowChanged);
}

// Thin wrapper over ImGuiFullscreen::SetFullscreenFooterText's icon+label
// span overload — every call site below builds its hint list as a brace-init
// list of {icon, label} pairs (icons from ImGuiFullscreen::GetGamepadGlyphs(),
// which auto-selects Xbox/PlayStation/Nintendo/generic glyphs for whatever
// controller DetectGamepadLayout() found at startup — see main()) rather
// than the plain-text "A: Foo    B: Bar" strings used before real
// controller-button glyphs were available (IconsFontAwesome.h/
// IconsPromptFont.h).
void SetFooterHints(std::initializer_list<std::pair<const char*, std::string_view>> items)
{
    SetFullscreenFooterText(std::span(items.begin(), items.size()));
}

constexpr float kTopBarHeight = 60.0f;

// A single top-bar tab icon (see DrawTopBar's tabs parameter) — no text
// label, matching the reference (PCSX2's own settings screen shows category
// tabs as bare icons in the title bar, not a separate labeled row).
struct TopBarTab {
    const char* icon;
    bool active;
};

// A persistent status bar (app icon + screen title on the left) drawn above
// every screen's content — the visual reference (PCSX2's own BigScreen
// mode) has exactly this, but it's part of their FullscreenUI.cpp, which
// isn't vendored (see the BigScreen plan, M1) — this is BigScreen's own
// equivalent, using the foreground draw list directly rather than a
// window, so it stays fixed while screen content (each its own ImGui
// window) scrolls/changes underneath it.
//
// On the right: a live clock normally, or — on screens with categories
// (e.g. Settings) — a row of icon tabs instead, again matching the
// reference, which puts its settings-category icons in the title bar
// itself rather than a separate tab strip. Purely decorative here; the
// actual LB/RB switching logic lives in the screen that passes `tabs`.
void DrawTopBar(const char* title, std::span<const TopBarTab> tabs = {})
{
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float barHeight = LayoutScale(kTopBarHeight);
    const float padding = LayoutScale(20.0f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(displaySize.x, barHeight), ImGui::GetColorU32(UIBackgroundColor));
    dl->AddLine(ImVec2(0.0f, barHeight), ImVec2(displaySize.x, barHeight), ImGui::GetColorU32(UIBackgroundLineColor),
                LayoutScale(1.0f));

    const float iconSize = barHeight * 0.6f;
    const ImVec2 iconPos(padding, (barHeight - iconSize) * 0.5f);
    if (GSTexture* appIcon = GetCachedTexture("images/icons/app.png"))
        dl->AddImage(static_cast<ImTextureID>(appIcon->GetNativeHandle()), iconPos, iconPos + ImVec2(iconSize, iconSize));

    const ImVec2 titlePos(iconPos.x + iconSize + LayoutScale(12.0f), (barHeight - g_large_font.second) * 0.5f);
    dl->AddText(g_large_font.first, g_large_font.second, titlePos, ImGui::GetColorU32(UIBackgroundTextColor), title);

    if (tabs.empty()) {
        const QString timeStr = QTime::currentTime().toString("HH:mm:ss");
        const QByteArray timeUtf8 = timeStr.toUtf8();
        const ImVec2 timeSize = g_large_font.first->CalcTextSizeA(g_large_font.second, FLT_MAX, 0.0f, timeUtf8.constData());
        const ImVec2 timePos(displaySize.x - timeSize.x - padding, (barHeight - g_large_font.second) * 0.5f);
        dl->AddText(g_large_font.first, g_large_font.second, timePos, ImGui::GetColorU32(UIBackgroundTextColor), timeUtf8.constData());
        return;
    }

    const float tabIconSize = barHeight * 0.5f;
    const float tabBoxSize = barHeight * 0.8f;
    const float tabGap = LayoutScale(6.0f);
    const float rowWidth = static_cast<float>(tabs.size()) * tabBoxSize + static_cast<float>(tabs.size() - 1) * tabGap;
    float x = displaySize.x - padding - rowWidth;
    const float boxY = (barHeight - tabBoxSize) * 0.5f;

    for (const TopBarTab& tab : tabs) {
        if (tab.active) {
            dl->AddRectFilled(ImVec2(x, boxY), ImVec2(x + tabBoxSize, boxY + tabBoxSize), ImGui::GetColorU32(UIPrimaryColor),
                               LayoutScale(LAYOUT_FRAME_ROUNDING));
        }
        if (GSTexture* icon = GetCachedTexture(tab.icon)) {
            const ImVec2 pos(x + (tabBoxSize - tabIconSize) * 0.5f, (barHeight - tabIconSize) * 0.5f);
            dl->AddImage(static_cast<ImTextureID>(icon->GetNativeHandle()), pos, pos + ImVec2(tabIconSize, tabIconSize));
        }
        x += tabBoxSize + tabGap;
    }
}

// Every screen calls this instead of BeginFullscreenColumns() directly:
// draws the top bar, then opens the columns window positioned below it
// (title passed as nullptr so ImGui doesn't also draw its own native
// titlebar — DrawTopBar() already shows the title).
bool BeginScreen(const char* title, bool footer = true, std::span<const TopBarTab> topTabs = {})
{
    DrawTopBar(title, topTabs);
    return BeginFullscreenColumns(nullptr, LayoutScale(kTopBarHeight), true, footer);
}

// A single row of HorizontalMenuItem-style cards that may not all fit the
// real available width (a fixed 4-up row overflowing at high Scale/on a
// narrow window, or an instance list wider than 4 items) — draws whichever
// fit, centered, in a horizontally-scrolling child, plus a left/right
// chevron in the margin on whichever side has more items currently
// scrolled out of view.
//
// Doesn't track a scroll or selection index itself: Dear ImGui's own nav
// system already scrolls a child window to keep the moved-to item in view
// (the same mechanism every vertical list in this file already relies on,
// just working on the X axis here) — drawItems() just draws every item
// normally, same as before this existed, and this only wraps that in a
// sized/scrollable child plus reads back its resulting scroll position
// for the indicators. id must be unique per call site (becomes the child
// window's ImGui ID); rowWidth is the *content* width drawItems() will
// produce (e.g. itemCount * LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH, in logical
// units) — needed up front to decide whether centering or edge-alignment
// applies, and whether the chevrons should show at all.
void DrawScrollableCardRow(const char* id, float rowWidthLogical, float rowHeight, const std::function<void()>& drawItems)
{
    const float chevronAreaWidth = LayoutScale(36.0f);
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float rowWidth = LayoutScale(rowWidthLogical);
    const bool canOverflow = rowWidth > availableWidth - chevronAreaWidth * 2.0f;
    const float scrollAreaWidth = canOverflow ? (availableWidth - chevronAreaWidth * 2.0f) : availableWidth;

    bool showLeft = false;
    bool showRight = false;

    if (canOverflow)
        ImGui::Dummy(ImVec2(chevronAreaWidth, rowHeight));

    if (canOverflow)
        ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild(id, ImVec2(scrollAreaWidth, rowHeight), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    // Center when everything fits (matches the pre-scrolling behavior
    // exactly); flush to the start edge when it doesn't, so there's
    // nothing but the chevron to imply more content in that direction.
    ImGui::SetCursorPosX(std::max(0.0f, (scrollAreaWidth - rowWidth) * 0.5f));
    drawItems();
    if (canOverflow) {
        showLeft = ImGui::GetScrollX() > 0.5f;
        showRight = ImGui::GetScrollX() < ImGui::GetScrollMaxX() - 0.5f;
    }
    ImGui::EndChild();

    if (canOverflow) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(chevronAreaWidth, rowHeight));
    }

    if (showLeft || showRight) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const float rowTop = windowPos.y + ImGui::GetCursorPosY() - rowHeight - ImGui::GetScrollY();
        const float centerY = rowTop + rowHeight * 0.5f;
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        if (showLeft)
            dl->AddText(g_large_font.first, g_large_font.second, ImVec2(windowPos.x + LayoutScale(6.0f), centerY - g_large_font.second * 0.5f),
                        color, ICON_FA_CHEVRON_LEFT);
        if (showRight)
            dl->AddText(g_large_font.first, g_large_font.second,
                        ImVec2(windowPos.x + availableWidth - chevronAreaWidth + LayoutScale(6.0f), centerY - g_large_font.second * 0.5f),
                        color, ICON_FA_CHEVRON_RIGHT);
    }
}

// Instance icons come from Qt's IconList (built-in resource icons and
// user-added image files alike) as QIcon/QImage — outside ImGuiFullscreen's
// own file-path texture cache, so BigScreen keeps its own, keyed by the
// instance's icon key.
GSTexture* GetInstanceIconTexture(MinecraftInstance* inst)
{
    static std::unordered_map<QString, std::shared_ptr<GSTexture>> cache;

    const QString key = inst->iconKey();
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.get();

    const QImage image = APPLICATION->icons()->getIcon(key).pixmap(150, 150).toImage();
    auto texture = BigScreenGui::UploadQImage(image);
    GSTexture* raw = texture.get();
    cache.emplace(key, std::move(texture));
    return raw;
}
MinecraftInstance* g_consoleInstance = nullptr;
MinecraftInstance* g_instanceSettingsTarget = nullptr;

// Microsoft device-code login state (Accounts/AccountLogin screens). No
// keyboard needed on this device at all: the user opens the shown URL (or
// scans the QR, which encodes the same URL with the code pre-filled) on
// *any* other device and enters the code there — see
// MinecraftAccount::login(true) / AuthFlow::Action::DeviceCode, which
// launcher/ui/dialogs/MSALoginDialog.cpp already drives for the desktop UI;
// this reuses the exact same task class, just renders it differently.
MinecraftAccountPtr g_loginAccount;
shared_qobject_ptr<AuthFlow> g_loginTask;
QString g_loginUrl;
QString g_loginCode;
QString g_loginStatus;
QString g_loginError;
std::shared_ptr<GSTexture> g_loginQrTexture;

void ResetLogin()
{
    if (g_loginTask)
        g_loginTask->disconnect();
    g_loginTask.reset();
    g_loginAccount.reset();
    g_loginUrl.clear();
    g_loginCode.clear();
    g_loginStatus.clear();
    g_loginError.clear();
    g_loginQrTexture.reset();
}

void StartLogin()
{
    ResetLogin();
    g_loginAccount = MinecraftAccount::createBlankMSA();
    g_loginTask = g_loginAccount->login(true);
    g_loginStatus = QObject::tr("Starting device code login...");

    QObject::connect(g_loginTask.get(), &Task::status, APPLICATION, [](QString status) { g_loginStatus = status; });

    QObject::connect(g_loginTask.get(), &AuthFlow::authorizeWithBrowserWithExtra, APPLICATION,
                      [](QString url, QString code, int) {
                          g_loginUrl = url;
                          g_loginCode = code;

                          // Same QR rendering approach as MSALoginDialog::paintQR
                          // (launcher/ui/dialogs/MSALoginDialog.cpp) — reuses the
                          // same qrencode dependency, just uploaded to a GSTexture
                          // instead of set on a QLabel.
                          QUrl linkUrl(url);
                          QUrlQuery query;
                          if (url == "https://www.microsoft.com/link" && !code.isEmpty())
                              query.addQueryItem("otc", code);
                          linkUrl.setQuery(query);

                          const auto* qr = QRcode_encodeString(linkUrl.toString().toUtf8().constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
                          if (!qr)
                              return;

                          const int canvas = 300;
                          QPixmap pixmap(canvas, canvas);
                          pixmap.fill(Qt::white);
                          QPainter painter(&pixmap);
                          painter.setPen(Qt::NoPen);
                          painter.setBrush(Qt::black);
                          const double scale = 0.9 * std::min(canvas / static_cast<double>(qr->width), canvas / static_cast<double>(qr->width));
                          const double offset = (canvas - qr->width * scale) / 2.0;
                          for (int y = 0; y < qr->width; ++y) {
                              for (int x = 0; x < qr->width; ++x) {
                                  if (qr->data[y * qr->width + x] & 1)
                                      painter.drawRect(QRectF(offset + x * scale, offset + y * scale, scale, scale));
                              }
                          }
                          painter.end();
                          g_loginQrTexture = BigScreenGui::UploadQImage(pixmap.toImage());
                      });

    QObject::connect(g_loginTask.get(), &Task::succeeded, APPLICATION, [] {
        APPLICATION->accounts()->addAccount(g_loginAccount);
        ResetLogin();
        SetScreen(Screen::Accounts);
    });

    QObject::connect(g_loginTask.get(), &Task::failed, APPLICATION, [](QString reason) { g_loginError = reason; });
    QObject::connect(g_loginTask.get(), &Task::aborted, APPLICATION, [] { g_loginError = QObject::tr("Aborted"); });

    QMetaObject::invokeMethod(g_loginTask.get(), &Task::start, Qt::QueuedConnection);
}

// Locates the normal desktop Qt Widgets build (BuildConfig.LAUNCHER_APP_BINARY_NAME,
// "prismlauncher" — the same name Application.cpp already uses for e.g. its
// updater filename and URL scheme, so this isn't a new assumption). Two
// candidate locations, tried in order:
//   1. A sibling executable next to BigScreen's own binary — covers a dev
//      build (both land in the same build/<config>/ directory) and any
//      packaging that ships both binaries side by side in one prefix.
//   2. QStandardPaths::findExecutable(), i.e. a PATH search — covers the
//      much more common real-world case of BigScreen having been built/
//      packaged/run completely independently of a system-package desktop
//      install (e.g. `/usr/bin/prismlauncher` from a distro package, while
//      BigScreen itself runs from an extracted CI tarball with no relation
//      to that install location at all — confirmed a real gap, not
//      hypothetical, since BigScreen's own portable packaging keeps it
//      self-contained specifically so it *doesn't* need to sit next to
//      anything).
// Returns an empty string if neither finds anything runnable.
QString FindDesktopLauncherBinary()
{
    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(BuildConfig.LAUNCHER_APP_BINARY_NAME);
    if (QFileInfo(sibling).isExecutable())
        return sibling;

    const QString onPath = QStandardPaths::findExecutable(BuildConfig.LAUNCHER_APP_BINARY_NAME);
    if (!onPath.isEmpty())
        return onPath;

    return {};
}

// Launches the desktop build located by FindDesktopLauncherBinary() as a
// fresh, detached process, then quits BigScreen. QProcess::startDetached
// (not a QProcess this owns) so the new process keeps running after this
// one exits; Application's own LocalPeer single-instance check (confirmed
// working earlier this session) means the two don't collide even during
// the brief window both are alive.
void SwitchToDesktopMode()
{
    const QString desktopBinary = FindDesktopLauncherBinary();
    if (desktopBinary.isEmpty()) {
        SDL_Log("[switch-to-desktop] could not find '%s' next to this binary or on PATH",
                qUtf8Printable(BuildConfig.LAUNCHER_APP_BINARY_NAME));
        return;
    }
    if (!QProcess::startDetached(desktopBinary, {})) {
        SDL_Log("[switch-to-desktop] failed to start '%s'", qUtf8Printable(desktopBinary));
        return;
    }
    g_wantsQuit = true;
}

// B (gamepad) / Escape (keyboard, on release) — ImGuiFullscreen's own
// WantsToCloseMenu()/ResetCloseMenuIfNeeded() pair, the same debounced
// press-detector its own popups (choice dialog, file selector) use to
// close themselves. Guarded against those popups being open: they already
// consume the same button internally (see DrawChoiceDialog/DrawFileSelector
// in the vendored ImGuiFullscreen.cpp), so acting here too in the same
// frame would both close the popup *and* pop our own screen stack.
void HandleBackButton()
{
    if (IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen())
        return;
    if (!WantsToCloseMenu())
        return;

    switch (g_screen) {
        case Screen::Landing:
            // Nowhere further back to go — a dedicated screen (matching
            // PCSX2's own reference layout — icon cards, not a popup list)
            // offers quitting or switching to desktop mode instead. A
            // *screen*, not a dialog: this used to be an OpenChoiceDialog,
            // but B/Escape on the Landing screen was also still hitting a
            // leftover raw `if (SDLK_ESCAPE) done = true;` check in the SDL
            // event loop from before any confirmation existed at all,
            // instantly quitting and skipping the dialog entirely — that's
            // now removed, but a real screen (going through the exact same
            // SetScreen()/HandleBackButton() machinery every other screen
            // does) doesn't leave room for that class of bug to resurface.
            SetScreen(Screen::Quit);
            break;
        case Screen::Instances:
            SetScreen(Screen::Landing);
            break;
        case Screen::Console:
            SetScreen(Screen::Instances);
            break;
        case Screen::Accounts:
            SetScreen(Screen::Landing);
            break;
        case Screen::AccountLogin:
            if (g_loginTask)
                g_loginTask->abort();
            ResetLogin();
            SetScreen(Screen::Accounts);
            break;
        case Screen::Settings:
            SetScreen(Screen::Landing);
            break;
        case Screen::InstanceSettings:
            SetScreen(Screen::Instances);
            break;
        case Screen::Quit:
            SetScreen(Screen::Landing);
            break;
    }

    ResetCloseMenuIfNeeded();
}

// Drawn instead of the normal per-screen switch whenever
// BigScreenDialogs::BlockingDepth is set (see its comment) — a task is
// blocking-waiting via a frame-pumping loop (a Settings dialog, or a real
// instance launch), and the actual screen underneath must NOT keep
// processing input during that: pumping frames doesn't stop SDL_PollEvent
// from delivering a stray gamepad press, and if e.g. the Instances screen
// stayed interactive, that press could fire a second, unrelated
// Application::launch() call nested inside the first one's own wait loop.
// This has no inputs of its own, so nothing can navigate or confirm here.
void DrawBlockingWait()
{
    if (BeginScreen("Please Wait", false)) {
        // Every BeginFullscreenColumnWindow(0.0f, 0.0f, ...) call in this
        // file (not just this one) relies on the vendored function's own
        // documented behavior for its "end" parameter: <= 0 means
        // "DisplaySize.x + end", so 0.0f resolves to the real window's
        // actual right edge — not the fixed 1280 LAYOUT_SCREEN_WIDTH
        // reference these all used before. That fixed width was the actual
        // cause of "Scale breaks the layout"/no-16:9-support: at anything
        // above 1.0x, or on a wider-than-16:9 real window, a column sized
        // to LayoutScale(1280) either overflowed the real (narrower)
        // window — clipped by the parent's own bounds — or sat short of
        // the real (wider) one, leaving unused space. BeginScreen() itself
        // already passes expand_to_screen_width=true to
        // BeginFullscreenColumns() for the *outer* window (dynamic,
        // correct already) — this makes the *inner* content column match
        // it, instead of quietly reintroducing the same fixed-width
        // assumption one level down.
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "blocking_wait")) {
            const char* text = "Please wait...";
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - textSize.x) * 0.5f, (ImGui::GetWindowHeight() - textSize.y) * 0.5f));
            ImGui::TextUnformatted(text);
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

struct LandingItem {
    const char* icon;
    QString title;
    const char* description;
};

// PCSX2's own BigScreen home screen (a row of big icon cards with a title
// and description under each) is the explicit visual reference — this
// mirrors it with ImGuiFullscreen's own HorizontalMenuItem widget, the same
// one PCSX2 uses there.
void DrawLanding(bool& done)
{
    // "Instances"/"Accounts"/"Settings..."/"Quit" all reuse real
    // MainWindow.ui strings now: actionViewInstanceFolder's text ("Open the
    // instances folder...") happens to be the literal word "&Instances" —
    // not semantically about *this* card, but the translated word itself
    // is exactly what a translator would use for an "Instances" menu label
    // too, and it's the only "Instances" string that exists anywhere in
    // the desktop UI to reuse. accountsMenu's title ("&Accounts") and
    // actionSettings's text ("Setti&ngs..."). "Quit" reuses
    // actionCloseWindow's text ("Close &Window") — the desktop has no
    // dedicated "Quit"/"Exit" action (closing its one window *is* quitting
    // there), so this is the closest real equivalent, same word BigScreen's
    // own DrawQuit() screen's own "Quit" card uses.
    // Rebuilt fresh every frame (cheap) rather than a `static` array, since
    // TR()/MW() need to run after the translator is installed and could
    // change if the language setting changes mid-session.
    const LandingItem items[] = {
        { "images/icons/instances.png", StripMnemonic(MW("&Instances")), "Browse and launch your installed Minecraft instances." },
        { "images/icons/accounts.png", StripMnemonic(MW("&Accounts")), "Manage your logged-in Microsoft and offline accounts." },
        { "images/icons/settings.png", StripMnemonic(MW("Setti&ngs...")), "Change launcher and instance settings." },
        { "images/icons/quit.png", StripMnemonic(MW("Close &Window")), "Exit BigScreen and return to the desktop." },
    };

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Select" }, { glyphs.cancel(false), "Quit" } });
    }

    if (BeginScreen("PrismLauncher BigScreen")) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "landing")) {
            BeginNavBar();
            // See DrawSettings' settings_content comment: BeginNavBar(), like
            // BeginMenuButtons(), never consumes a queued focus reset on its
            // own — every built-in ImGuiFullscreen dialog does this
            // explicitly, so every BigScreen screen needs to as well.
            ResetFocusHere();

            // HorizontalMenuItem cards are a fixed LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH
            // each and flow left-to-right via ImGui::SameLine() with no
            // built-in centering — the reference layout (PCSX2's own home
            // screen) has the row centered as a group, not flush against
            // the left edge, so center it here (vertically directly;
            // DrawScrollableCardRow below handles horizontal centering,
            // and — if the row doesn't fit, e.g. at a high Scale setting
            // or on a narrow window — falling back to a scrollable row
            // with "more this way" chevrons instead of running off-screen).
            const float rowWidthLogical = static_cast<float>(std::size(items)) * LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH;
            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float rowHeight = LayoutScale(LAYOUT_HORIZONTAL_MENU_HEIGHT);
            ImGui::SetCursorPosY(std::max(0.0f, (availableHeight - rowHeight) * 0.5f));

            // Dispatched by index, not by matching the (now sometimes
            // translated, no longer English-guaranteed) title text.
            DrawScrollableCardRow("landing_row", rowWidthLogical, rowHeight, [&items]() {
                for (int i = 0; i < static_cast<int>(std::size(items)); ++i) {
                    const LandingItem& item = items[i];
                    GSTexture* icon = GetCachedTexture(item.icon);
                    const QByteArray titleUtf8 = item.title.toUtf8();
                    if (HorizontalMenuItem(icon, titleUtf8.constData(), item.description)) {
                        switch (i) {
                            case 0:
                                SetScreen(Screen::Instances);
                                break;
                            case 1:
                                SetScreen(Screen::Accounts);
                                break;
                            case 2:
                                SetScreen(Screen::Settings);
                                break;
                            case 3:
                                // Same confirmation screen B shows — clicking
                                // this card used to quit instantly with no
                                // confirmation at all, inconsistent with B's
                                // own behavior on the very same screen.
                                SetScreen(Screen::Quit);
                                break;
                        }
                    }
                }
            });
            EndNavBar();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

// B (or clicking "Quit" on Landing) opens this — matches PCSX2's own quit
// screen (icon cards: Back / Exit / switch UI mode), reusing DrawLanding's
// own row-of-HorizontalMenuItem-cards layout rather than a popup dialog, at
// the user's explicit request ("хочу что-то типа такого", with a PCSX2
// screenshot showing exactly this layout).
void DrawQuit(bool& done)
{
    // "Quit" reuses actionCloseWindow's text ("Close &Window") — see
    // DrawLanding's comment on why that's the closest real desktop
    // equivalent. "Back" and "Switch to Desktop Mode" have no desktop
    // equivalent (BigScreen-only concepts) and stay BigScreen's own text.
    const LandingItem items[] = {
        { "images/icons/back.png", "Back", "Return to the previous menu." },
        { "images/icons/quit.png", StripMnemonic(MW("Close &Window")), "Fully exit BigScreen, returning to your desktop." },
        { "images/icons/desktop.png", "Switch to Desktop Mode", "Exit BigScreen mode, switch to the desktop interface." },
    };

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Select" }, { glyphs.cancel(false), "Back" } });
    }

    if (BeginScreen("PrismLauncher BigScreen")) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "quit")) {
            BeginNavBar();
            ResetFocusHere();

            // See DrawLanding()'s identical row-centering for why this uses
            // GetContentRegionAvail() rather than LAYOUT_SCREEN_WIDTH.
            const float rowWidth = LayoutScale(static_cast<float>(std::size(items)) * LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH);
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float rowHeight = LayoutScale(LAYOUT_HORIZONTAL_MENU_HEIGHT);
            ImGui::SetCursorPos(
                ImVec2(std::max(0.0f, (availableWidth - rowWidth) * 0.5f), std::max(0.0f, (availableHeight - rowHeight) * 0.5f)));

            for (int i = 0; i < static_cast<int>(std::size(items)); ++i) {
                const LandingItem& item = items[i];
                GSTexture* icon = GetCachedTexture(item.icon);
                const QByteArray titleUtf8 = item.title.toUtf8();
                if (HorizontalMenuItem(icon, titleUtf8.constData(), item.description)) {
                    switch (i) {
                        case 0:
                            SetScreen(Screen::Landing);
                            break;
                        case 1:
                            done = true;
                            break;
                        case 2:
                            SwitchToDesktopMode();
                            break;
                    }
                }
            }
            EndNavBar();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

void DrawAccounts()
{
    AccountList* accounts = APPLICATION->accounts();

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Set default" }, { glyphs.cancel(false), "Back" } });
    }

    if (BeginScreen("Accounts")) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "accounts")) {
            BeginMenuButtons();
            ResetFocusHere();

            if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Back"))
                SetScreen(Screen::Landing);

            if (MenuButtonWithoutSummary("+ Add Microsoft Account")) {
                StartLogin();
                SetScreen(Screen::AccountLogin);
            }

            const MinecraftAccountPtr defaultAccount = accounts->defaultAccount();
            const int count = accounts->count();
            for (int i = 0; i < count; ++i) {
                const MinecraftAccountPtr account = accounts->at(i);
                if (!account)
                    continue;

                const QByteArray nameUtf8 = account->profileName().toUtf8();
                QString summaryStr = account->accountType() == AccountType::Offline ? QObject::tr("Offline account") : QObject::tr("Microsoft account");
                if (account == defaultAccount)
                    summaryStr += QObject::tr("  •  Default");
                const QByteArray summaryUtf8 = summaryStr.toUtf8();

                if (MenuButton(nameUtf8.constData(), summaryUtf8.constData()))
                    accounts->setDefaultAccount(account);
            }

            if (count == 0)
                ImGui::TextUnformatted("No accounts yet — add one above.");

            EndMenuButtons();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

// Microsoft device-code login: no on-screen keyboard needed anywhere in
// BigScreen for this — the code is entered on whatever device the user
// scans the QR (or opens the URL) with.
void DrawAccountLogin()
{
    SetFooterHints({ { GetGamepadGlyphs().cancel(false), "Cancel" } });

    if (BeginScreen("Sign in with Microsoft")) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "account_login")) {
            BeginMenuButtons();
            ResetFocusHere();
            if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Cancel")) {
                if (g_loginTask)
                    g_loginTask->abort();
                ResetLogin();
                SetScreen(Screen::Accounts);
            }
            EndMenuButtons();

            ImGui::Dummy(LayoutScale(0.0f, 20.0f));

            if (!g_loginError.isEmpty()) {
                const QByteArray errUtf8 = g_loginError.toUtf8();
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", errUtf8.constData());
            } else if (!g_loginQrTexture) {
                const QByteArray statusUtf8 = g_loginStatus.toUtf8();
                ImGui::TextUnformatted(statusUtf8.constData());
            } else {
                const QByteArray codeUtf8 = g_loginCode.toUtf8();
                const QByteArray urlUtf8 = g_loginUrl.toUtf8();

                ImGui::TextUnformatted("Scan this QR code with your phone,");
                ImGui::TextUnformatted("or open the URL below and enter the code:");
                ImGui::Dummy(LayoutScale(0.0f, 10.0f));

                const float qrSize = LayoutScale(300.0f);
                ImGui::Image(static_cast<ImTextureID>(g_loginQrTexture->GetNativeHandle()), ImVec2(qrSize, qrSize));

                ImGui::Dummy(LayoutScale(0.0f, 10.0f));
                ImGui::Text("URL:  %s", urlUtf8.constData());
                ImGui::Text("Code: %s", codeUtf8.constData());
            }
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

// A toggle bound directly to a SettingsObject key: reads fresh every frame
// (immediate-mode, so no cached/stale state to worry about) and writes back
// only when the widget reports a change. invert flips both directions, for
// keys that are phrased negatively (e.g. "...Disabled") but read more
// naturally as a positive toggle in a menu ("Enable ...").
//
// settings defaults to the global SettingsObject (every existing call site
// predates this parameter and still means that) — pass instance->settings()
// instead to draw/edit a per-instance override (DrawInstanceSettings*
// below). Per-instance settings are the *same* SettingsObject API with a
// transparent inheritance layer underneath (MinecraftInstance::
// loadSpecificSettings()'s registerOverride() calls) — get()/set() here
// don't need to know or care which one they're pointed at.
void DrawToggleSetting(const char* key, const QString& title, const QString& summary, bool invert = false, SettingsObject* settings = nullptr)
{
    if (!settings)
        settings = APPLICATION->settings();
    bool value = settings->get(key).toBool() != invert;
    const QByteArray titleUtf8 = title.toUtf8();
    const QByteArray summaryUtf8 = summary.toUtf8();
    if (ToggleButton(titleUtf8.constData(), summaryUtf8.constData(), &value))
        settings->set(key, value != invert);
}

// A string-valued setting picked from a fixed set of options (e.g. a theme
// name) via BigScreenDialogs::Choose, the same picker DrawMemorySetting
// uses for its MB presets. The actual blocking Choose() call is deferred to
// the top of the next frame via g_pendingAction (see its comment) — this
// function itself only ever runs mid-frame.
void DrawChoiceSetting(const char* key, const QString& title, const QString& summary, const std::vector<std::string>& options,
                        SettingsObject* settings = nullptr)
{
    if (!settings)
        settings = APPLICATION->settings();
    const QString current = settings->get(key).toString();
    const std::string valueStr = current.isEmpty() ? std::string("(default)") : current.toStdString();

    const QByteArray titleUtf8 = title.toUtf8();
    const QByteArray summaryUtf8 = summary.toUtf8();
    if (MenuButtonWithValue(titleUtf8.constData(), summaryUtf8.constData(), valueStr.c_str())) {
        g_pendingAction = [settings, key = std::string(key), title = title.toStdString(), options]() {
            const auto choice = BigScreenDialogs::Choose(title, options);
            if (choice && *choice >= 0 && *choice < static_cast<int>(options.size()))
                settings->set(QString::fromStdString(key), QString::fromStdString(options[*choice]));
        };
    }
}

// Memory sliders aren't part of the vendored widget set, and BigScreen has
// no text input — so instead of a raw MB entry field, this is a preset
// picker: current value shown on the button, tap opens a choice dialog
// (BigScreenDialogs::Choose, same deferred pattern as DrawChoiceSetting
// above) listing fixed MB steps.
void DrawMemorySetting(const char* key, const QString& title, const QString& summary, SettingsObject* settings = nullptr)
{
    static const int kPresetsMb[] = { 512, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 10240, 12288 };

    if (!settings)
        settings = APPLICATION->settings();
    const int currentMb = settings->get(key).toInt();
    const std::string valueStr = std::to_string(currentMb) + " MB";

    const QByteArray titleUtf8 = title.toUtf8();
    const QByteArray summaryUtf8 = summary.toUtf8();
    if (MenuButtonWithValue(titleUtf8.constData(), summaryUtf8.constData(), valueStr.c_str())) {
        g_pendingAction = [settings, key = std::string(key), title = title.toStdString()]() {
            std::vector<std::string> labels;
            for (const int mb : kPresetsMb)
                labels.push_back(std::to_string(mb) + " MB");

            const auto choice = BigScreenDialogs::Choose(title, labels);
            if (choice && *choice >= 0 && *choice < static_cast<int>(std::size(kPresetsMb)))
                settings->set(QString::fromStdString(key), kPresetsMb[*choice]);
        };
    }
}

// M6 turned out not to need a hand-built on-screen keyboard: Dear ImGui's
// SDL2 backend already calls SDL_StartTextInput()/SDL_StopTextInput() as
// InputText widgets gain/lose focus (imgui_impl_sdl2.cpp's
// ImGui_ImplSDL2_PlatformSetImeData, wired via io.PlatformSetImeDataFn) —
// which is exactly the documented mechanism platforms with a virtual
// keyboard (Android, and — per Valve's own SDL2 integration — Steam/
// Gamescope on Steam Deck) use to pop it up automatically. So any ordinary
// ImGui::InputText (including the one inside
// ImGuiFullscreen::OpenInputStringDialog, which BigScreenDialogs::InputString
// wraps) already gets a controller-usable on-screen keyboard for free on
// those platforms; this just needed confirming, not building. Unverified on
// real Steam Deck hardware — no way to test that from here.
void DrawTextSetting(const char* key, const QString& title, const QString& summary, bool isPassword = false, SettingsObject* settings = nullptr)
{
    if (!settings)
        settings = APPLICATION->settings();
    const QString current = settings->get(key).toString();
    // Masked the same way for the row's own value preview as for the edit
    // dialog below — showing the real password right on the settings list
    // would defeat the point of masking it in the edit field.
    const std::string valueStr =
        current.isEmpty() ? std::string("(not set)") : (isPassword ? std::string(current.size(), '*') : current.toStdString());

    // See DrawChoiceSetting above — InputString() blocks by pumping frames,
    // so it can't be called from here (mid-frame); deferred via
    // g_pendingAction to run outside any frame instead.
    const QByteArray titleUtf8 = title.toUtf8();
    const QByteArray summaryUtf8 = summary.toUtf8();
    if (MenuButtonWithValue(titleUtf8.constData(), summaryUtf8.constData(), valueStr.c_str())) {
        g_pendingAction = [settings, key = std::string(key), title = title.toStdString(), summary = summary.toStdString(),
                            currentStr = current.toStdString(), isPassword]() {
            const auto result = BigScreenDialogs::InputString(title, summary, currentStr, "OK", isPassword);
            if (result)
                settings->set(QString::fromStdString(key), *result);
        };
    }
}

// One draw function per sub-tab — each just a handful of items, unlike the
// old single scrolling wall of ~17 rows.
// Every DrawSettings* function below passes TR()/MW()-resolved desktop
// wording as title/description wherever a good match exists in the
// mapping researched against launcher/ui/pages/**/*.ui and their widgets
// (context = the class named in that .ui's <class> tag, e.g. Java settings
// live in JavaSettingsWidget.ui so their context is "JavaSettingsWidget",
// not "JavaPage" — MinecraftPage/AppearancePage are themselves just thin
// subclasses of MinecraftSettingsWidget/AppearanceWidget with no .ui of
// their own). Only one is truly byte-for-byte verbatim to begin with
// (MaxMemAlloc's description) — everywhere else, BigScreen's own English
// wording was adjusted here to *match* the desktop's exact source text,
// specifically so this lookup succeeds instead of silently falling back to
// English. Rich-text (HTML) desktop tooltips are deliberately NOT reused
// for descriptions (ImGui would render the literal "<html>..." tags as
// text) — those keep a plain-text BigScreen-authored description instead,
// noted inline. Settings with no reasonable desktop match at all
// (ProxyAddr/Port, JvmArgs's description, Language, Rename's dialog, ...)
// keep their original BigScreen-only text, which has no existing
// translation to pick up regardless of context.
void DrawSettingsGeneral()
{
    DrawToggleSetting("ModMetadataDisabled", TR("LauncherPage", "Keep track of mod metadata"),
                       TR("LauncherPage", "Store version information provided by mod providers (like Modrinth or CurseForge) for mods."),
                       true);
    DrawToggleSetting("ModDependenciesDisabled", TR("LauncherPage", "Install dependencies automatically"),
                       TR("LauncherPage", "Automatically detect, install, and update mod dependencies."), true);
    DrawToggleSetting("ShowModIncompat", TR("LauncherPage", "Detect and show mod incompatibilities (experimental)"),
                       TR("LauncherPage",
                          "Currently this just shows mods which are not marked as compatible with the current Minecraft version."));
    DrawToggleSetting(
        "DownloadsDirWatchRecursive", StripMnemonic(TR("LauncherPage", "Check &subfolders for blocked mods")),
        TR("LauncherPage", "When enabled, in addition to the downloads folder, its sub folders will also be searched when looking for "
                            "resources (e.g. when looking for blocked mods on CurseForge)."));
    DrawToggleSetting("MoveModsFromDownloadsDir", TR("LauncherPage", "Move blocked mods instead of copying them"),
                       TR("LauncherPage", "When enabled, it will move blocked resources instead of copying them."));
}

void DrawSettingsAppearance()
{
    // Affects the normal Qt Widgets desktop UI's look, not BigScreen's own
    // rendering (which always uses its own fixed dark ImGuiFullscreen theme)
    // — still a real setting worth exposing for anyone who switches back
    // and forth between the two front-ends. AppearanceWidget's own "Theme:"
    // label has no matching description text, so the summary stays
    // BigScreen's own.
    DrawChoiceSetting("ApplicationTheme", TrimTrailingColon(TR("AppearanceWidget", "Theme:")),
                       "Color theme used by the normal (non-BigScreen) launcher window.", { "system", "dark", "bright" });
    DrawToggleSetting("EnableCat", TR("AppearanceWidget", "Enable cat"),
                       "Show the launcher's cat easter egg (desktop UI only).");

    // Unlike everything above, this DOES affect BigScreen's own rendering
    // (Settings > Appearance > Scale) — a user-adjustable multiplier on top
    // of the automatic fit-to-window scale (see its application in
    // renderFrame, right after UpdateLayoutScale()). No slider widget in
    // the vendored toolkit (same reasoning as DrawMemorySetting's MB
    // presets), so this is a preset picker, same pattern.
    //
    // Was capped at 100% for a round (values above it clipped cards/the top
    // bar off-screen) — that was actually caused by every screen's content
    // column being sized to a fixed 1280-wide reference instead of the
    // real window (see BeginFullscreenColumnWindow's call sites), which
    // also broke non-16:9 windows the same way; fixed at the source, so
    // the full range is back.
    {
        static const float kScalePresets[] = { 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f };
        SettingsObject* settings = APPLICATION->settings();
        const float currentScale = settings->get("BigScreenUIScale").toFloat();
        const std::string valueStr = std::to_string(static_cast<int>(currentScale * 100.0f + 0.5f)) + "%";

        if (MenuButtonWithValue("Scale", "Adjust the size of BigScreen's own interface.", valueStr.c_str())) {
            g_pendingAction = []() {
                std::vector<std::string> labels;
                for (const float s : kScalePresets)
                    labels.push_back(std::to_string(static_cast<int>(s * 100.0f + 0.5f)) + "%");
                const auto choice = BigScreenDialogs::Choose("Scale", labels);
                if (choice && *choice >= 0 && *choice < static_cast<int>(std::size(kScalePresets)))
                    APPLICATION->settings()->set("BigScreenUIScale", kScalePresets[static_cast<size_t>(*choice)]);
            };
        }
    }

    // Applied live (no restart needed) — see the SDL_SetWindowFullscreen()
    // call in the main render loop, right after UpdateFontScale(), which
    // compares against the last-applied value each frame and calls it again
    // only when this setting actually changes.
    DrawToggleSetting("BigScreenFullscreen", "Fullscreen",
                       "Run BigScreen as a fullscreen window instead of a resizable desktop window.");
}

void DrawSettingsWindow()
{
    DrawToggleSetting("LaunchMaximized", TR("MinecraftSettingsWidget", "Start Minecraft maximized"),
                       "Start Minecraft's window maximized.");
}

void DrawSettingsConsole()
{
    DrawToggleSetting("ShowConsole", TR("MinecraftSettingsWidget", "When the game is launched, show the console window"),
                       "Open the console window automatically when an instance launches.");
    DrawToggleSetting("ShowConsoleOnError", TR("MinecraftSettingsWidget", "When the game crashes, show the console window"),
                       "Open the console window automatically if an instance crashes.");
    DrawToggleSetting("AutoCloseConsole", TR("MinecraftSettingsWidget", "When the game quits, hide the console window"),
                       "Close the console window automatically when the game exits successfully.");
    DrawToggleSetting("ConsoleOverflowStop", StripMnemonic(TR("LauncherPage", "&Stop logging when log overflows")),
                       "Stop appending new lines once the console's line limit is reached.");
}

void DrawSettingsInstances()
{
    // Desktop's checkbox is the inverted phrasing of this key (checked =
    // "suggest update" = SkipModpackUpdatePrompt *false*) — same pattern as
    // ModMetadataDisabled/ModDependenciesDisabled above, hence invert=true.
    DrawToggleSetting("SkipModpackUpdatePrompt",
                       TR("LauncherPage", "Suggest to update an existing instance during modpack installation"),
                       TR("LauncherPage", "When creating a new modpack instance, suggest updating an existing instance instead."), true);
    DrawToggleSetting(
        "DownloadGameFilesDuringInstanceCreation", TR("LauncherPage", "Download game files during instance creation"),
        TR("LauncherPage", "Downloads required game files while creating the instance. Disable this to skip the initial download and "
                            "fetch files when the instance is launched instead."));
}

void DrawSettingsMemory()
{
    // Titles use "Usage" (desktop's own wording) instead of "Allocation";
    // MinMemAlloc's description also switches to the desktop's shorter
    // wording so it picks up the existing translation — MaxMemAlloc's was
    // already byte-for-byte identical to begin with.
    DrawMemorySetting("MinMemAlloc", TrimTrailingColon(StripMnemonic(TR("JavaSettingsWidget", "M&inimum Memory Usage:"))),
                       TR("JavaSettingsWidget", "The amount of memory Minecraft is started with."));
    DrawMemorySetting("MaxMemAlloc", TrimTrailingColon(StripMnemonic(TR("JavaSettingsWidget", "Ma&ximum Memory Usage:"))),
                       TR("JavaSettingsWidget", "The maximum amount of memory Minecraft is allowed to use."));
}

void DrawSettingsJavaAdvanced()
{
    DrawToggleSetting("AutomaticJavaDownload", StripMnemonic(TR("JavaSettingsWidget", "Auto-download &Mojang Java")),
                       TR("JavaSettingsWidget", "Automatically downloads and selects the Java build recommended by Mojang."));
    DrawToggleSetting("IgnoreJavaCompatibility", TR("JavaSettingsWidget", "Skip Java compatibility checks"),
                       TR("JavaSettingsWidget",
                          "If enabled, the launcher will not check if an instance is compatible with the selected Java version."));
    // "Java Arguments" is a QGroupBox title on the desktop side (the
    // actual text field has no label/description of its own), so only the
    // title is reused here.
    DrawTextSetting("JvmArgs", StripMnemonic(TR("JavaSettingsWidget", "Java Argumen&ts")),
                     "Additional arguments passed to the Java virtual machine.");
}

void DrawSettingsPerformance()
{
    // These three have HTML-rich-text tooltips on the desktop side (e.g.
    // "<html><body><p>Enable Feral Interactive's GameMode...</p></body>
    // </html>") — reused for the title only; descriptions stay BigScreen's
    // own plain text rather than leaking raw HTML tags into the ImGui UI.
    DrawToggleSetting("EnableFeralGamemode", TR("MinecraftSettingsWidget", "Enable Feral GameMode"),
                       "Request performance optimizations from GameMode while playing (Linux only).");
    DrawToggleSetting("EnableMangoHud", TR("MinecraftSettingsWidget", "Enable MangoHud"),
                       "Show the MangoHud performance overlay while playing.");
    DrawToggleSetting("UseDiscreteGpu", TR("MinecraftSettingsWidget", "Use discrete GPU"),
                       "Hint the system to use the discrete GPU, if there is one.");
    // UseZink's tooltip is plain text (unlike the three above), so both
    // title and description are reused.
    DrawToggleSetting("UseZink", TR("MinecraftSettingsWidget", "Use Zink"),
                       TR("MinecraftSettingsWidget",
                          "Use Zink, a Mesa OpenGL driver that implements OpenGL on top of Vulkan. Performance may vary depending on the "
                          "situation. Note: If no suitable Vulkan driver is found, software rendering will be used."));
}

void DrawSettingsCommands()
{
    DrawTextSetting("WrapperCommand", StripMnemonic(TR("CustomCommands", "&Wrapper Command")),
                     "Command to wrap the Minecraft launch with (e.g. gamemoderun, mangohud).");
    DrawTextSetting("PreLaunchCommand", StripMnemonic(TR("CustomCommands", "&Pre-launch Command")),
                     "Command to run before Minecraft starts.");
    DrawTextSetting("PostExitCommand", StripMnemonic(TR("CustomCommands", "P&ost-exit Command")),
                     "Command to run after Minecraft exits.");
}

void DrawSettingsProxy()
{
    DrawChoiceSetting("ProxyType", TR("ProxyPage", "Type"), "Type of proxy to route network requests through.",
                       { "Default", "None", "SOCKS5", "HTTP" });
    DrawTextSetting("ProxyAddr", "Proxy Address", "Hostname or IP address of the proxy server.");
    DrawTextSetting("ProxyPort", "Proxy Port", "Port number of the proxy server.");
    DrawTextSetting("ProxyUser", TrimTrailingColon(StripMnemonic(TR("ProxyPage", "&Username:"))),
                     "Username for proxy authentication, if required.");
    DrawTextSetting("ProxyPass", TrimTrailingColon(StripMnemonic(TR("ProxyPage", "&Password:"))),
                     "Password for proxy authentication, if required.", /*isPassword=*/true);
}

void DrawSettingsLanguage()
{
    // The desktop Language page builds its list dynamically from whatever
    // translation files TranslationsModel finds at runtime, and has no
    // static "Language" label to reuse at all (LanguageSelectionWidget is
    // the whole page — confirmed no matching source string exists);
    // duplicating that discovery here for a picker isn't worth it yet, so
    // this stays a fixed list of the more common ones, entirely
    // BigScreen's own text. Empty string (shown as "System") is
    // PrismLauncher's own default meaning "follow the system locale".
    DrawChoiceSetting("Language", "Language", "Language used throughout the launcher.",
                       { "en_US", "ru_RU", "de_DE", "fr_FR", "es_ES", "zh_CN", "ja_JP", "pt_BR" });
}

void DrawSettingsServicesBehavior()
{
    DrawToggleSetting("FallbackMRBlockedMods", TR("APIPage", "Enable fallback to Modrinth for blocked mods"),
                       "Try downloading a mod from Modrinth if a CurseForge download is blocked.");
    DrawToggleSetting("MetaRefreshOnLaunch", TR("APIPage", "Refresh on launch"),
                       "Check for updated version metadata every time an instance launches.");
}

void DrawSettingsServicesApiKeys()
{
    // These are generic section labels on the desktop side ("Modrinth",
    // "CurseForge", ...) rather than "X API Token" — kept alongside
    // BigScreen's own fuller description, which does the disambiguating.
    DrawTextSetting("ModrinthToken", StripMnemonic(TR("APIPage", "Mod&rinth")),
                     "Personal access token for the Modrinth API.");
    DrawTextSetting("FlameKeyOverride", StripMnemonic(TR("APIPage", "&CurseForge")),
                     "Override for the CurseForge (Flame) API key.");
    DrawTextSetting("TechnicClientID", StripMnemonic(TR("APIPage", "&Technic")),
                     "Client ID used for Technic platform API requests.");
    DrawTextSetting("PastebinCustomAPIBase", StripMnemonic(TR("APIPage", "Base &URL")),
                     "Base URL for a self-hosted or alternate Pastebin-compatible service.");
}

void DrawSettingsTools()
{
    DrawTextSetting("JProfilerPath", StripMnemonic(TR("ExternalToolsPage", "J&Profiler")),
                     "Path to the JProfiler executable, for the JProfiler launch profiler.");
    DrawTextSetting("JVisualVMPath", StripMnemonic(TR("ExternalToolsPage", "&VisualVM")),
                     "Path to the JVisualVM executable, for the JVisualVM launch profiler.");
    DrawTextSetting("MCEditPath", StripMnemonic(TR("ExternalToolsPage", "&MCEdit")),
                     "Path to the MCEdit executable, for editing worlds.");
    DrawTextSetting("JsonEditor", StripMnemonic(TR("ExternalToolsPage", "&Text Editor")),
                     TR("ExternalToolsPage", "Used to edit component JSON files."));
}

struct SettingsSubTab {
    const char* name;
    void (*draw)();
};

struct SettingsTopTab {
    const char* name;
    const char* icon;
    const SettingsSubTab* subtabs;
    int subtabCount;
};

static const SettingsSubTab kGeneralSubTabs[] = {
    { "Mods & Downloads", &DrawSettingsGeneral },
};
static const SettingsSubTab kAppearanceSubTabs[] = {
    { "Theme", &DrawSettingsAppearance },
};
static const SettingsSubTab kMinecraftSubTabs[] = {
    { "Window", &DrawSettingsWindow },
    { "Console", &DrawSettingsConsole },
    { "Instances", &DrawSettingsInstances },
};
static const SettingsSubTab kJavaSubTabs[] = {
    { "Memory", &DrawSettingsMemory },
    { "Advanced", &DrawSettingsJavaAdvanced },
};
static const SettingsSubTab kProxySubTabs[] = {
    { "Server", &DrawSettingsProxy },
};
static const SettingsSubTab kSystemSubTabs[] = {
    { "Performance", &DrawSettingsPerformance },
    { "Commands", &DrawSettingsCommands },
};
static const SettingsSubTab kLanguageSubTabs[] = {
    { "Language", &DrawSettingsLanguage },
};
static const SettingsSubTab kServicesSubTabs[] = {
    { "Behavior", &DrawSettingsServicesBehavior },
    { "API Keys", &DrawSettingsServicesApiKeys },
};
static const SettingsSubTab kToolsSubTabs[] = {
    { "External Tools", &DrawSettingsTools },
};
// 9 categories — matches the desktop settings dialog's page count. Not a
// literal 1:1 mirror of its 9 pages, though: "Accounts" isn't repeated here
// since it already has its own dedicated screen (Screen::Accounts, reachable
// from Landing) — a duplicate entry would just be confusing rather than
// helpful — and "System" (Performance/Commands) doesn't exist on the
// desktop side at all, added here instead for BigScreen/Steam Deck-relevant
// settings (GameMode, MangoHud, Zink, ...) that don't have a home in the
// original page set.
static const SettingsTopTab kSettingsTabs[] = {
    { "General", "images/icons/tab_general_home.png", kGeneralSubTabs, static_cast<int>(std::size(kGeneralSubTabs)) },
    { "Language", "images/icons/tab_language.png", kLanguageSubTabs, static_cast<int>(std::size(kLanguageSubTabs)) },
    { "Appearance", "images/icons/tab_appearance.png", kAppearanceSubTabs, static_cast<int>(std::size(kAppearanceSubTabs)) },
    { "Minecraft", "images/icons/tab_general.png", kMinecraftSubTabs, static_cast<int>(std::size(kMinecraftSubTabs)) },
    { "Java", "images/icons/tab_java.png", kJavaSubTabs, static_cast<int>(std::size(kJavaSubTabs)) },
    { "Services", "images/icons/tab_services.png", kServicesSubTabs, static_cast<int>(std::size(kServicesSubTabs)) },
    { "Tools", "images/icons/tab_tools.png", kToolsSubTabs, static_cast<int>(std::size(kToolsSubTabs)) },
    { "Proxy", "images/icons/tab_proxy.png", kProxySubTabs, static_cast<int>(std::size(kProxySubTabs)) },
    { "System", "images/icons/tab_system.png", kSystemSubTabs, static_cast<int>(std::size(kSystemSubTabs)) },
};

int g_settingsTab = 0;
int g_settingsSubTab = 0;

void DrawSettings()
{
    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        if (kSettingsTabs[g_settingsTab].subtabCount > 1) {
            SetFooterHints({ { glyphs.confirm(false), "Toggle / Change" },
                              { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                              { ICON_PF_XBOX_LT "/" ICON_PF_XBOX_RT, "Tab" },
                              { glyphs.cancel(false), "Back" } });
        } else {
            SetFooterHints({ { glyphs.confirm(false), "Toggle / Change" },
                              { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                              { glyphs.cancel(false), "Back" } });
        }
    }

    const int tabCount = static_cast<int>(std::size(kSettingsTabs));

    // Checked before BeginScreen() so this frame's top-bar tab icons (drawn
    // by BeginScreen itself — see TopBarTab) already reflect the switch,
    // and before Sub-tab switching so a top-tab change's g_settingsSubTab
    // reset is visible immediately too. LB/RB/LT/RT rather than D-pad:
    // NavTab (used for the sub-tab row below) is deliberately excluded from
    // gamepad nav — its own comment in the vendored ImGuiFullscreen.cpp
    // explains why: "usually activated with the bumpers and/or the back
    // button" — and the top-level tabs moved into the title bar itself
    // (matching the reference) aren't interactive widgets at all, just
    // icons, so they need the same shoulder-button switching to mean
    // anything.
    // Every branch here also queues a focus reset: without it, the
    // highlighted item stays pointed at a widget ID that no longer exists
    // once the tab/sub-tab switches (the settings list underneath is
    // completely different content), so nothing visibly appears selected
    // until the next D-pad/stick press — looked like the selection was
    // "slow to show up".
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        g_settingsTab = (g_settingsTab + 1) % tabCount;
        g_settingsSubTab = 0;
        QueueResetFocus(FocusResetType::Other);
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        g_settingsTab = (g_settingsTab - 1 + tabCount) % tabCount;
        g_settingsSubTab = 0;
        QueueResetFocus(FocusResetType::Other);
    }

    const SettingsTopTab& currentTab = kSettingsTabs[g_settingsTab];
    // Nothing to switch to with only one sub-tab (e.g. General's "Mods &
    // Downloads") — skip LT/RT handling, and skip drawing the sub-tab row
    // at all below, rather than showing a strip with a single, permanently-
    // selected, unclickable tab.
    if (currentTab.subtabCount > 1) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) {
            g_settingsSubTab = (g_settingsSubTab + 1) % currentTab.subtabCount;
            QueueResetFocus(FocusResetType::Other);
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) {
            g_settingsSubTab = (g_settingsSubTab - 1 + currentTab.subtabCount) % currentTab.subtabCount;
            QueueResetFocus(FocusResetType::Other);
        }
    }

    TopBarTab topTabs[std::size(kSettingsTabs)];
    for (int i = 0; i < tabCount; ++i)
        topTabs[i] = { kSettingsTabs[i].icon, i == g_settingsTab };

    // Title shows which category is selected (e.g. "Settings — Java"), per
    // feedback that "Settings" alone didn't say which tab you were on.
    const std::string screenTitle = std::string("Settings \xE2\x80\x94 ") + currentTab.name;

    if (BeginScreen(screenTitle.c_str(), true, topTabs)) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "settings")) {
            // The sub-tab row is its own non-scrolling child, separate from
            // the settings list below — otherwise (a) NavTab's trailing
            // ImGui::SameLine() (it always calls it, to chain the *next*
            // tab onto the same line, with no way to know it was the last
            // one) would carry straight into the first setting row, placing
            // it beside "Instances" instead of on its own line below, and
            // (b) scrolling a long settings list would carry the tab row
            // away with it instead of keeping it pinned, per feedback on
            // both.
            // ImGuiChildFlags_NavFlattened on both: without it, each nested
            // BeginChild becomes its own separate nav boundary instead of
            // merging into the parent's — D-pad nav couldn't reach into
            // (or back out of) the settings list at all. BeginFullscreenColumnWindow
            // itself already passes this same flag for exactly this reason.
            if (currentTab.subtabCount > 1) {
                ImGui::BeginChild("settings_subtabs",
                                   ImVec2(0.0f, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                                                     ImGui::GetStyle().FramePadding.y * 2.0f + LayoutScale(4.0f)),
                                   ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
                BeginNavBar();
                for (int i = 0; i < currentTab.subtabCount; ++i) {
                    if (NavTab(currentTab.subtabs[i].name, i == g_settingsSubTab, true, 150.0f, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                               UISecondaryColor)) {
                        g_settingsSubTab = i;
                        QueueResetFocus(FocusResetType::Other);
                    }
                }
                EndNavBar();
                ImGui::EndChild();
            }

            ImGui::BeginChild("settings_content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
            BeginMenuButtons();
            // Every QueueResetFocus() call above (tab switch, sub-tab
            // switch) queues a reset, but nothing was ever *consuming* it
            // for this window specifically — ImGuiFullscreen's own dialogs
            // (DrawChoiceDialog, DrawInputDialog, ...) all call this same
            // ResetFocusHere() right after their own BeginMenuButtons(),
            // but main.cpp never did for the regular Settings content
            // list, leaving it to Dear ImGui's own default "focus the
            // first widget in a newly-focused window" behavior — which
            // turns out to be unreliable with exactly one focusable
            // widget (a single-toggle sub-tab like Window's
            // LaunchMaximized): with 2+ items, an imprecise initial focus
            // still leaves something to move Up/Down between and
            // eventually reach the right one, but with only one item and
            // nothing else to move to, landing outside it at all means
            // there's no way to ever reach it. Forcing the reset here
            // deterministically, the same way the built-in dialogs do,
            // fixes that regardless of item count.
            ResetFocusHere();
            currentTab.subtabs[g_settingsSubTab].draw();
            EndMenuButtons();
            ImGui::EndChild();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

// ---- Instance Settings (X-menu "Edit...") — per-instance overrides of
// global settings, the "settings" tab of the desktop's Edit Instance
// window (InstanceSettingsPage -> MinecraftSettingsWidget,
// launcher/ui/pages/instance/InstanceSettingsPage.h). A cut-down mirror of
// DrawSettings() above, scoped to one instance's own SettingsObject
// (instance->settings()) instead of APPLICATION->settings() — reuses the
// exact same DrawToggleSetting/DrawChoiceSetting/DrawMemorySetting/
// DrawTextSetting helpers (each takes an optional trailing SettingsObject*
// for exactly this) since per-instance settings are the same SettingsObject
// get()/set() API with a transparent global-inheritance layer underneath
// (MinecraftInstance::loadSpecificSettings()/BaseInstance's constructor —
// each registerOverride(globalSetting, gateFlag) call wires one key to fall
// back to the global value whenever its gate flag is off). Each group below
// starts with a toggle for that real "OverrideXxx" gate — matching the
// desktop's own checkable QGroupBox — and only draws the gated fields while
// it's on, rather than showing them present-but-disabled.
//
// Deliberately not ported this round (real gaps, not oversights): window
// width/height (no numeric entry widget exists yet, unlike the MB-preset
// picker memory settings get), Environment Variables (a key/value list
// editor), native library workaround paths, account-for-instance override
// (needs an account picker), mod-download-loader override, and everything
// on the desktop's other Edit Instance tabs (Version/Mods/Worlds/
// Screenshots/Servers/Resource-Texture-Shader-packs/log viewers — each a
// real, separate, substantially larger BigScreen screen of its own).

void DrawInstanceSettingsGeneral()
{
    SettingsObject* s = g_instanceSettingsTarget->settings();

    DrawToggleSetting("OverrideWindow", StripMnemonic(TR("MinecraftSettingsWidget", "Game &Window")),
                       "Use different window settings for this instance.", false, s);
    if (s->get("OverrideWindow").toBool())
        DrawToggleSetting("LaunchMaximized", TR("MinecraftSettingsWidget", "Start Minecraft maximized"),
                           "Start Minecraft's window maximized.", false, s);

    DrawToggleSetting("OverrideConsole", StripMnemonic(TR("MinecraftSettingsWidget", "&Console Window")),
                       "Use different console settings for this instance.", false, s);
    if (s->get("OverrideConsole").toBool()) {
        DrawToggleSetting("ShowConsole", TR("MinecraftSettingsWidget", "When the game is launched, show the console window"),
                           "Open the console window automatically when this instance launches.", false, s);
        DrawToggleSetting("ShowConsoleOnError", TR("MinecraftSettingsWidget", "When the game crashes, show the console window"),
                           "Open the console window automatically if this instance crashes.", false, s);
        DrawToggleSetting("AutoCloseConsole", TR("MinecraftSettingsWidget", "When the game quits, hide the console window"),
                           "Close the console window automatically when the game exits successfully.", false, s);
    }

    DrawToggleSetting("OverrideGameTime", StripMnemonic(TR("MinecraftSettingsWidget", "Game &Time")),
                       "Use different play-time tracking for this instance.", false, s);
    if (s->get("OverrideGameTime").toBool()) {
        DrawToggleSetting("ShowGameTime", StripMnemonic(TR("MinecraftSettingsWidget", "Show time &playing this instance")),
                           "Show how long this instance has been played for.", false, s);
        DrawToggleSetting("RecordGameTime", StripMnemonic(TR("MinecraftSettingsWidget", "&Record time playing this instance")),
                           "Keep track of how long this instance has been played for.", false, s);
    }
    DrawToggleSetting("CountGameTime",
                       StripMnemonic(TR("MinecraftSettingsWidget", "&Count time playing this instance into total time played")),
                       "Add this instance's play time to the launcher's overall total.", false, s);

    DrawToggleSetting("OverrideMiscellaneous", "Miscellaneous", "Use different launcher-behavior settings for this instance.", false, s);
    if (s->get("OverrideMiscellaneous").toBool()) {
        DrawToggleSetting("CloseAfterLaunch", TR("MinecraftSettingsWidget", "When the game window opens, hide the launcher"),
                           "Hide the launcher once this instance's game window appears.", false, s);
        DrawToggleSetting("QuitAfterGameStop", TR("MinecraftSettingsWidget", "When the game window closes, quit the launcher"),
                           "Quit the launcher once this instance's game exits.", false, s);
    }
}

void DrawInstanceSettingsJava()
{
    SettingsObject* s = g_instanceSettingsTarget->settings();

    DrawToggleSetting("OverrideJavaLocation", StripMnemonic(TR("JavaSettingsWidget", "Java Insta&llation")),
                       "Use a different Java installation for this instance.", false, s);
    if (s->get("OverrideJavaLocation").toBool()) {
        DrawTextSetting("JavaPath", StripMnemonic(TR("JavaSettingsWidget", "Java &Executable")),
                         "Path to the JRE/JDK executable to launch this instance with.", false, s);
        DrawToggleSetting("IgnoreJavaCompatibility", TR("JavaSettingsWidget", "Skip Java compatibility checks"),
                           TR("JavaSettingsWidget",
                              "If enabled, the launcher will not check if this instance is compatible with the selected Java version."),
                           false, s);
    }

    DrawToggleSetting("OverrideJavaArgs", StripMnemonic(TR("JavaSettingsWidget", "Java Argumen&ts")),
                       "Use different JVM arguments for this instance.", false, s);
    if (s->get("OverrideJavaArgs").toBool())
        DrawTextSetting("JvmArgs", StripMnemonic(TR("JavaSettingsWidget", "Java Argumen&ts")),
                         "Additional arguments passed to the Java virtual machine.", false, s);

    DrawToggleSetting("OverrideMemory", StripMnemonic(TR("JavaSettingsWidget", "Memor&y")),
                       "Use different memory limits for this instance.", false, s);
    if (s->get("OverrideMemory").toBool()) {
        DrawMemorySetting("MinMemAlloc", TrimTrailingColon(StripMnemonic(TR("JavaSettingsWidget", "M&inimum Memory Usage:"))),
                           TR("JavaSettingsWidget", "The amount of memory Minecraft is started with."), s);
        DrawMemorySetting("MaxMemAlloc", TrimTrailingColon(StripMnemonic(TR("JavaSettingsWidget", "Ma&ximum Memory Usage:"))),
                           TR("JavaSettingsWidget", "The maximum amount of memory Minecraft is allowed to use."), s);
    }
}

void DrawInstanceSettingsTweaks()
{
    SettingsObject* s = g_instanceSettingsTarget->settings();

    DrawToggleSetting("OverridePerformance", StripMnemonic(TR("MinecraftSettingsWidget", "&Performance")),
                       "Use different performance-related settings for this instance.", false, s);
    if (s->get("OverridePerformance").toBool()) {
        DrawToggleSetting("EnableFeralGamemode", TR("MinecraftSettingsWidget", "Enable Feral GameMode"),
                           "Request performance optimizations from GameMode while playing (Linux only).", false, s);
        DrawToggleSetting("EnableMangoHud", TR("MinecraftSettingsWidget", "Enable MangoHud"),
                           "Show the MangoHud performance overlay while playing.", false, s);
        DrawToggleSetting("UseDiscreteGpu", TR("MinecraftSettingsWidget", "Use discrete GPU"),
                           "Hint the system to use the discrete GPU, if there is one.", false, s);
        DrawToggleSetting("UseZink", TR("MinecraftSettingsWidget", "Use Zink"),
                           TR("MinecraftSettingsWidget",
                              "Use Zink, a Mesa OpenGL driver that implements OpenGL on top of Vulkan. Performance may vary depending on "
                              "the situation. Note: If no suitable Vulkan driver is found, software rendering will be used."),
                           false, s);
    }

    DrawToggleSetting("OverrideLegacySettings", StripMnemonic(TR("MinecraftSettingsWidget", "&Legacy Tweaks")),
                       "Use different legacy-compatibility settings for this instance.", false, s);
    if (s->get("OverrideLegacySettings").toBool())
        DrawToggleSetting("OnlineFixes", TR("MinecraftSettingsWidget", "Enable online fixes (experimental)"),
                           "Emulate old online services (skin and online-mode support) that are no longer operating.", false, s);
}

void DrawInstanceSettingsCommands()
{
    SettingsObject* s = g_instanceSettingsTarget->settings();

    DrawToggleSetting("OverrideCommands", TR("MinecraftSettingsWidget", "Custom Commands"),
                       "Use different custom commands for this instance.", false, s);
    if (s->get("OverrideCommands").toBool()) {
        DrawTextSetting("WrapperCommand", StripMnemonic(TR("CustomCommands", "&Wrapper Command")),
                         "Command to wrap the Minecraft launch with (e.g. gamemoderun, mangohud).", false, s);
        DrawTextSetting("PreLaunchCommand", StripMnemonic(TR("CustomCommands", "&Pre-launch Command")),
                         "Command to run before Minecraft starts.", false, s);
        DrawTextSetting("PostExitCommand", StripMnemonic(TR("CustomCommands", "P&ost-exit Command")),
                         "Command to run after Minecraft exits.", false, s);
    }
}

void DrawInstanceSettingsNotes()
{
    SettingsObject* s = g_instanceSettingsTarget->settings();
    DrawTextSetting("notes", TR("NotesPage", "Notes"), "Freeform notes about this instance.", false, s);
}

struct InstanceSettingsTab {
    const char* name;
    void (*draw)();
};
static const InstanceSettingsTab kInstanceSettingsTabs[] = {
    { "General", &DrawInstanceSettingsGeneral }, { "Java", &DrawInstanceSettingsJava },     { "Tweaks", &DrawInstanceSettingsTweaks },
    { "Commands", &DrawInstanceSettingsCommands }, { "Notes", &DrawInstanceSettingsNotes },
};
int g_instanceSettingsTab = 0;

void DrawInstanceSettings()
{
    // Shouldn't happen (only reachable via ShowInstanceActionsMenu(), which
    // always sets this first) — defensive fallback instead of dereferencing
    // null.
    if (!g_instanceSettingsTarget) {
        SetScreen(Screen::Instances);
        return;
    }

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Toggle / Change" },
                          { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                          { glyphs.cancel(false), "Back" } });
    }

    const int tabCount = static_cast<int>(std::size(kInstanceSettingsTabs));
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        g_instanceSettingsTab = (g_instanceSettingsTab + 1) % tabCount;
        QueueResetFocus(FocusResetType::Other);
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        g_instanceSettingsTab = (g_instanceSettingsTab - 1 + tabCount) % tabCount;
        QueueResetFocus(FocusResetType::Other);
    }

    // "Edit... — <instance name> — <category>", matching Settings' own
    // "Settings — <category>" pattern.
    const std::string screenTitle = StripMnemonic(MW("&Edit...")).toStdString() + " \xE2\x80\x94 " +
                                     g_instanceSettingsTarget->name().toStdString() + " \xE2\x80\x94 " +
                                     kInstanceSettingsTabs[g_instanceSettingsTab].name;

    if (BeginScreen(screenTitle.c_str())) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "instance_settings")) {
            // Same non-scrolling tab-row / scrolling-content two-child-window
            // split DrawSettings() uses for its sub-tab row, and for the
            // same reasons (NavTab's own trailing SameLine() would otherwise
            // run into the first setting row; keeps the tab row pinned while
            // the content below it scrolls).
            ImGui::BeginChild("instance_settings_tabs",
                               ImVec2(0.0f, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + ImGui::GetStyle().FramePadding.y * 2.0f +
                                                 LayoutScale(4.0f)),
                               ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
            BeginNavBar();
            for (int i = 0; i < tabCount; ++i) {
                if (NavTab(kInstanceSettingsTabs[i].name, i == g_instanceSettingsTab, true, 150.0f, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                           UISecondaryColor)) {
                    g_instanceSettingsTab = i;
                    QueueResetFocus(FocusResetType::Other);
                }
            }
            EndNavBar();
            ImGui::EndChild();

            ImGui::BeginChild("instance_settings_content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
            BeginMenuButtons();
            ResetFocusHere();
            kInstanceSettingsTabs[g_instanceSettingsTab].draw();
            EndMenuButtons();
            ImGui::EndChild();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

// Shared by the HorizontalMenuItem confirm handler below and the "Launch"
// entry in the X-button actions menu.
void LaunchInstance(MinecraftInstance* inst)
{
    if (inst->isRunning()) {
        g_consoleInstance = inst;
        SetScreen(Screen::Console);
    } else if (inst->canLaunch()) {
        APPLICATION->launch(inst, LaunchMode::Normal, nullptr, nullptr, QString(), &BigScreenLaunchController::create);
        g_consoleInstance = inst;
        SetScreen(Screen::Console);
    }
}

// X on a focused instance card opens this — a curated subset of the
// desktop's right-click context menu (MainWindow::showInstanceContextMenu),
// picking the actions that make sense without a mouse/keyboard: launching,
// killing, opening the console, editing settings (Screen::InstanceSettings,
// via DrawInstanceSettings() above — the "settings" tab of the desktop's
// full Edit Instance window, not the whole thing), renaming, changing
// group, viewing the instance folder, and deleting. Not yet ported: the
// desktop Edit Instance window's other tabs (Version/Mods/Worlds/
// Screenshots/Servers/resource-texture-shader-packs/log viewers), Copy
// Instance, Export, Create Shortcut — all native Qt Widgets dialogs/pages
// with enough surface area (checkboxes, radio groups, file pickers, list
// management) that porting each is its own follow-up, not a simple
// BigScreenDialogs::* swap like the actions below.
//
// Uses the raw, non-blocking OpenChoiceDialog directly rather than
// BigScreenDialogs::Choose() — this is called from inside DrawInstances(),
// itself mid-frame, and Choose() blocks by pumping frames in a loop, which
// would recursively re-enter the very frame this call is already inside of
// (see DialogHelpers.h). OpenChoiceDialog just queues state and renders
// across future frames instead, so it's safe to call directly here — same
// reasoning as HandleBackButton()'s quit-confirm dialog. Its callback (see
// below) fires from inside a *later* frame's EndLayout() — still mid-frame
// — so any action needing a follow-up BigScreenDialogs::* call (Kill,
// Rename, Change Group, Delete) defers that through g_pendingAction rather
// than calling it directly; Launch/Open Console/View Folder don't need a
// dialog at all, so they run immediately.
void ShowInstanceActionsMenu(MinecraftInstance* inst)
{
    struct Action {
        std::string label;
        std::function<void()> run;
    };
    auto actions = std::make_shared<std::vector<Action>>();

    // Labels below use MW() with MainWindow.ui's exact action text
    // (including its '&' mnemonic, stripped for display by StripMnemonic)
    // where one exists, so the menu item picks up the same translation the
    // desktop's own right-click menu already has for it.
    if (inst->isRunning()) {
        // No desktop equivalent for this one (opening BigScreen's own
        // Console screen isn't a concept the desktop UI has).
        actions->push_back({ "Open Console", [inst]() {
                                 g_consoleInstance = inst;
                                 SetScreen(Screen::Console);
                             } });
        actions->push_back({ StripMnemonic(MW("&Kill")).toStdString(), [inst]() {
                                 g_pendingAction = [inst]() { APPLICATION->kill(inst); };
                             } });
    } else if (inst->canLaunch()) {
        actions->push_back({ StripMnemonic(MW("&Launch")).toStdString(), [inst]() { LaunchInstance(inst); } });
    }

    actions->push_back({ StripMnemonic(MW("&Edit...")).toStdString(), [inst]() {
                             g_instanceSettingsTarget = inst;
                             SetScreen(Screen::InstanceSettings);
                         } });

    // "Rename" here has no desktop dialog to match (the desktop version
    // edits the name inline in the instance list instead of via a popup),
    // so this text stays BigScreen's own.
    actions->push_back({ "Rename", [inst]() {
                             g_pendingAction = [inst]() {
                                 const auto result =
                                     BigScreenDialogs::InputString("Rename Instance", "Enter a new name", inst->name().toStdString());
                                 if (result && !result->isEmpty())
                                     inst->setName(*result);
                             };
                         } });
    actions->push_back({ StripMnemonic(MW("&Change Group...")).toStdString(), [inst]() {
                             g_pendingAction = [inst]() {
                                 const QString currentGroup = APPLICATION->instances()->getInstanceGroup(inst->id());
                                 const auto result = BigScreenDialogs::InputString(MW("Group name").toStdString(),
                                                                                    MW("Enter a new group name.").toStdString(),
                                                                                    currentGroup.toStdString());
                                 if (result)
                                     APPLICATION->instances()->setInstanceGroup(inst->id(), result->simplified());
                             };
                         } });
    actions->push_back({ StripMnemonic(MW("&Folder")).toStdString(), [inst]() { DesktopServices::openPath(inst->instanceRoot()); } });

    if (!inst->isRunning()) {
        actions->push_back({ StripMnemonic(MW("Dele&te")).toStdString(), [inst]() {
                                 g_pendingAction = [inst]() {
                                     // Matches MainWindow::on_actionDeleteInstance_triggered()'s
                                     // confirmation text exactly (%2 is its
                                     // "and its N registered shortcut(s)"
                                     // clause — always empty here, since
                                     // BigScreen doesn't have a shortcuts
                                     // concept), so it picks up the same
                                     // translation.
                                     const QString message =
                                         MW("You are about to delete \"%1\"%2.\n"
                                            "This may be permanent and will completely delete the instance.\n\n"
                                            "Are you sure?")
                                             .arg(inst->name(), QString());
                                     if (!BigScreenDialogs::Confirm(MW("Confirm Deletion").toStdString(), message.toStdString(), false,
                                                                     "Delete", "Cancel"))
                                         return;
                                     const QString id = inst->id();
                                     if (!APPLICATION->instances()->trashInstance(id))
                                         APPLICATION->instances()->deleteInstance(id);
                                 };
                             } });
    }

    ChoiceDialogOptions options;
    options.reserve(actions->size());
    for (const Action& a : *actions)
        options.emplace_back(a.label, false);

    // BigScreen note: OpenChoiceDialog's own DrawChoiceDialog() only calls
    // CloseChoiceDialog() automatically on cancel (!is_open) — a genuine
    // selection (choice >= 0) never closes it by design, since that's also
    // how checkable multi-select choice dialogs are meant to behave (pick
    // several, close explicitly). This is a plain single-select menu, so
    // the callback closes it itself once an action is actually chosen —
    // without this, picking anything here (e.g. "Edit...", which switches
    // g_screen) left the menu still open and rendering on top of whatever
    // screen came next, since DrawChoiceDialog() runs unconditionally every
    // frame regardless of g_screen.
    //
    // CRASH FIX: CloseChoiceDialog() must NOT be called directly from in
    // here. This lambda *is* s_choice_dialog_callback — DrawChoiceDialog()
    // is mid-way through invoking it (`s_choice_dialog_callback(choice,
    // ...)`) when we're running. CloseChoiceDialog() does
    // `ChoiceDialogCallback().swap(s_choice_dialog_callback)`, which
    // swaps *this very closure* (including the captured `actions`
    // shared_ptr) into a temporary that's destroyed at the end of that
    // statement — while we're still executing inside it. Continuing to
    // touch `actions` afterwards (the .run() call) was then a
    // use-after-free, which is exactly what crashed on "Edit..." (and any
    // other action here) once this fix started actually getting exercised.
    // Deferred through g_pendingAction instead, same as every other
    // blocking/stateful action in this file — runs at the very top of the
    // *next* frame, well outside DrawChoiceDialog()'s own call stack, so
    // swapping the (by-then-already-returned) callback is safe.
    OpenChoiceDialog(inst->name().toStdString(), false, std::move(options), [actions](s32 index, const std::string&, bool) {
        if (index >= 0 && static_cast<size_t>(index) < actions->size()) {
            g_pendingAction = [actions, index]() {
                CloseChoiceDialog();
                (*actions)[static_cast<size_t>(index)].run();
            };
        }
    });
}

// The actual instance-creation work for the Y-button "Add Instance" menu's
// "Vanilla Minecraft" entry — only ever invoked via g_pendingAction (see its
// comment), so it's safe to call the blocking BigScreenDialogs::* helpers
// and BigScreenDialogs::WaitForTask directly here: this runs from the top
// of a frame, before ImGui::NewFrame(), never nested inside one.
//
// v1 gap: this is the only creation method wired up so far — importing a
// modpack from Modrinth/CurseForge/a zip file all need their own (much
// larger) browsing UIs, deferred like the rest of M5.
void StartVanillaInstanceCreation()
{
    // "Add Instanc&e..." is actionAddInstance's exact text in MainWindow.ui
    // — reused (mnemonic stripped) as the title for every dialog this whole
    // flow shows, so they all pick up the same existing translation.
    const std::string dialogTitle = StripMnemonic(MW("Add Instanc&e...")).toStdString();

    Meta::VersionList::Ptr versionList = APPLICATION->metadataIndex()->get("net.minecraft");
    if (!versionList->isLoaded()) {
        Task::Ptr loadTask = versionList->getLoadTask();
        loadTask->start();
        BigScreenDialogs::WaitForTask(loadTask.get());
        if (!versionList->isLoaded()) {
            BigScreenDialogs::Confirm(dialogTitle, "Failed to load the Minecraft version list. Check your internet connection.", false,
                                       "OK", "OK");
            return;
        }
    }

    // The version *list* load above only fetches lightweight per-version
    // metadata (id/type/time) — descriptor() (== Meta::Version's raw "id"
    // field, e.g. "1.21.1") is populated from that, but name() falls back
    // to the *list's* uid ("net.minecraft") until that specific version's
    // own full data is loaded separately, on demand — same as the desktop
    // UI only loading it once a version is actually selected. So this uses
    // descriptor() for the picker labels, and loads the chosen version's
    // data (below) before actually creating anything from it.
    std::vector<std::string> labels;
    std::vector<Meta::Version::Ptr> releaseVersions;
    for (int i = 0; i < versionList->count(); ++i) {
        Meta::Version::Ptr version = std::dynamic_pointer_cast<Meta::Version>(versionList->at(i));
        if (version && version->typeString() == "release")
            releaseVersions.push_back(version);
    }
    std::sort(releaseVersions.begin(), releaseVersions.end(),
              [](const Meta::Version::Ptr& a, const Meta::Version::Ptr& b) { return a->rawTime() > b->rawTime(); });
    for (const Meta::Version::Ptr& version : releaseVersions)
        labels.push_back(version->descriptor().toStdString());
    if (labels.empty())
        return;

    const auto versionChoice = BigScreenDialogs::Choose("Select Minecraft Version", labels);
    if (!versionChoice || *versionChoice < 0 || static_cast<size_t>(*versionChoice) >= releaseVersions.size())
        return;
    Meta::Version::Ptr chosenVersion = releaseVersions[static_cast<size_t>(*versionChoice)];

    if (!chosenVersion->isLoaded()) {
        Task::Ptr versionLoadTask = chosenVersion->loadTask();
        versionLoadTask->start();
        BigScreenDialogs::WaitForTask(versionLoadTask.get());
        if (!chosenVersion->isLoaded()) {
            BigScreenDialogs::Confirm(dialogTitle, "Failed to load version " + chosenVersion->descriptor().toStdString() + ".", false,
                                       "OK", "OK");
            return;
        }
    }

    const auto name =
        BigScreenDialogs::InputString("Instance Name", "Enter a name for the new instance", chosenVersion->descriptor().toStdString());
    if (!name || name->isEmpty())
        return;

    auto* creationTask = new VanillaCreationTask(chosenVersion);
    creationTask->setName(*name);

    unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(creationTask));
    task->start();
    BigScreenDialogs::WaitForTask(task.get());

    if (!task->wasSuccessful())
        BigScreenDialogs::Confirm(dialogTitle, "Failed to create instance: " + task->failReason().toStdString(), false, "OK", "OK");
}

// Y on the Instances screen opens this. Only one real creation method is
// wired up yet (see StartVanillaInstanceCreation's comment) — structured as
// a proper menu now so adding more (Modrinth, CurseForge, zip import, ...)
// later is just more entries, not a redesign. Same non-blocking
// OpenChoiceDialog reasoning as ShowInstanceActionsMenu above.
void ShowAddInstanceMenu()
{
    ChoiceDialogOptions options;
    options.emplace_back("Vanilla Minecraft", false);

    // See ShowInstanceActionsMenu()'s comment on why this closes the dialog
    // itself before acting — DrawChoiceDialog() doesn't do it automatically
    // for a real selection, only for cancel — and why that close has to be
    // deferred through g_pendingAction rather than called directly from in
    // here (this lambda *is* s_choice_dialog_callback while it's running;
    // CloseChoiceDialog() would swap it — and its captures — out from under
    // itself mid-call).
    OpenChoiceDialog(StripMnemonic(MW("Add Instanc&e...")).toStdString(), false, std::move(options),
                      [](s32 index, const std::string&, bool) {
                          if (index == 0) {
                              g_pendingAction = []() {
                                  CloseChoiceDialog();
                                  StartVanillaInstanceCreation();
                              };
                          }
                      });
}

void DrawInstances()
{
    InstanceList* instances = APPLICATION->instances();

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Launch" },
                          { glyphs.west, "Actions" },
                          { glyphs.north, "Add Instance" },
                          { glyphs.cancel(false), "Back" } });
    }

    // Guards X/Y the same way HandleBackButton() guards B: don't open a
    // second dialog on top of one that's already open (e.g. Y while the X
    // menu or a Rename prompt is already up).
    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

    if (BeginScreen("Instances")) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "instances")) {
            BeginNavBar();
            ResetFocusHere();

            // No explicit "< Back" card here (B already returns to Landing
            // via HandleBackButton()) — matches the reference: PCSX2's own
            // sub-screens rely on the footer's B hint, not a dedicated card.

            // Single horizontally-scrolling row, not a multi-row grid — a
            // fixed items-per-row grid meant every row after the first
            // needed vertical scrolling to reach (easy to lose track of
            // gamepad nav position in), and didn't adapt to how many
            // actually fit the real window width the way DrawLanding()'s
            // row now does. DrawScrollableCardRow handles centering when
            // everything fits and a scrolling view with "more this way"
            // chevrons when it doesn't — same as Landing, just with
            // however many instances there actually are instead of a
            // fixed 4.
            const int count = instances->count();
            const float rowWidthLogical = static_cast<float>(count) * LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH;
            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float rowHeight = LayoutScale(LAYOUT_HORIZONTAL_MENU_HEIGHT);
            ImGui::SetCursorPosY(std::max(0.0f, (availableHeight - rowHeight) * 0.5f));

            DrawScrollableCardRow("instances_row", rowWidthLogical, rowHeight, [&]() {
                for (int i = 0; i < count; ++i) {
                    MinecraftInstance* inst = instances->at(i);
                    if (!inst)
                        continue;

                    GSTexture* icon = GetInstanceIconTexture(inst);
                    if (!icon)
                        icon = GetCachedTexture("images/icons/instances.png");

                    const QByteArray nameUtf8 = inst->name().toUtf8();
                    const QString summaryStr = inst->isRunning() ? QObject::tr("Running") : QObject::tr("Ready to launch");
                    const QByteArray summaryUtf8 = summaryStr.toUtf8();

                    if (HorizontalMenuItem(icon, nameUtf8.constData(), summaryUtf8.constData()))
                        LaunchInstance(inst);

                    // X opens the actions menu for whichever card currently
                    // has nav focus — IsItemFocused() reports that for the
                    // item HorizontalMenuItem just submitted, same idiom as
                    // any other ImGui widget.
                    if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
                        ShowInstanceActionsMenu(inst);
                }
            });

            EndNavBar();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();

    // Y isn't tied to a focused card — it opens "Add Instance" regardless
    // of what's currently selected (or even if the list is empty).
    if (!anyDialogOpen && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
        ShowAddInstanceMenu();
}

void DrawConsole()
{
    const std::string title = g_consoleInstance ? (g_consoleInstance->name().toStdString() + " — Console") : "Console";

    SetFooterHints({ { GetGamepadGlyphs().cancel(false), "Back" } });

    if (BeginScreen(title.c_str())) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "console")) {
            BeginMenuButtons();
            ResetFocusHere();
            if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Back"))
                SetScreen(Screen::Instances);
            EndMenuButtons();

            ImGui::BeginChild("console_log", ImVec2(0.0f, 0.0f), true);
            LaunchTask* task = g_consoleInstance ? g_consoleInstance->getLaunchTask() : nullptr;
            if (task) {
                LogModel* log = task->getLogModel().get();
                const int rows = log ? log->rowCount(QModelIndex()) : 0;
                for (int i = 0; i < rows; ++i) {
                    const QString line = log->data(log->index(i, 0), Qt::DisplayRole).toString();
                    ImGui::TextUnformatted(line.toUtf8().constData());
                }
                if (rows > 0)
                    ImGui::SetScrollHereY(1.0f);
            } else {
                ImGui::TextUnformatted(g_consoleInstance && g_consoleInstance->isRunning() ? "(no log yet)" : "(instance is not running)");
            }
            ImGui::EndChild();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();
}

}  // namespace

// Maps SDL's own controller-type detection onto ImGuiFullscreen's
// InputLayout so ImGuiFullscreen::GetGamepadGlyphs() (and therefore every
// footer hint built from it, below) shows the actual button glyphs printed
// on whatever's plugged in — Xbox-style A/B/X/Y, PlayStation-style
// Cross/Circle/Square/Triangle, or Nintendo-style — instead of one
// hardcoded convention. SDL_GameControllerType has more values than
// InputLayout distinguishes (individual PS3/PS4/PS5 members, etc.); only
// the family matters here.
static InputLayout DetectGamepadLayout(SDL_GameController* pad)
{
    if (!pad)
        return InputLayout::Unknown;

    switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return InputLayout::Xbox;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return InputLayout::PlayStation;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            return InputLayout::Nintendo;
        default:
            // Recognized as a gamepad, just not one of the above families
            // (Steam Deck's own built-in controls report this way) — falls
            // back to GetGamepadGlyphs()'s position-based generic icons,
            // same as Unknown.
            return InputLayout::Generic;
    }
}

int main(int argc, char** argv)
{
    // EmuFolders::Resources defaults to BIGSCREEN_RESOURCES_DIR — an
    // absolute path baked in at compile time, into *this machine's* source
    // tree (bigscreen/CMakeLists.txt sets it to
    // ${CMAKE_CURRENT_SOURCE_DIR}/resources). That's fine for a local dev
    // build run straight out of build/, but not portable: a binary built
    // in CI and downloaded to run somewhere else (a packaged release, or
    // just copied to another machine) would still look for resources on
    // the CI runner's filesystem and fail to load any icon/font. If a
    // "resources" directory exists next to the actual running executable,
    // prefer that instead — CI packaging copies bigscreen/resources/
    // there. No QCoreApplication exists yet this early to ask for
    // applicationDirPath(), so this reads /proc/self/exe directly — fine
    // for v1's Linux-only scope.
    {
        std::error_code ec;
        const std::filesystem::path exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            const std::filesystem::path candidate = exePath.parent_path() / "resources";
            if (std::filesystem::is_directory(candidate, ec) && !ec)
                EmuFolders::Resources = candidate.string();
        }
    }

    // Launcher_logic compiles these Qt resources in (qrc_*.cpp), but a
    // static library's resource initializers only run if something in the
    // final executable actually touches them — the normal launcher/main.cpp
    // does this too. Without it, every built-in icon/widget theme reads as
    // invalid, which makes Application::createSetupWizard() think first-run
    // setup is needed and show a Qt Widgets wizard instead of ever calling
    // performMainStartupAction() (so app.status() never reaches Initialized).
    Q_INIT_RESOURCE(multimc);
    Q_INIT_RESOURCE(backgrounds);
    Q_INIT_RESOURCE(documents);
    Q_INIT_RESOURCE(prismlauncher);
    Q_INIT_RESOURCE(pe_dark);
    Q_INIT_RESOURCE(pe_light);
    Q_INIT_RESOURCE(pe_blue);
    Q_INIT_RESOURCE(pe_colored);
    Q_INIT_RESOURCE(breeze_dark);
    Q_INIT_RESOURCE(breeze_light);
    Q_INIT_RESOURCE(OSX);
    Q_INIT_RESOURCE(iOS);
    Q_INIT_RESOURCE(flat);
    Q_INIT_RESOURCE(flat_white);
    Q_INIT_RESOURCE(shaders);

    // Must be set before SDL_Init() — both are read once when the joystick
    // subsystem starts up.
    //
    // SDL defaults joystick/gamecontroller event delivery to OFF whenever
    // the app's window doesn't have SDL's own notion of input focus — a
    // sensible default for a normal desktop app sharing the screen with
    // others, but wrong for BigScreen: it's meant to be the sole,
    // exclusive fullscreen UI (launched via a Steam shortcut, gamescope,
    // or directly), and gamescope's nested-compositor focus handling is a
    // known source of SDL never seeing itself as "focused" even while
    // it's the only thing visible — silently dropping every single
    // controller event with no error, which looks exactly like "the
    // gamepad doesn't respond to buttons at all" while keyboard/mouse
    // (routed differently, through the window system rather than this
    // focus gate) can still work.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Steam Deck's (and other SteamOS handhelds') built-in controls need
    // this HIDAPI driver to show up as a normal SDL_GameController at all;
    // explicit rather than relying on SDL_HINT_JOYSTICK_HIDAPI's own
    // default in case a particular SDL2 build ships with it off.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // TEMPORARY diagnostic: logs which video driver SDL actually picked and
    // what else was available, to tell apart "picked the wrong backend
    // (e.g. fell back to the headless 'offscreen' one because no Wayland/
    // X11 session was reachable from wherever this was launched)" from "a
    // real driver was selected but its own EGL/GL init still failed" — the
    // literal error text alone doesn't distinguish these, and they need
    // very different fixes (an invocation/environment problem vs. a real
    // code bug). Remove once the real ARM EGL failure is understood.
    {
        SDL_Log("[video-diag] selected driver: %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
        const int numDrivers = SDL_GetNumVideoDrivers();
        for (int i = 0; i < numDrivers; ++i)
            SDL_Log("[video-diag] available driver %d: %s", i, SDL_GetVideoDriver(i));
    }

    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) {
                SDL_Log("Using gamepad: %s", SDL_GameControllerName(pad));
                break;
            }
        }
    }
    if (!pad)
        SDL_Log("No gamepad detected — keyboard arrow keys + Enter/Escape also drive ImGui nav.");
    ImGuiFullscreen::ReportGamepadLayout(DetectGamepadLayout(pad));

    // GLES3 (via EGL), not desktop Core GL — real ARM hardware confirmed
    // this the hard way: requesting SDL_GL_CONTEXT_PROFILE_CORE failed
    // outright with "Could not find a valid EGL device to initialize"
    // (SDL_CreateWindow itself never succeeds, so nothing renders at all —
    // not a missing-library problem as first suspected, a GL profile one).
    // Many ARM/embedded GPUs only expose GLES through EGL, no desktop GL/
    // GLX at all. Mesa (this dev machine's own driver, and what SteamOS
    // uses) supports GLES3+EGL on desktop GPUs too, so standardizing on
    // GLES3 everywhere — rather than a desktop/GLES runtime fallback,
    // which Dear ImGui's OpenGL3 backend can't do anyway (IMGUI_IMPL_OPENGL_ES3
    // selects its GL function loader and GLSL dialect at *compile* time,
    // see CMakeLists.txt) — is the simpler, more portable single code path.
    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    auto window_flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window =
        SDL_CreateWindow("PrismLauncher BigScreen", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, window_flags);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);  // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    if (!BigScreenGui::LoadFonts(1.0f)) {
        SDL_Log("Failed to load fonts");
        return 1;
    }

    SetTheme("Dark");
    if (!Initialize("images/placeholder.png")) {
        SDL_Log("ImGuiFullscreen::Initialize failed (missing placeholder texture?)");
        return 1;
    }
    // PCSX2's real ImGuiManager::Initialize() (not vendored) calls this so
    // the nav highlight is visible from frame one, before any input has
    // happened yet, rather than only appearing after the first mouse-vs-
    // gamepad input source switch.
    ForceKeyNavEnabled();

    // Bring up the real PrismLauncher core (settings, instances, accounts,
    // meta, network) exactly like the normal launcher does, but with
    // --no-window so MainWindow is never constructed. Qt args need real
    // storage for the app's lifetime.
    std::vector<char*> qtArgv(argv, argv + argc);
    char noWindowArg[] = "--no-window";
    qtArgv.push_back(noWindowArg);
    int qtArgc = static_cast<int>(qtArgv.size());
    Application app(qtArgc, qtArgv.data());
    if (app.status() == Application::Succeeded) {
        // Not an error: LocalPeer detected another already-running instance
        // (the normal Qt Widgets PrismLauncher, or another BigScreen
        // process) sharing this data directory, forwarded our arguments to
        // it, and that's the whole job here — exit quietly.
        SDL_Log("Another PrismLauncher instance is already running against this data directory; exiting.");
        return 0;
    }
    if (app.status() != Application::Initialized) {
        SDL_Log("PrismLauncher core failed to initialize (status=%d)", static_cast<int>(app.status()));
        return 1;
    }

    // Bridges LaunchController::showInstanceConsole() (see
    // BigScreenLaunchController's override) back into our own screen state
    // — needed because that file has no access to this anonymous-namespace
    // global otherwise.
    BigScreenLaunchController::onShowConsole = [](MinecraftInstance* inst) {
        g_consoleInstance = inst;
        SetScreen(Screen::Console);
    };

    // BigScreen-only settings (Settings > Appearance) — not real desktop
    // keys, so registered here rather than in the shared
    // Application::init() registerSetting() block.
    APPLICATION->settings()->registerSetting("BigScreenUIScale", 1.0);
    // Defaults to true (this is meant to run as a kiosk-style "Big Picture"
    // front-end, not a windowed app) — applied here rather than at
    // SDL_CreateWindow() above since reading the setting needs
    // APPLICATION->settings(), which needs Application constructed first,
    // which needs the SDL window to already exist (BigScreenGui pulls in
    // OpenGL symbols the Qt side also touches) — SDL_SetWindowFullscreen()
    // right after creation is visually identical to creating it fullscreen
    // outright, just very briefly windowed first.
    APPLICATION->settings()->registerSetting("BigScreenFullscreen", true);
    SDL_SetWindowFullscreen(window, APPLICATION->settings()->get("BigScreenFullscreen").toBool() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

    // TEMPORARY diagnostic: BIGSCREEN_AUTOLAUNCH=<instance id> triggers the
    // exact same launch codepath a real "A: Launch" press would, without
    // needing gamepad/keyboard input to reach it — for reproducing the
    // "launch crashes" report headlessly. Remove once that's root-caused.
    if (const char* autoLaunchId = std::getenv("BIGSCREEN_AUTOLAUNCH")) {
        QTimer::singleShot(500, &app, [id = QString::fromUtf8(autoLaunchId)]() {
            MinecraftInstance* inst = APPLICATION->instances()->getInstanceById(id);
            if (!inst) {
                SDL_Log("[autolaunch] no such instance: %s", qUtf8Printable(id));
                return;
            }
            SDL_Log("[autolaunch] launching %s", qUtf8Printable(id));
            APPLICATION->launch(inst, LaunchMode::Normal, nullptr, nullptr, QString(), &BigScreenLaunchController::create);
        });
    }

    // TEMPORARY diagnostic: BIGSCREEN_TEST_SCREEN=<landing|instances|
    // accounts|settings|instance_settings> jumps straight to that screen shortly after
    // startup, without needing input to navigate there — for visually
    // checking font/translation rendering via a screenshot. Remove once
    // that's confirmed.
    if (const char* screenName = std::getenv("BIGSCREEN_TEST_SCREEN")) {
        const std::string name = screenName;
        QTimer::singleShot(1500, &app, [name]() {
            if (name == "instances")
                SetScreen(Screen::Instances);
            else if (name == "accounts")
                SetScreen(Screen::Accounts);
            else if (name == "settings")
                SetScreen(Screen::Settings);
            else if (name == "quit")
                SetScreen(Screen::Quit);
            else if (name == "settings_window") {
                // Minecraft category (index 3), Window sub-tab (index 0) —
                // DrawSettingsWindow draws exactly one toggle
                // (LaunchMaximized), for reproducing "single-item sub-tab
                // doesn't want to select" headlessly.
                g_settingsTab = 3;
                g_settingsSubTab = 0;
                SetScreen(Screen::Settings);
            } else if (name == "settings_console") {
                // Minecraft category, Console sub-tab (index 1) — 4 items,
                // for comparison against settings_window's single-item case.
                g_settingsTab = 3;
                g_settingsSubTab = 1;
                SetScreen(Screen::Settings);
            } else if (name == "settings_appearance") {
                g_settingsTab = 2;
                g_settingsSubTab = 0;
                SetScreen(Screen::Settings);
            } else if (name == "settings_proxy") {
                g_settingsTab = 7;
                g_settingsSubTab = 0;
                SetScreen(Screen::Settings);
            } else if (name == "instance_settings" || name == "instance_settings_java" || name == "instance_settings_commands") {
                InstanceList* instances = APPLICATION->instances();
                if (instances->rowCount() > 0) {
                    g_instanceSettingsTarget = instances->at(0);
                    g_instanceSettingsTab = (name == "instance_settings_java") ? 1 : (name == "instance_settings_commands") ? 3 : 0;
                    SetScreen(Screen::InstanceSettings);
                }
            } else if (name == "instance_actions") {
                InstanceList* instances = APPLICATION->instances();
                if (instances->rowCount() > 0) {
                    SetScreen(Screen::Instances);
                    ShowInstanceActionsMenu(instances->at(0));
                }
            }
            SDL_Log("[test-screen] jumped to %s", name.c_str());
        });
        for (int i = 1; i <= 5; ++i) {
            QTimer::singleShot(1500 + i * 400, &app, [i]() {
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                SDL_Log("[test-screen] t+%dms: NavId=%u NavWindow=%s", i * 400, ctx->NavId,
                        ctx->NavWindow ? ctx->NavWindow->Name : "(null)");
            });
        }
    }

    bool done = false;

    // Qt owns the event loop (app.exec() below); this timer pumps SDL
    // events and renders one ImGui frame on every tick, so Qt's signal/slot
    // machinery keeps working exactly as it does in the normal Qt Widgets
    // build — no manual processEvents() calls, no second thread.
    //
    // Named (rather than a lambda passed straight to connect()) so it can
    // also be handed to BigScreenDialogs::PumpFrame below: BigScreenDialogs
    // and LaunchController::waitForTask (see BigScreenLaunchController)
    // both need to keep rendering while they block waiting on a dialog
    // answer or a background task, and this is the one function that knows
    // how to render a frame.
    std::function<void()> renderFrame = [&]() {
        // Run before anything ImGui-frame-related: a pending action may
        // itself call PumpFrame() (== this function) in a loop while it
        // blocks on a dialog, so it needs to start from a point with no
        // NewFrame()/Render() pair already open on the stack. See
        // g_pendingAction's comment.
        if (g_pendingAction) {
            auto action = std::move(g_pendingAction);
            g_pendingAction = nullptr;
            action();
        }

        // Gamepad input should only drive BigScreen while its own window
        // actually has WM focus — otherwise a button press meant for
        // whatever else is on screen (alt-tabbed away, a Steam overlay, a
        // different app) also gets processed here, which reads as BigScreen
        // "stealing" input in the background. Deliberately a *different*
        // signal from SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS above: that
        // hint controls whether SDL's joystick subsystem *delivers*
        // controller events at all (needed because its own internal focus
        // tracking was unreliable under gamescope — see the comment there),
        // while this checks the window's actual WM-reported focus before
        // *acting* on events that did arrive. The two don't conflict: under
        // gamescope, a dedicated BigScreen session is still the WM-focused
        // surface, so SDL_WINDOW_INPUT_FOCUS reads true there regardless of
        // whichever internal joystick quirk the other hint works around.
        const bool windowFocused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool isControllerEvent = event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP ||
                                            event.type == SDL_CONTROLLERAXISMOTION;
            if (!isControllerEvent || windowFocused)
                ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;

            // TEMPORARY diagnostic (see the BigScreen plan): logs every raw
            // SDL controller event so we can tell, from the log alone,
            // whether SDL is even receiving button presses at all — vs. the
            // events arriving fine but ImGui's nav not reacting to them.
            // Remove once gamepad nav is confirmed working end-to-end.
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                SDL_Log("[gamepad] button down: %d", event.cbutton.button);
            } else if (event.type == SDL_CONTROLLERAXISMOTION && std::abs(event.caxis.value) > 16000) {
                SDL_Log("[gamepad] axis %d moved: %d", event.caxis.axis, event.caxis.value);
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_Log("[gamepad] device added: index %d", event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                SDL_Log("[gamepad] device removed: instance %d", event.cdevice.which);
            }
        }

        UpdateLayoutScale();
        // UpdateLayoutScale() computed the scale that exactly fits BigScreen's
        // 1280x720 reference layout to the actual window — "Scale" in
        // Settings > Appearance (BigScreenUIScale) is a user multiplier on
        // top of that fit, applied here before UpdateFontScale() so medium/
        // large fonts (sized via LayoutScale(), same as every widget
        // dimension) pick it up too, not just the widget geometry.
        //
        // No padding recompute here (there used to be one, and it's the
        // reason this whole block used to be capped at 100% — see below):
        // g_layout_padding_left/top only ever mattered for
        // BeginFullscreenColumnWindow()/BeginFullscreenColumns() calls that
        // size themselves to the fixed 1280 reference width instead of the
        // real window — this file no longer has any of those (every such
        // call now passes 0.0f, meaning "to the real edge", relying on
        // BeginFullscreenColumnWindow's own documented end<=0 behavior —
        // see its first call site's comment). With every screen's content
        // column already dynamically matching the real window,
        // over-100% scale no longer pushes anything *past* the window's
        // own edge the way it used to (confirmed visually at 150%, no
        // clipping) — content can still visually crowd at extreme scale on
        // a small window (Landing/Quit's horizontal card rows aren't
        // wrap-aware), but that's a normal "zoomed in" tradeoff, not the
        // catastrophic edge-clipping this cap existed to prevent.
        {
            const float userScale = APPLICATION->settings()->get("BigScreenUIScale").toFloat();
            if (userScale > 0.0f && userScale != 1.0f) {
                ImGuiFullscreen::g_layout_scale *= userScale;
                ImGuiFullscreen::g_rcp_layout_scale = 1.0f / ImGuiFullscreen::g_layout_scale;
            }
        }
        UpdateFontScale();

        // Applies Settings > Appearance > Fullscreen live, the moment it's
        // toggled, rather than needing a restart — cheap to check every
        // frame (one settings lookup, one static comparison), and simpler
        // than threading a "this setting just changed" signal all the way
        // from DrawToggleSetting() back to here.
        {
            static bool lastAppliedFullscreen = APPLICATION->settings()->get("BigScreenFullscreen").toBool();
            const bool wantFullscreen = APPLICATION->settings()->get("BigScreenFullscreen").toBool();
            if (wantFullscreen != lastAppliedFullscreen) {
                lastAppliedFullscreen = wantFullscreen;
                SDL_SetWindowFullscreen(window, wantFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();

        // Left stick also moves menu focus, not just D-pad — many
        // controller users default to the stick. imgui_impl_sdl2 already
        // maps stick deflection to ImGuiKey_GamepadLStick{Up,Down,Left,
        // Right} (set by the UpdateGamepads() call inside NewFrame above),
        // but Dear ImGui's own nav system only treats those as a *scroll*
        // axis, not as equivalent to D-pad presses for moving between
        // items — so mirror stick-down onto the D-pad keys ourselves,
        // between NewFrame (so the stick state above is current) and
        // ImGui::NewFrame() (so nav sees it this frame).
        //
        // Reads the raw axis directly rather than using
        // ImGuiKey_GamepadLStick{Up,Down,Left,Right} — imgui_impl_sdl2's own
        // derivation of those uses an 8000/32767 (~24%) dead zone (SDL's own
        // suggested *minimum*), which read as "too sensitive"/triggering
        // from small or unintended stick movement. This uses a much larger
        // one instead, requiring a deliberate, firm push before nav moves
        // at all. Still purely additive on top of the real D-pad's own key
        // state (never explicitly clears these keys) — ImGui_ImplSDL2_NewFrame()
        // just above already re-polls and resets them to the real D-pad's
        // current state every frame, so once the stick returns inside the
        // dead zone this naturally stops re-asserting "down" the same way a
        // real D-pad release does — including the same built-in
        // hold-then-repeat pacing Dear ImGui's nav system already applies
        // to any held direction key, gamepad-native or (as here) synthetic,
        // so a held stick deflection repeats at the same rate a held D-pad
        // press would, with no separate repeat logic needed here.
        // Same reasoning as the controller-event gating above: this reads
        // the stick's raw current state directly rather than going through
        // SDL's event queue, so it needs its own explicit focus check —
        // otherwise stick-driven nav would keep working in the background
        // even while windowFocused gates every other controller input.
        if (pad && windowFocused) {
            ImGuiIO& io = ImGui::GetIO();
            constexpr Sint16 kStickDeadZone = 20000; // out of a max 32767 — a firm, deliberate push
            const Sint16 axisX = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            const Sint16 axisY = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            if (axisX < -kStickDeadZone)
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, true);
            else if (axisX > kStickDeadZone)
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true);
            if (axisY < -kStickDeadZone)
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, true);
            else if (axisY > kStickDeadZone)
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
        }

        ImGui::NewFrame();

        BeginLayout();

        // A BigScreenDialogs::Choose/InputString/Confirm dialog renders via
        // ImGui::BeginPopupModal (see DrawChoiceDialog() etc. in the
        // vendored toolkit) — a real ImGui modal, which already blocks
        // input from reaching whatever's behind it. So while one of those
        // is open, the normal screen underneath is safe to keep drawing
        // (and looks right — the dialog just overlays it, via EndLayout()'s
        // own DrawChoiceDialog()/DrawInputDialog() calls below, same as
        // always). It's only a *bare* Task wait (no dialog — e.g.
        // LaunchController::waitForTask, StartVanillaInstanceCreation's
        // download steps) that has nothing else guarding the screen
        // underneath, which is what DrawBlockingWait() below is actually
        // for — showing it while a dialog is already up was the "please
        // wait" the user reported: functionally harmless (the dialog still
        // worked, since BeginPopupModal doesn't care what's drawn behind
        // it) but confusing to look at.
        const bool dialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

        if (BigScreenDialogs::BlockingDepth > 0 && !dialogOpen && g_screen != Screen::Console) {
            // See DrawBlockingWait()'s comment — skip the normal screen (and
            // HandleBackButton(), so B can't do anything unexpected either)
            // while a bare Task-wait pump loop further down the call stack
            // is blocking on this same renderFrame being called repeatedly.
            // Console is exempt: it's how the user watches a launch's
            // progress live, and its only inline action (the "< Back"
            // MenuButton) just changes g_screen — harmless here, since this
            // same check means nothing renders that new screen until
            // blocking ends anyway.
            DrawBlockingWait();
        } else {
            switch (g_screen) {
                case Screen::Landing:
                    DrawLanding(done);
                    break;
                case Screen::Instances:
                    DrawInstances();
                    break;
                case Screen::Console:
                    DrawConsole();
                    break;
                case Screen::Accounts:
                    DrawAccounts();
                    break;
                case Screen::AccountLogin:
                    DrawAccountLogin();
                    break;
                case Screen::Settings:
                    DrawSettings();
                    break;
                case Screen::InstanceSettings:
                    DrawInstanceSettings();
                    break;
                case Screen::Quit:
                    DrawQuit(done);
                    break;
            }

            HandleBackButton();
        }
        if (g_wantsQuit)
            done = true;

        EndLayout();

        // TEMPORARY diagnostic (see the previous comment) — logs whenever
        // ImGui's nav focus actually moves, so we can tell whether the
        // problem is "SDL events not arriving" or "events arrive but ImGui
        // nav isn't moving" or "nav moves but isn't visible/doesn't confirm".
        {
            static ImGuiID lastNavId = 0;
            const ImGuiID navId = ImGui::GetCurrentContext()->NavId;
            if (navId != lastNavId) {
                SDL_Log("[gamepad] ImGui NavId changed: %u -> %u (source=%d)", lastNavId, navId,
                        static_cast<int>(ImGui::GetCurrentContext()->NavInputSource));
                lastNavId = navId;
            }
        }

        ImGui::Render();
        int display_w, display_h;
        SDL_GL_GetDrawableSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);

        if (done)
            app.quit();
    };

    // BigScreenDialogs (Settings' choice/text pickers) and
    // LaunchController::waitForTask (see BigScreenLaunchController) both
    // block by calling this same function in a loop until they're done —
    // see DialogHelpers.h for why that's safe here specifically (this app's
    // render loop being a QTimer tick rather than something driven directly
    // off app.exec(), unlike the normal Qt Widgets build).
    BigScreenDialogs::PumpFrame = renderFrame;

    QTimer frameTimer;
    frameTimer.setInterval(0);
    QObject::connect(&frameTimer, &QTimer::timeout, &app, renderFrame);
    frameTimer.start();

    const int exitCode = app.exec();

    Shutdown(true);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    if (pad)
        SDL_GameControllerClose(pad);
    SDL_Quit();

    return exitCode;
}
