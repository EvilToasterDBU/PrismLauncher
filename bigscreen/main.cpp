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
#include "InstanceCopyPrefs.h"
#include "InstanceCopyTask.h"
#include "InstanceImportTask.h"
#include "MMCZip.h"
#include "ResourceDownloadTask.h"
#include "archive/ExportToZipTask.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/Component.h"
#include "minecraft/PackProfile.h"
#include "minecraft/World.h"
#include "minecraft/WorldList.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "net/ApiRequest.h"
#include "net/NetJob.h"

#include <FileSystem.h>
#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
#include <sstream>
#include "minecraft/mod/DataPackFolderModel.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/Resource.h"
#include "minecraft/mod/ResourcePackFolderModel.h"
#include "minecraft/mod/ShaderPackFolderModel.h"
#include "minecraft/mod/TexturePackFolderModel.h"
#include "settings/SettingsObject.h"

#include <QColor>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <qrencode.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <unordered_map>
#include <unordered_set>

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

enum class Screen { Landing, Instances, Console, Accounts, AccountLogin, Settings, InstanceSettings, Quit, ModrinthBrowse };

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

// Whether the window should actually go fullscreen right now — the real
// "BigScreenFullscreen" setting (persisted to the user's real
// prismlauncher.cfg, defaults to true), UNLESS the BIGSCREEN_WINDOWED dev/
// test env var is set, in which case this always returns false. Never
// writes BIGSCREEN_WINDOWED's override back into the setting itself — a
// test run must never persist "windowed" into the user's real config and
// have it silently stick on their actual hardware afterward. Used both at
// startup (main()) and every frame's live-toggle check, so a test run
// can't be fought back into fullscreen by that per-frame check comparing
// against the (unmodified) real setting value.
bool WantFullscreen()
{
    if (qEnvironmentVariableIsSet("BIGSCREEN_WINDOWED"))
        return false;
    return APPLICATION->settings()->get("BigScreenFullscreen").toBool();
}

// Kept in sync with the vendored toolkit's own LAYOUT_TOP_BAR_HEIGHT
// (ImGuiFullscreen.h), which its dialog-centering code (DrawChoiceDialog()
// etc.) needs to know about too — see its comment.
constexpr float kTopBarHeight = ImGuiFullscreen::LAYOUT_TOP_BAR_HEIGHT;

// A single top-bar tab icon (see DrawTopBar's tabs parameter) — no text
// label, matching the reference (PCSX2's own settings screen shows category
// tabs as bare icons in the title bar, not a separate labeled row).
struct TopBarTab {
    const char* icon;
    bool active;
};

// Linux battery info via sysfs (/sys/class/power_supply/<name>/{type,
// capacity,status}) — no library needed, this is the same interface
// upower/acpi themselves read. Handheld/laptop-only: desktops typically
// have no entry with type=="Battery" at all, in which case this returns
// nullopt and the top bar simply omits the indicator, matching the user's
// own "if the device has one" ask. The scan for *which* power_supply entry
// is the battery (there can be several non-battery entries — AC adapters,
// USB-PD ports) only runs once; capacity/status are re-read periodically
// (sysfs reads are cheap, but no reason to do it every single frame).
struct BatteryInfo {
    int percent;
    bool charging;
};

std::optional<BatteryInfo> GetBatteryInfo()
{
    static bool scanned = false;
    static QString batteryDir;
    if (!scanned) {
        scanned = true;
        QDir powerSupply("/sys/class/power_supply");
        for (const QString& entry : powerSupply.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile typeFile(powerSupply.filePath(entry) + "/type");
            if (typeFile.open(QIODevice::ReadOnly) && typeFile.readAll().trimmed() == "Battery") {
                batteryDir = powerSupply.filePath(entry);
                break;
            }
        }
    }
    if (batteryDir.isEmpty())
        return std::nullopt;

    static std::optional<BatteryInfo> cached;
    static std::chrono::steady_clock::time_point lastRead;
    const auto now = std::chrono::steady_clock::now();
    if (cached && now - lastRead < std::chrono::seconds(5))
        return cached;
    lastRead = now;

    QFile capacityFile(batteryDir + "/capacity");
    if (!capacityFile.open(QIODevice::ReadOnly)) {
        cached.reset();
        return cached;
    }
    bool ok = false;
    const int percent = capacityFile.readAll().trimmed().toInt(&ok);
    if (!ok) {
        cached.reset();
        return cached;
    }

    bool charging = false;
    QFile statusFile(batteryDir + "/status");
    if (statusFile.open(QIODevice::ReadOnly)) {
        const QByteArray status = statusFile.readAll().trimmed();
        charging = (status == "Charging" || status == "Full");
    }

    cached = BatteryInfo{ std::clamp(percent, 0, 100), charging };
    return cached;
}

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

    // Right-hand content's *left edge* needs to be known before the title
    // is drawn, not after — a long title (instance name + category, e.g.
    // "Изменить… — automodpack — Screenshots") with enough tab icons
    // (10, at the high end now with Servers/Screenshots added) can run
    // right into the icon row otherwise, since AddText() has no width
    // limit of its own and nothing previously stopped it there. Confirmed
    // live via a real screenshot: title glyphs overlapping the Settings/
    // Mods tab icons. Clipped (via cpu_fine_clip_rect) rather than
    // measured-and-truncated-with-ellipsis — simpler, and good enough to
    // just stop the overlap; a title long enough to hit this only loses a
    // few trailing characters behind the icon row, still fully readable
    // via the sub-tab's own on-screen content.
    float titleClipRight = displaySize.x - padding;
    if (!tabs.empty()) {
        const float tabBoxSize = barHeight * 0.8f;
        const float tabGap = LayoutScale(6.0f);
        const float rowWidth = static_cast<float>(tabs.size()) * tabBoxSize + static_cast<float>(tabs.size() - 1) * tabGap;
        titleClipRight = displaySize.x - padding - rowWidth - LayoutScale(12.0f);
    } else {
        // Mirrors the battery/clock width computation below (kept in sync
        // by construction — same GetBatteryInfo()/QTime formatting) so
        // the clip boundary matches where that content will actually
        // start.
        float rightEdge = displaySize.x - padding;
        if (const std::optional<BatteryInfo> battery = GetBatteryInfo()) {
            char label[32];
            std::snprintf(label, sizeof(label), "%s%s %d%%", battery->charging ? ICON_FA_BOLT " " : "",
                          battery->percent >= 90   ? ICON_FA_BATTERY_FULL
                          : battery->percent >= 65 ? ICON_FA_BATTERY_THREE_QUARTERS
                          : battery->percent >= 35 ? ICON_FA_BATTERY_HALF
                          : battery->percent >= 10 ? ICON_FA_BATTERY_QUARTER
                                                    : ICON_FA_BATTERY_EMPTY,
                          battery->percent);
            rightEdge -= g_large_font.first->CalcTextSizeA(g_large_font.second, FLT_MAX, 0.0f, label).x + LayoutScale(20.0f);
        }
        const QByteArray timeUtf8 = QTime::currentTime().toString("HH:mm:ss").toUtf8();
        rightEdge -= g_large_font.first->CalcTextSizeA(g_large_font.second, FLT_MAX, 0.0f, timeUtf8.constData()).x;
        titleClipRight = rightEdge - LayoutScale(12.0f);
    }
    const ImVec4 titleClipRect(titlePos.x, 0.0f, std::max(titlePos.x, titleClipRight), barHeight);
    dl->AddText(g_large_font.first, g_large_font.second, titlePos, ImGui::GetColorU32(UIBackgroundTextColor), title, nullptr, 0.0f,
                &titleClipRect);

    if (tabs.empty()) {
        float rightEdge = displaySize.x - padding;

        // "clock -> battery" (outermost-right, clock just to its left) —
        // only shown when the device actually reports one via sysfs
        // (desktops typically don't, matching the "if it has one" ask).
        if (const std::optional<BatteryInfo> battery = GetBatteryInfo()) {
            const char* icon = battery->percent >= 90   ? ICON_FA_BATTERY_FULL
                                : battery->percent >= 65 ? ICON_FA_BATTERY_THREE_QUARTERS
                                : battery->percent >= 35 ? ICON_FA_BATTERY_HALF
                                : battery->percent >= 10 ? ICON_FA_BATTERY_QUARTER
                                                          : ICON_FA_BATTERY_EMPTY;
            char label[32];
            std::snprintf(label, sizeof(label), "%s%s %d%%", battery->charging ? ICON_FA_BOLT " " : "", icon, battery->percent);
            const ImVec2 batterySize = g_large_font.first->CalcTextSizeA(g_large_font.second, FLT_MAX, 0.0f, label);
            const ImVec2 batteryPos(rightEdge - batterySize.x, (barHeight - g_large_font.second) * 0.5f);
            dl->AddText(g_large_font.first, g_large_font.second, batteryPos, ImGui::GetColorU32(UIBackgroundTextColor), label);
            rightEdge -= batterySize.x + LayoutScale(20.0f);
        }

        const QString timeStr = QTime::currentTime().toString("HH:mm:ss");
        const QByteArray timeUtf8 = timeStr.toUtf8();
        const ImVec2 timeSize = g_large_font.first->CalcTextSizeA(g_large_font.second, FLT_MAX, 0.0f, timeUtf8.constData());
        const ImVec2 timePos(rightEdge - timeSize.x, (barHeight - g_large_font.second) * 0.5f);
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
    // Skipped entirely when rowWidth is 0 (an empty row, e.g. no instances
    // yet) — moving the cursor with SetCursorPosX() and then submitting no
    // item at all is exactly the case Dear ImGui's own debug tools flag
    // ("Code uses SetCursorPos()... Please submit an item e.g. Dummy()
    // afterwards"), reproduced live with an empty instances list.
    if (rowWidth > 0.0f)
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

// Instance Settings > Logs' own "list vs. viewer" state — see
// DrawInstanceSettingsLogs(). Empty g_selectedLogFile means "show the file
// list"; non-empty means "show g_selectedLogContent for this file".
QString g_selectedLogFile;
QString g_selectedLogContent;
MinecraftInstance* g_selectedLogInstance = nullptr;

// Instance Settings > Screenshots' own "list vs. viewer" state — same
// shape as the Logs globals above, texture instead of text.
// g_selectedScreenshotIndex is this file's position in the current
// GetInstanceScreenshots() listing — drives gallery-style Left/Right
// browsing inside the viewer (see OpenScreenshotViewer()); -1 means "not
// viewing anything" / "unknown position."
QString g_selectedScreenshotPath;
std::shared_ptr<GSTexture> g_selectedScreenshotTexture;
int g_selectedScreenshotIndex = -1;
MinecraftInstance* g_selectedScreenshotInstance = nullptr;

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

// Defined near DrawModrinthBrowse() (depends on g_modrinthBrowse, declared
// there) — forward-declared here since HandleBackButton() needs it and is
// defined earlier in the file.
Screen ModrinthBrowseBackTarget();
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
        case Screen::ModrinthBrowse:
            SetScreen(ModrinthBrowseBackTarget());
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
            // BigScreenDialogs::WaitForTask() (see its comment) fills these
            // in from the waited-on Task's own getStatus()/getProgress()/
            // getTotalProgress() every pump iteration — the same API the
            // desktop's own ProgressDialog reads, so a real launch's
            // "Executing N task(s) (X out of Y are done)" status text (from
            // ConcurrentTask::updateState(), used for library/asset
            // downloads) and its progress show up here too, instead of a
            // bare static message. Empty/zero when no task is being waited
            // on (e.g. a plain dialog answer via Choose/InputString/Confirm
            // — those don't drive a Task at all), so this falls back to the
            // same plain text as before.
            const bool hasTask = !BigScreenDialogs::CurrentTaskStatus.isEmpty();
            const QByteArray statusUtf8 = hasTask ? BigScreenDialogs::CurrentTaskStatus.toUtf8() : QByteArray("Please wait...");
            const char* text = statusUtf8.constData();

            const float barWidth = LayoutScale(600.0f);
            const float barHeight = LayoutScale(24.0f);
            const bool hasProgress = hasTask && BigScreenDialogs::CurrentTaskTotalProgress > 0;
            const float blockHeight =
                ImGui::GetTextLineHeight() + (hasProgress ? (LayoutScale(12.0f) + barHeight) : 0.0f);

            ImVec2 textSize = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - textSize.x) * 0.5f, (ImGui::GetWindowHeight() - blockHeight) * 0.5f));
            ImGui::TextUnformatted(text);

            if (hasProgress) {
                const float fraction = std::clamp(
                    static_cast<float>(BigScreenDialogs::CurrentTaskProgress) / static_cast<float>(BigScreenDialogs::CurrentTaskTotalProgress),
                    0.0f, 1.0f);
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(fraction * 100.0f));

                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(12.0f));
                ImGui::ProgressBar(fraction, ImVec2(barWidth, barHeight), overlay);
            }
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

    // Not an "override" gate like the others above (no matching
    // OverrideXxx flag — registerSetting("GlobalDataPacksEnabled", false)
    // is a plain per-instance setting) — this is what makes the "Data
    // Packs" top-level tab (BuildInstanceTopTabs()) appear at all:
    // dataPackList() returns nullptr unless this is on, matching
    // GlobalDataPackPage::shouldDisplay() exactly. Off by default, so the
    // tab is normally hidden — this toggle (plus the path field, reused
    // verbatim from MinecraftSettingsWidget.ui) is the only way to turn it
    // on from BigScreen.
    DrawToggleSetting("GlobalDataPacksEnabled", StripMnemonic(TR("MinecraftSettingsWidget", "&Global Data Packs")),
                       TR("MinecraftSettingsWidget", "Allows installing data packs across all worlds if an applicable mod is installed.\n"
                                                      "It is most likely you will need to change the path - please refer to the mod's website."),
                       false, s);
    if (s->get("GlobalDataPacksEnabled").toBool())
        DrawTextSetting("GlobalDataPacksPath", TR("MinecraftSettingsWidget", "Folder Path"),
                         "Path to the shared data packs folder, relative to the instance's .minecraft directory.", false, s);
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

// X on a focused resource (mod / resource pack / texture pack / shader
// pack — every one of these is a ResourceFolderModel under the hood, see
// DrawResourceFolderList() below) opens this — matches
// ShowInstanceActionsMenu()'s own shape exactly, including the same reason
// its "Delete" action defers through g_pendingAction rather than calling
// BigScreenDialogs::Confirm directly (this runs from inside
// DrawChoiceDialog()'s own callback via OpenChoiceDialog, which is
// mid-frame; Confirm()'s blocking pump loop can't start from there).
// "Check for Updates" still has no implementation (needs a real
// update-checking task graph — a substantially larger, separate piece of
// work) — shown as an OpenInfoMessageDialog placeholder rather than
// silently doing nothing or being left out of the menu, so it's clear
// it's planned, not forgotten. "Download ..." opens StartResourceBrowse()
// (defined near DrawModrinthBrowse(), forward-declared below) — the same
// Modrinth browse screen StartModrinthBrowse() uses for whole-modpack
// installs, here targeting this one resource type + the current
// instance's already-existing folder instead. confirmDeleteTitle/
// checkForUpdatesText/downloadText are passed in by the caller rather
// than hardcoded here, since the real desktop source strings differ by
// resource type (Mods reuses ModFolderPage's own bespoke overrides;
// Resource/Texture/Shader/Data Packs reuse the shared
// ExternalResourcesPage base text instead — see each
// DrawInstanceSettingsXxx() wrapper below for exactly which).
void StartResourceBrowse(MinecraftInstance* inst, ResourceFolderModel* model, ModPlatform::ResourceType type, const QString& screenTitle);
void ShowResourceActionsMenu(ResourceFolderModel* model,
                              ModPlatform::ResourceType resourceType,
                              const QString& resourceName,
                              const QString& fileName,
                              const QString& confirmDeleteTitle,
                              const QString& checkForUpdatesText,
                              const QString& downloadText)
{
    ChoiceDialogOptions options;
    options.emplace_back(StripMnemonic(MW("Dele&te")).toStdString(), false);
    options.emplace_back(checkForUpdatesText.toStdString(), false);
    options.emplace_back(downloadText.toStdString(), false);

    OpenChoiceDialog(
        resourceName.toStdString(), false, std::move(options),
        [model, resourceType, resourceName, fileName, confirmDeleteTitle, checkForUpdatesText, downloadText](s32 index,
                                                                                                                const std::string&, bool) {
            if (index < 0)
                return;
            g_pendingAction = [model, resourceType, resourceName, fileName, index, confirmDeleteTitle, checkForUpdatesText,
                                downloadText]() {
                CloseChoiceDialog();
                switch (index) {
                    case 0: {
                        // BigScreen always confirms destructive gamepad
                        // actions regardless of running state — the desktop
                        // side only confirms deletion while the instance is
                        // actually running (a crash-risk warning, see
                        // ExternalResourcesPage::removeItems()), and skips
                        // any prompt at all otherwise, but that asymmetry
                        // doesn't fit a controller-only UI, so this message
                        // is BigScreen's own wording. The *title* does reuse
                        // the real desktop string the caller passed in
                        // though.
                        const QString message =
                            QString("Are you sure you want to delete \"%1\"?\nThis cannot be undone.").arg(resourceName);
                        if (BigScreenDialogs::Confirm(confirmDeleteTitle.toStdString(), message.toStdString(), false,
                                                       StripMnemonic(MW("Dele&te")).toStdString(),
                                                       TR("LaunchController", "Cancel").toStdString()))
                            model->uninstallResource(fileName);
                        break;
                    }
                    case 1:
                        // No real equivalent for this exact status sentence
                        // — BigScreen-only, describing a BigScreen-specific
                        // gap.
                        OpenInfoMessageDialog(checkForUpdatesText.toStdString(), "Checking for updates isn't implemented yet.");
                        break;
                    case 2:
                        StartResourceBrowse(g_instanceSettingsTarget, model, resourceType, downloadText);
                        break;
                }
            };
        });
}

// Shared by every "list of installed X, toggle to enable/disable, X opens
// the actions menu" tab (Mods/Resource Packs/Texture Packs/Shader Packs) —
// all four of the underlying model types (ModFolderModel,
// ResourcePackFolderModel, TexturePackFolderModel, ShaderPackFolderModel)
// are plain ResourceFolderModel subclasses with no additional API this
// screen needs, confirmed by reading each header directly rather than
// assumed from the similar page names. Matches explicit feedback that
// destructive/heavier actions belong in a popup menu (X), not as another
// inline control next to the toggle. The model's *List() getter on
// MinecraftInstance lazily creates it but doesn't populate it — matches
// each desktop page's own on-open update() call — so this triggers one
// update() the first time a given model pointer is seen (comparing against
// the previous call's pointer; harmless to re-trigger on every tab switch
// since update() is designed to be called repeatedly, same as the desktop
// page does every time it's opened), then just re-polls size()/at() every
// frame the same way every other BigScreen list already does (Instances,
// Accounts, ...) — no signal wiring needed, each model's own
// QFileSystemWatcher keeps it current in the background regardless of
// whether its tab is being drawn.
void DrawResourceFolderList(ResourceFolderModel* model,
                             ModPlatform::ResourceType resourceType,
                             const char* emptyText,
                             const QString& confirmDeleteTitle,
                             const QString& checkForUpdatesText,
                             const QString& downloadText)
{
    static ResourceFolderModel* lastModel = nullptr;
    if (model != lastModel) {
        model->update();
        lastModel = model;
    }

    const int count = static_cast<int>(model->size());
    if (count == 0) {
        ImGui::TextUnformatted(emptyText);
        return;
    }

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

    for (int i = 0; i < count; ++i) {
        Resource& resource = model->at(i);
        bool enabled = resource.enabled();
        const QByteArray nameUtf8 = resource.name().toUtf8();

        // PushID(i): ImGuiFullscreen's widgets derive their ImGui ID from
        // the title text alone (see MenuButtonFrame() in the vendored
        // toolkit) — two resources with the same display name (confirmed
        // live: a real instance had two shader packs both named
        // "LethalRudimentary") would otherwise collide onto the same ID,
        // which Dear ImGui flags with its own "2 visible items with
        // conflicting ID" debug overlay and — worse — makes both rows
        // respond to input as if they were one. Scoping by loop index
        // guarantees uniqueness regardless of what's on disk.
        ImGui::PushID(i);
        if (ToggleButton(nameUtf8.constData(), nullptr, &enabled)) {
            const QModelIndexList indexes = { model->index(i, 0) };
            model->setResourceEnabled(indexes, enabled ? EnableAction::ENABLE : EnableAction::DISABLE);
        }

        if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
            // ResourceFolderModel::uninstallResource() (see its own
            // implementation) matches against the *base* filename, and
            // itself strips a trailing ".disabled" — the suffix
            // PrismLauncher appends on disk when a resource is toggled off
            // — before comparing, but only from its own internal copy, not
            // from whatever the caller passes in. resource.fileinfo().
            // fileName() reports the file exactly as it sits on disk right
            // now, so for a currently-disabled resource that's already
            // ".disabled"-suffixed, passing it through unstripped never
            // matches and the delete silently no-ops (confirmed live for
            // Mods specifically — same underlying model type, so the same
            // fix applies uniformly here).
            QString fileName = resource.fileinfo().fileName();
            if (!resource.enabled() && fileName.endsWith(".disabled"))
                fileName.chop(9);
            ShowResourceActionsMenu(model, resourceType, resource.name(), fileName, confirmDeleteTitle, checkForUpdatesText,
                                     downloadText);
        }
        ImGui::PopID();
    }
}

void DrawInstanceSettingsMods()
{
    // "Check for Updates"/"Download Mods" reuse ModFolderPage's own real
    // action text verbatim (ModFolderPage.cpp: updateMenu->addAction(tr(
    // "Check for Updates")); ui->actionDownloadItem->setText(tr("Download
    // Mods"));), and its delete-confirmation title reuses ModFolderPage's
    // own override of removeItems() (tr("Confirm Delete")) — distinct from
    // MainWindow's "Confirm Deletion" (used for whole-instance delete
    // elsewhere in this file), a different real string for a different
    // operation.
    DrawResourceFolderList(g_instanceSettingsTarget->loaderModList(), ModPlatform::ResourceType::Mod, "No mods installed.",
                            TR("ModFolderPage", "Confirm Delete"), TR("ModFolderPage", "Check for Updates"),
                            TR("ModFolderPage", "Download Mods"));
}

// Resource/Texture/Shader Packs don't override removeItems() on the
// desktop side the way ModFolderPage does (confirmed — grepped each page's
// .cpp for it, found nothing), so their real "Confirm Delete" title and
// "Check for &Updates" action text come from the shared
// ExternalResourcesPage base class instead (ExternalResourcesPage.cpp's
// removeItems(), and ExternalResourcesPage.ui's actionUpdateItem — the
// context for a Designer-form action's default text is the form's own
// <class>, i.e. "ExternalResourcesPage", not whichever subclass happens to
// use it unchanged). Each page's "Download Packs" text *is* its own
// override though (every one of the three sets it explicitly in its
// constructor), so that one is looked up per-type.
void DrawInstanceSettingsResourcePacks()
{
    DrawResourceFolderList(g_instanceSettingsTarget->resourcePackList(), ModPlatform::ResourceType::ResourcePack,
                            "No resource packs installed.", TR("ExternalResourcesPage", "Confirm Delete"),
                            StripMnemonic(TR("ExternalResourcesPage", "Check for &Updates")), TR("ResourcePackPage", "Download Packs"));
}

// Texture Packs search under ResourceType::ResourcePack too, not a
// separate "TexturePack" type — confirmed by reading both
// ModrinthAPI.cpp's g_resourceTypeMap (no TexturePack entry — Modrinth's
// own taxonomy files legacy texture packs under the same "resourcepack"
// project type as modern resource packs) and the desktop's own
// TexturePackResourceModel (ui/pages/modplatform/TexturePackModel.cpp),
// which subclasses ResourcePackResourceModel and only adds legacy-MC-
// version filtering on top of its search — never introduces its own
// resource type. The install *target* is still texturePackList()
// (the correct legacy-format instance folder), just the browse/search
// type that differs from the folder name.
void DrawInstanceSettingsTexturePacks()
{
    DrawResourceFolderList(g_instanceSettingsTarget->texturePackList(), ModPlatform::ResourceType::ResourcePack,
                            "No texture packs installed.", TR("ExternalResourcesPage", "Confirm Delete"),
                            StripMnemonic(TR("ExternalResourcesPage", "Check for &Updates")), TR("TexturePackPage", "Download Packs"));
}

void DrawInstanceSettingsShaderPacks()
{
    DrawResourceFolderList(g_instanceSettingsTarget->shaderPackList(), ModPlatform::ResourceType::ShaderPack,
                            "No shader packs installed.", TR("ExternalResourcesPage", "Confirm Delete"),
                            StripMnemonic(TR("ExternalResourcesPage", "Check for &Updates")), TR("ShaderPackPage", "Download Packs"));
}

// Same shape/reasoning as Resource/Texture/Shader Packs above — confirmed
// DataPackPage.cpp doesn't override removeItems() either, so it's the same
// ExternalResourcesPage base text for the delete title and update action.
void DrawInstanceSettingsDataPacks()
{
    DrawResourceFolderList(g_instanceSettingsTarget->dataPackList(), ModPlatform::ResourceType::DataPack, "No data packs installed.",
                            TR("ExternalResourcesPage", "Confirm Delete"), StripMnemonic(TR("ExternalResourcesPage", "Check for &Updates")),
                            TR("DataPackPage", "Download Packs"));
}

// WorldList (launcher/minecraft/WorldList.h) isn't a ResourceFolderModel —
// no enable/disable toggle, no "Check for Updates"/"Download" concept — so
// this doesn't reuse DrawResourceFolderList()/ShowResourceActionsMenu().
// X on a focused world opens Delete/Rename/Copy, all real strings from
// WorldListPage.cpp (context "WorldListPage" throughout — that page owns
// these directly, no shared base class the way the resource pages did).
// Reset Icon and View Folder aren't ported (lower value, no clear gamepad-
// friendly way to preview an icon file picker without a working file
// selector round-trip — deferred like the rest of what this pass skips).
void ShowWorldActionsMenu(WorldList* worlds, int index)
{
    if (index < 0 || index >= static_cast<int>(worlds->size()))
        return;
    const World& targetWorld = (*worlds)[static_cast<size_t>(index)];
    // Same empty-LevelName fallback as DrawInstanceSettingsWorlds() — see
    // its comment for why this can legitimately happen on real disk.
    const QString worldName = targetWorld.name().isEmpty() ? targetWorld.folderName() : targetWorld.name();

    ChoiceDialogOptions options;
    options.emplace_back(StripMnemonic(MW("Dele&te")).toStdString(), false);
    options.emplace_back(TR("WorldListPage", "Rename World").toStdString(), false);
    options.emplace_back(TR("WorldListPage", "Copy World").toStdString(), false);

    OpenChoiceDialog(worldName.toStdString(), false, std::move(options), [worlds, index, worldName](s32 choice, const std::string&, bool) {
        if (choice < 0)
            return;
        g_pendingAction = [worlds, index, worldName, choice]() {
            CloseChoiceDialog();
            // The world list could have changed (deleted elsewhere,
            // watcher refresh) between opening this menu and the action
            // actually running a frame later — re-validate rather than
            // trusting the captured index blindly.
            if (index < 0 || index >= static_cast<int>(worlds->size()))
                return;
            switch (choice) {
                case 0: {  // Delete
                    const QString message = TR("WorldListPage",
                                                "You are about to delete \"%1\".\n"
                                                "The world may be gone forever (A LONG TIME).\n\n"
                                                "Are you sure?")
                                                 .arg(worldName);
                    if (BigScreenDialogs::Confirm(TR("WorldListPage", "Confirm Deletion").toStdString(), message.toStdString(), false,
                                                   StripMnemonic(MW("Dele&te")).toStdString(),
                                                   TR("LaunchController", "Cancel").toStdString())) {
                        // stopWatching()/startWatching() around the task
                        // matches WorldListPage::on_actionRemove_triggered()
                        // exactly — avoids the QFileSystemWatcher racing the
                        // task's own filesystem operations on the same
                        // directory.
                        worlds->stopWatching();
                        std::unique_ptr<Task> task = worlds->createDeleteWorldTask(index);
                        if (task) {
                            task->start();
                            BigScreenDialogs::WaitForTask(task.get());
                        }
                        worlds->startWatching();
                    }
                    break;
                }
                case 1: {  // Rename
                    const auto newName =
                        BigScreenDialogs::InputString(TR("WorldListPage", "World name").toStdString(),
                                                       TR("WorldListPage", "Enter a new world name.").toStdString(), worldName.toStdString());
                    if (newName && !newName->isEmpty() && index < static_cast<int>(worlds->size()))
                        (*worlds)[static_cast<size_t>(index)].rename(*newName);
                    break;
                }
                case 2: {  // Copy
                    const auto copyName = BigScreenDialogs::InputString(
                        TR("WorldListPage", "World name").toStdString(), TR("WorldListPage", "Enter a new name for the copy.").toStdString(),
                        worldName.toStdString());
                    if (copyName && !copyName->isEmpty()) {
                        worlds->stopWatching();
                        std::unique_ptr<Task> task = worlds->createCopyWorldTask(index, *copyName);
                        if (task) {
                            task->start();
                            BigScreenDialogs::WaitForTask(task.get());
                        }
                        worlds->startWatching();
                    }
                    break;
                }
            }
        };
    });
}

// worldList() lazily creates the model but doesn't populate it —
// startWatching() (WorldList.cpp) both begins the QFileSystemWatcher *and*
// triggers the initial update(), matching WorldListPage's own
// openedImpl(). Never calls stopWatching() when leaving this tab (same
// choice as DrawResourceFolderList() makes for its own models) — a bit of
// background filesystem watching left running is harmless, and simpler
// than threading a "tab closed" event through this immediate-mode screen
// stack.
void DrawInstanceSettingsWorlds()
{
    WorldList* worlds = g_instanceSettingsTarget->worldList();

    static WorldList* lastWorldList = nullptr;
    if (worlds != lastWorldList) {
        worlds->startWatching();
        lastWorldList = worlds;
    }

    const int count = static_cast<int>(worlds->size());
    if (count == 0) {
        ImGui::TextUnformatted("No worlds yet.");
        return;
    }

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();
    const QLocale locale;

    for (int i = 0; i < count; ++i) {
        const World& world = (*worlds)[static_cast<size_t>(i)];
        // World::name() falls back to the folder name only when the
        // level.dat "LevelName" NBT tag is entirely absent (World.cpp) —
        // not when the tag exists but holds an empty string, which does
        // happen on real disk (confirmed live: one of this instance's
        // real worlds hit exactly this). An empty title crashes
        // MenuButton() — ImGuiFullscreen's MenuButtonFrame() computes its
        // ID from the title text, and an empty string at the root of this
        // child window hashes to the same ID as the window itself,
        // tripping ImGui's own "Cannot have an empty ID at the root of a
        // window" assertion (confirmed via gdb backtrace). Falling back to
        // folderName() here replicates the same "never blank" intent the
        // constructor's own fallback has, for the case it doesn't cover.
        const QString displayName = world.name().isEmpty() ? world.folderName() : world.name();
        const QByteArray nameUtf8 = displayName.toUtf8();
        const QString summary = QString("%1  \xE2\x80\xA2  %2  \xE2\x80\xA2  %3")
                                     .arg(world.gameType().toTranslatedString())
                                     .arg(locale.toString(world.lastPlayed(), QLocale::ShortFormat))
                                     .arg(locale.formattedDataSize(world.bytes()));
        const QByteArray summaryUtf8 = summary.toUtf8();

        // PushID(i): same reasoning as DrawResourceFolderList() — two
        // worlds can share a display name (e.g. two "New World"s), which
        // would otherwise collide onto the same ImGui ID.
        ImGui::PushID(i);
        MenuButton(nameUtf8.constData(), summaryUtf8.constData());

        if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
            ShowWorldActionsMenu(worlds, i);
        ImGui::PopID();
    }
}

// Same file set the desktop's OtherLogsPage::getPaths() builds (crash-
// reports/, logs/, and the instance root itself — MinecraftInstance::
// getLogFileSearchPaths(), a real public API, reused directly rather than
// duplicated) and the same filters (*.log/*.log.gz always; *.txt too for
// search paths other than the instance root itself, matching that
// function's own "searchPath != m_basePath" check) — paths returned
// relative to gameRoot(), newest-first (QDir::SortFlag::Time).
std::vector<QString> GetInstanceLogFiles(MinecraftInstance* inst)
{
    const QDir baseDir(inst->gameRoot());
    std::vector<QString> result;
    for (const QString& searchPath : inst->getLogFileSearchPaths()) {
        const QDir searchDir(searchPath);
        QStringList filters{ "*.log", "*.log.gz" };
        if (searchPath != inst->gameRoot())
            filters.append("*.txt");
        const QStringList entries = searchDir.entryList(filters, QDir::Files | QDir::Readable, QDir::SortFlag::Time);
        for (const QString& name : entries)
            result.push_back(baseDir.relativeFilePath(searchDir.filePath(name)));
    }
    return result;
}

// A two-state screen within one sub-tab (list of log files, or the
// selected file's content) — the first tab in Instance Settings that
// needs a "drill into an item" step rather than a flat list + X-menu. B
// still exits the whole Instance Settings screen from either state
// (HandleBackButton()'s Screen::InstanceSettings case doesn't know about
// this nested state — same as it doesn't for any other tab), so the "<
// Back" row below is the only way to return to the file list without
// leaving Edit Instance entirely; not ideal, but consistent with how B
// already behaves everywhere else in this screen (always a full exit),
// not a new inconsistency.
//
// v1 gap, deliberately not ported: Delete/Copy-to-clipboard/Paste-to-
// mclo.gs (OtherLogsPage's on_btnDelete/on_btnCopy/on_btnPaste) and
// decompressing *.log.gz (would need zlib/libarchive wired up for this one
// screen) — view-only for now, most useful case (checking a recent crash
// report on real hardware) doesn't need either.
void DrawInstanceSettingsLogs()
{
    MinecraftInstance* inst = g_instanceSettingsTarget;

    // Reset if the target instance changed (e.g. Y/X back out to
    // Instances, edit a different instance, come back to Logs) — leftover
    // content from a different instance's log would otherwise show
    // briefly. Switching tabs and back within the *same* instance
    // deliberately keeps the viewer open where the user left it. A global
    // (not a function-local static) so external code — namely the
    // BIGSCREEN_TEST_SCREEN diagnostic — can pre-populate the viewer state
    // and have it survive this function's own first call.
    if (inst != g_selectedLogInstance) {
        g_selectedLogFile.clear();
        g_selectedLogContent.clear();
        g_selectedLogInstance = inst;
    }

    if (!g_selectedLogFile.isEmpty()) {
        if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Back")) {
            g_selectedLogFile.clear();
            g_selectedLogContent.clear();
            return;
        }
        ImGui::BeginChild("log_viewer", ImVec2(0.0f, 0.0f), true);
        if (g_selectedLogContent.isEmpty()) {
            ImGui::TextUnformatted("(empty file)");
        } else {
            const QStringList lines = g_selectedLogContent.split('\n');
            for (const QString& line : lines) {
                const QByteArray lineUtf8 = line.toUtf8();
                ImGui::TextUnformatted(lineUtf8.constData());
            }
        }
        ImGui::EndChild();
        return;
    }

    const std::vector<QString> files = GetInstanceLogFiles(inst);
    if (files.empty()) {
        ImGui::TextUnformatted("No log files found.");
        return;
    }

    const QDir baseDir(inst->gameRoot());
    const QLocale locale;
    for (size_t i = 0; i < files.size(); ++i) {
        const QString& relPath = files[i];
        const QFileInfo fi(baseDir.filePath(relPath));
        const QString summary =
            locale.toString(fi.lastModified(), QLocale::ShortFormat) + "  \xE2\x80\xA2  " + locale.formattedDataSize(fi.size());
        const QByteArray nameUtf8 = relPath.toUtf8();
        const QByteArray summaryUtf8 = summary.toUtf8();

        ImGui::PushID(static_cast<int>(i));
        if (MenuButton(nameUtf8.constData(), summaryUtf8.constData())) {
            if (relPath.endsWith(".gz")) {
                g_selectedLogContent = "This log is compressed (.gz) — viewing compressed logs isn't supported yet.";
            } else {
                QFile file(baseDir.filePath(relPath));
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    g_selectedLogContent = QString::fromUtf8(file.readAll());
                    file.close();
                } else {
                    g_selectedLogContent = "Failed to open this log file.";
                }
            }
            g_selectedLogFile = relPath;
        }
        ImGui::PopID();
    }
}

// The actual version-change work — reachable only via g_pendingAction (see
// ShowVersionComponentActionsMenu() below), so BigScreenDialogs::* and
// WaitForTask are safe to call directly here, same reasoning as
// StartVanillaInstanceCreation(). Generic over any component (Minecraft,
// a mod loader, LWJGL, intermediary mappings, ...) — for anything other
// than net.minecraft itself, candidates are filtered to versions
// compatible with the currently-installed Minecraft version, the same way
// the desktop's VersionSelectDialog does via
// setExactIfPresentFilter(BaseVersionList::ParentVersionRole, ...)
// (VersionPage.cpp:399): Meta::Version::requiredSet() carries the same
// "requires net.minecraft == X" data that role reads from, already
// populated by the lightweight version-*list* load alone (confirmed live
// — no per-candidate full load needed just to filter). Skipping this
// filter would let a gamepad user pick a loader version flat-out
// incompatible with the installed Minecraft version and silently break
// the instance. Release-only for net.minecraft specifically (same
// scoping choice StartVanillaInstanceCreation() already made — snapshot/
// old_beta/old_alpha filtering is real desktop-side UI surface, not just
// a missing checkbox); every other component's version list has no
// release/snapshot concept of its own, so no such filter applies there.
void ChangeComponentVersion(ComponentPtr comp)
{
    MinecraftInstance* inst = g_instanceSettingsTarget;
    PackProfile* profile = inst->getPackProfile();
    const QString uid = comp->getID();
    const bool isMinecraft = uid == "net.minecraft";
    const QString currentMcVersion = profile->getComponentVersion("net.minecraft");

    const std::string dialogTitle = TR("VersionPage", "Change Version").toStdString();

    Meta::VersionList::Ptr versionList = isMinecraft ? APPLICATION->metadataIndex()->get("net.minecraft") : comp->getVersionList();
    if (!versionList) {
        BigScreenDialogs::Confirm(dialogTitle, "No version list is available for this component.", false, "OK", "OK");
        return;
    }
    if (!versionList->isLoaded()) {
        Task::Ptr loadTask = versionList->getLoadTask();
        loadTask->start();
        BigScreenDialogs::WaitForTask(loadTask.get());
        if (!versionList->isLoaded()) {
            BigScreenDialogs::Confirm(dialogTitle, "Failed to load the version list. Check your internet connection.", false, "OK", "OK");
            return;
        }
    }

    std::vector<std::string> labels;
    std::vector<Meta::Version::Ptr> candidates;
    for (int i = 0; i < versionList->count(); ++i) {
        Meta::Version::Ptr version = std::dynamic_pointer_cast<Meta::Version>(versionList->at(i));
        if (!version)
            continue;
        if (isMinecraft) {
            if (version->typeString() != "release")
                continue;
        } else {
            // A version with no declared net.minecraft requirement at all
            // is treated as "compatible with anything" (matches the
            // desktop filter's own exact-if-present semantics — an empty
            // filter value doesn't exclude).
            const auto& reqs = version->requiredSet();
            const auto it = std::find_if(reqs.begin(), reqs.end(), [](const Meta::Require& req) { return req.uid == "net.minecraft"; });
            if (it != reqs.end() && it->equalsVersion != currentMcVersion)
                continue;
        }
        candidates.push_back(version);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Meta::Version::Ptr& a, const Meta::Version::Ptr& b) { return a->rawTime() > b->rawTime(); });
    for (const Meta::Version::Ptr& version : candidates)
        labels.push_back(version->descriptor().toStdString());
    if (labels.empty()) {
        BigScreenDialogs::Confirm(dialogTitle,
                                   isMinecraft ? "No release versions available."
                                               : ("No versions compatible with Minecraft " + currentMcVersion.toStdString() +
                                                  " are available for this component."),
                                   false, "OK", "OK");
        return;
    }

    const auto versionChoice = BigScreenDialogs::Choose(dialogTitle, labels);
    if (!versionChoice || *versionChoice < 0 || static_cast<size_t>(*versionChoice) >= candidates.size())
        return;
    Meta::Version::Ptr chosenVersion = candidates[static_cast<size_t>(*versionChoice)];

    if (isMinecraft) {
        // Same AutomaticJavaSwitch/OverrideJavaLocation reset the desktop
        // does right before a Minecraft version change (VersionPage.cpp)
        // — an instance-specific pinned Java install can be wrong for a
        // very different Minecraft version (e.g. Java 8 pinned for 1.8,
        // now switching to 1.21 which needs Java 21); clearing the
        // override lets automatic Java selection pick a compatible one
        // again.
        if (APPLICATION->settings()->get("AutomaticJavaSwitch").toBool() && inst->settings()->get("AutomaticJava").toBool() &&
            inst->settings()->get("OverrideJavaLocation").toBool()) {
            inst->settings()->set("OverrideJavaLocation", false);
            inst->settings()->set("JavaPath", "");
        }
    }

    // important=true only for net.minecraft, matching VersionPage.cpp's
    // own `bool important = false; if (uid == "net.minecraft") important =
    // true;` exactly.
    profile->setComponentVersion(uid, chosenVersion->descriptor(), isMinecraft);
    profile->resolve(Net::Mode::Online);
}

// X on any component row opens this — Minecraft and every loader/library
// component alike now (see ChangeComponentVersion()'s comment). Uses the
// raw non-blocking OpenChoiceDialog directly, same reasoning as every
// other X-menu in this file: called from inside
// DrawInstanceSettingsVersion(), itself mid-frame.
void ShowVersionComponentActionsMenu(ComponentPtr comp)
{
    ChoiceDialogOptions options;
    options.emplace_back(TR("VersionPage", "Change Version").toStdString(), false);

    OpenChoiceDialog(comp->getName().toStdString(), false, std::move(options), [comp](s32 index, const std::string&, bool) {
        if (index != 0)
            return;
        g_pendingAction = [comp]() {
            CloseChoiceDialog();
            ChangeComponentVersion(comp);
        };
    });
}

// List of every installed PackProfile component (Minecraft, mod loader,
// LWJGL, intermediary mappings, ...) — name + version as the summary, X
// opens "Change Version" for any row (ChangeComponentVersion()). Matches
// the desktop VersionPage's own list content, minus the reorder/remove/
// customize/revert/add-agents/import-components surface (real, larger
// follow-ups — see the "Известные пробелы" note in CLAUDE.md).
void DrawInstanceSettingsVersion()
{
    PackProfile* profile = g_instanceSettingsTarget->getPackProfile();

    // Unlike ResourceFolderModel/WorldList (which populate themselves the
    // instant BigScreen calls their own getter), PackProfile does NOT
    // auto-load — MinecraftInstance only parses mmc-pack.json into it on
    // demand, whenever something first calls reload()/resolve(). The
    // desktop's own VersionPage does this unconditionally on open
    // (VersionPage.cpp's reloadPackProfile(), called from its
    // constructor) — nothing in BigScreen's own code path happened to
    // trigger it before this tab existed. Confirmed live: without this,
    // the list showed "No components installed." for a real, fully
    // modded instance. Same "once per distinct model pointer" caching as
    // DrawResourceFolderList()/DrawInstanceSettingsWorlds().
    static PackProfile* lastProfile = nullptr;
    if (profile != lastProfile) {
        profile->reload(Net::Mode::Online);
        lastProfile = profile;
    }

    const int count = profile->rowCount(QModelIndex());
    if (count == 0) {
        ImGui::TextUnformatted("No components installed.");
        return;
    }

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

    for (int i = 0; i < count; ++i) {
        ComponentPtr comp = profile->getComponent(i);
        if (!comp)
            continue;

        const QString name = comp->getName();
        const QString version = comp->getVersion();
        const QByteArray nameUtf8 = name.toUtf8();
        const QByteArray summaryUtf8 = version.toUtf8();

        ImGui::PushID(i);
        MenuButton(nameUtf8.constData(), summaryUtf8.constData());

        if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
            ShowVersionComponentActionsMenu(comp);
        ImGui::PopID();
    }
}

struct BigScreenServerEntry {
    QString name;
    QString address;
};

// Desktop's ServersPage.cpp keeps its NBT parsing (parseServersDat()) and
// the Server struct that reads it (both private to that .cpp, not
// exported anywhere reusable) — but the underlying library they're built
// on (libnbtplusplus, CMake target "nbt++") is a real, ordinary link
// dependency of Launcher_logic, which prismlauncher_bigscreen already
// links — so this reimplements just the reading half directly against
// that same library, rather than needing ServersPage itself. Read-only,
// name + address only (no icon/acceptTextures) — see
// DrawInstanceSettingsServers()'s own comment for why this stays
// read-only in v1 rather than also writing servers.dat back.
std::vector<BigScreenServerEntry> ReadServersDat(const QString& path)
{
    std::vector<BigScreenServerEntry> result;
    try {
        const QByteArray input = FS::read(path);
        if (input.isEmpty())
            return result;
        std::istringstream stream(std::string(input.constData(), static_cast<size_t>(input.size())));
        auto pair = nbt::io::read_compound(stream);
        if (pair.first != "" || pair.second == nullptr)
            return result;
        if (!pair.second->has_key("servers", nbt::tag_type::List))
            return result;

        auto& serversList = pair.second->at("servers").as<nbt::tag_list>();
        for (auto& entry : serversList) {
            auto& serverTag = entry.as<nbt::tag_compound>();
            BigScreenServerEntry e;
            if (serverTag.has_key("name")) {
                const std::string nameStr(serverTag["name"]);
                e.name = QString::fromUtf8(nameStr.c_str());
            }
            if (serverTag.has_key("ip")) {
                const std::string ipStr(serverTag["ip"]);
                e.address = QString::fromUtf8(ipStr.c_str());
            }
            result.push_back(e);
        }
    } catch (...) {
        // Matches parseServersDat()'s own catch-all — a missing or
        // corrupt servers.dat isn't a BigScreen bug to surface, same as
        // the desktop silently treating it as "no servers".
    }
    return result;
}

// Loads the FULL parsed servers.dat compound, not just name/address —
// used by the write helpers below (unlike ReadServersDat(), which is
// display-only and never writes anything back). Returns a fresh empty
// compound (not null) on any read/parse failure, matching
// parseServersDat()'s own "missing/corrupt file == no servers yet"
// treatment — the write helpers then just add to (or, for a rename,
// harmlessly no-op past) an empty compound instead of needing a separate
// error path.
std::unique_ptr<nbt::tag_compound> LoadServersDatCompound(const QString& path)
{
    try {
        const QByteArray input = FS::read(path);
        if (!input.isEmpty()) {
            std::istringstream stream(std::string(input.constData(), static_cast<size_t>(input.size())));
            auto pair = nbt::io::read_compound(stream);
            if (pair.first == "" && pair.second != nullptr)
                return std::move(pair.second);
        }
    } catch (...) {
    }
    return std::make_unique<nbt::tag_compound>();
}

bool SaveServersDatCompound(const QString& path, nbt::tag_compound& compound)
{
    try {
        if (!FS::ensureFilePathExists(path))
            return false;
        std::ostringstream s;
        nbt::io::write_tag("", compound, s);
        const QByteArray val(s.str().data(), static_cast<int>(s.str().size()));
        FS::write(path, val);  // throws on failure, matching FS::write()'s own contract
        return true;
    } catch (...) {
        return false;
    }
}

// Delete/Rename/Change Address/Add all follow the same shape: load the
// full compound, mutate just the one list or entry actually being
// touched, save. Every OTHER entry's `value` (name, ip, icon,
// acceptTextures, whatever else — this project has no reason to know or
// care) is moved wholesale from the old list into the new one rather than
// reconstructed field-by-field from a lossy intermediate struct — the
// same "icon/acceptTextures could silently get dropped" risk flagged when
// this tab was first built (read-only, v1) doesn't apply here, since
// nothing not explicitly being edited is ever touched or re-serialized
// from scratch.
void DeleteServerEntry(const QString& path, int index)
{
    std::unique_ptr<nbt::tag_compound> root = LoadServersDatCompound(path);
    if (!root->has_key("servers", nbt::tag_type::List))
        return;
    nbt::tag_list& oldList = root->at("servers").as<nbt::tag_list>();
    if (index < 0 || static_cast<size_t>(index) >= oldList.size())
        return;

    nbt::tag_list newList(nbt::tag_type::Compound);
    for (size_t i = 0; i < oldList.size(); ++i) {
        if (static_cast<int>(i) == index)
            continue;
        newList.push_back(std::move(oldList[i]));
    }
    (*root)["servers"] = nbt::value(std::move(newList));
    SaveServersDatCompound(path, *root);
}

void SetServerField(const QString& path, int index, const char* key, const QString& newValue)
{
    std::unique_ptr<nbt::tag_compound> root = LoadServersDatCompound(path);
    if (!root->has_key("servers", nbt::tag_type::List))
        return;
    nbt::tag_list& list = root->at("servers").as<nbt::tag_list>();
    if (index < 0 || static_cast<size_t>(index) >= list.size())
        return;
    nbt::tag_compound& entry = list[static_cast<size_t>(index)].as<nbt::tag_compound>();
    entry[key] = newValue.toStdString();
    SaveServersDatCompound(path, *root);
}

// New entries get name + ip only — no icon (matches the desktop's own
// "Add" behavior: a blank row the user edits inline, no icon until the
// game actually connects and one gets cached), no acceptTextures (desktop
// only ever writes that tag when the user picks something other than the
// "Ask" default, which "Add" doesn't).
void AddServerEntry(const QString& path, const QString& name, const QString& address)
{
    std::unique_ptr<nbt::tag_compound> root = LoadServersDatCompound(path);
    nbt::tag_list list(nbt::tag_type::Compound);
    if (root->has_key("servers", nbt::tag_type::List)) {
        nbt::tag_list& existing = root->at("servers").as<nbt::tag_list>();
        for (size_t i = 0; i < existing.size(); ++i)
            list.push_back(std::move(existing[i]));
    }
    nbt::tag_compound newServer;
    newServer.insert("name", name.toStdString());
    newServer.insert("ip", address.toStdString());
    list.push_back(std::move(newServer));
    (*root)["servers"] = nbt::value(std::move(list));
    SaveServersDatCompound(path, *root);
}

// Instance Settings > Servers' own displayed list, cached like the other
// tabs' models (Mods/Worlds/...) — a plain read + a path key rather than a
// QAbstractListModel, since servers.dat has no equivalent of those models'
// own QFileSystemWatcher. Reloaded explicitly (ReloadServersCache()) after
// any write, and whenever DrawInstanceSettingsServers() sees a different
// path (switched instance).
QString g_serversCachePath;
std::vector<BigScreenServerEntry> g_serversCache;

void ReloadServersCache(const QString& path)
{
    g_serversCachePath = path;
    g_serversCache = ReadServersDat(path);
}

// X on a focused server — Rename/Change Address (no desktop dialog to
// match, same reasoning as Worlds' own "Rename" — the desktop edits these
// inline in its table, not via a popup, so this text stays BigScreen's
// own) and Remove (real strings, context "ServersPage"). g_pendingAction-
// deferred throughout, same reentrancy reasoning as every other X-menu in
// this file. Reloads g_serversCache after any successful write so the
// list reflects the change on the very next frame, rather than needing a
// tab switch to pick it up.
void ShowServerActionsMenu(const QString& path, int index, const QString& name)
{
    ChoiceDialogOptions options;
    options.emplace_back("Rename", false);
    options.emplace_back("Change Address", false);
    options.emplace_back(TR("ServersPage", "Remove").toStdString(), false);

    OpenChoiceDialog(name.toStdString(), false, std::move(options), [path, index, name](s32 choice, const std::string&, bool) {
        if (choice < 0)
            return;
        g_pendingAction = [path, index, name, choice]() {
            CloseChoiceDialog();
            switch (choice) {
                case 0: {
                    const auto newName = BigScreenDialogs::InputString("Rename Server", "Enter a new name", name.toStdString());
                    if (newName && !newName->isEmpty()) {
                        SetServerField(path, index, "name", *newName);
                        ReloadServersCache(path);
                    }
                    break;
                }
                case 1: {
                    const QString currentAddress =
                        (index >= 0 && static_cast<size_t>(index) < g_serversCache.size()) ? g_serversCache[static_cast<size_t>(index)].address : QString();
                    const auto newAddress =
                        BigScreenDialogs::InputString("Change Server Address", "Enter a new address", currentAddress.toStdString());
                    if (newAddress && !newAddress->isEmpty()) {
                        SetServerField(path, index, "ip", *newAddress);
                        ReloadServersCache(path);
                    }
                    break;
                }
                case 2: {
                    const QString message = TR("ServersPage", "You are about to remove \"%1\".\n"
                                                                "This is permanent and the server will be gone from your list forever (A "
                                                                "LONG TIME).\n\n"
                                                                "Are you sure?")
                                                 .arg(name);
                    if (BigScreenDialogs::Confirm(TR("ServersPage", "Confirm Removal").toStdString(), message.toStdString(), false,
                                                   TR("ServersPage", "Remove").toStdString(), TR("LaunchController", "Cancel").toStdString())) {
                        DeleteServerEntry(path, index);
                        ReloadServersCache(path);
                    }
                    break;
                }
            }
        };
    });
}

// Add/Rename/Change Address/Remove, on top of the read-only list this tab
// started with — see LoadServersDatCompound()/SaveServersDatCompound() and
// the write helpers above for why this doesn't risk the icon/
// acceptTextures data-loss originally flagged as the reason to stay
// read-only. Not ported: reorder, live server ping (player count/MOTD).
void DrawInstanceSettingsServers()
{
    MinecraftInstance* inst = g_instanceSettingsTarget;
    const QString path = FS::PathCombine(inst->gameRoot(), "servers.dat");

    if (path != g_serversCachePath)
        ReloadServersCache(path);

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

    // Real "Add" text (context "ServersPage") — new server prompts for
    // name then address via two InputString calls, matching the file
    // selector-based flows elsewhere in this file for "ask a couple of
    // things, then do the write" actions.
    if (MenuButtonWithoutSummary(("+ " + TR("ServersPage", "Add")).toStdString().c_str())) {
        g_pendingAction = [path]() {
            const auto name = BigScreenDialogs::InputString("Add Server", "Enter a server name", "Minecraft Server");
            if (!name || name->isEmpty())
                return;
            const auto address = BigScreenDialogs::InputString("Add Server", "Enter a server address", "");
            if (!address || address->isEmpty())
                return;
            AddServerEntry(path, *name, *address);
            ReloadServersCache(path);
        };
    }

    std::vector<BigScreenServerEntry>& cachedServers = g_serversCache;
    if (cachedServers.empty()) {
        ImGui::TextUnformatted("No servers saved.");
        return;
    }

    for (size_t i = 0; i < cachedServers.size(); ++i) {
        const QString entryName = cachedServers[i].name;
        const QByteArray nameUtf8 = entryName.toUtf8();
        const QByteArray addressUtf8 = cachedServers[i].address.toUtf8();
        ImGui::PushID(static_cast<int>(i));
        MenuButton(nameUtf8.constData(), addressUtf8.constData());

        if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
            ShowServerActionsMenu(path, static_cast<int>(i), entryName);
        ImGui::PopID();
    }
}

struct BigScreenScreenshotEntry {
    QString path;
    QString fileName;
    QDateTime modified;
    qint64 size;
};

// Same folder + filter the desktop's own ScreenshotsPage uses
// (InstancePageProvider.h: FS::PathCombine(inst->gameRoot(),
// "screenshots"); ScreenshotsPage.cpp: m_model->setNameFilters({"*.png"})
// — Minecraft only ever writes PNG screenshots) — plain QDir scan, no
// custom model needed the way ServersPage's servers.dat parsing did;
// desktop's own ScreenshotsFSModel is just a QFileSystemModel with a name
// filter, nothing this reimplements loses by not reusing it directly.
std::vector<BigScreenScreenshotEntry> GetInstanceScreenshots(MinecraftInstance* inst)
{
    std::vector<BigScreenScreenshotEntry> result;
    const QDir dir(FS::PathCombine(inst->gameRoot(), "screenshots"));
    const QStringList entries = dir.entryList({ "*.png" }, QDir::Files | QDir::Readable, QDir::SortFlag::Time);
    for (const QString& name : entries) {
        const QFileInfo fi(dir.filePath(name));
        result.push_back({ fi.absoluteFilePath(), name, fi.lastModified(), fi.size() });
    }
    return result;
}

// X on a focused screenshot — just Delete (the only destructive action;
// desktop's Rename/View Folder/Copy-to-clipboard aren't ported — Rename
// has no real gamepad-relevant benefit for an auto-named screenshot file,
// View Folder needs a working file browser round-trip, Copy-to-clipboard
// assumes a mouse-driven paste target). Real strings from
// ScreenshotsPage.cpp/.ui, context "ScreenshotsPage".
void ShowScreenshotActionsMenu(const QString& path, const QString& fileName)
{
    ChoiceDialogOptions options;
    options.emplace_back(TR("ScreenshotsPage", "Delete").toStdString(), false);

    OpenChoiceDialog(fileName.toStdString(), false, std::move(options), [path](s32 index, const std::string&, bool) {
        if (index != 0)
            return;
        g_pendingAction = [path]() {
            CloseChoiceDialog();
            if (BigScreenDialogs::Confirm(TR("ScreenshotsPage", "Confirm Deletion").toStdString(),
                                           TR("ScreenshotsPage", "You are about to delete the selected screenshot.\n"
                                                                  "This may be permanent and it will be gone from the folder.\n\n"
                                                                  "Are you sure?")
                                               .toStdString(),
                                           false, TR("ScreenshotsPage", "Delete").toStdString(),
                                           TR("LaunchController", "Cancel").toStdString())) {
                QFile::remove(path);
                // If the deleted file is the one currently open in the
                // viewer, back out to the list rather than leaving a
                // texture for a file that no longer exists on screen.
                if (g_selectedScreenshotPath == path) {
                    g_selectedScreenshotPath.clear();
                    g_selectedScreenshotTexture.reset();
                    g_selectedScreenshotIndex = -1;
                }
            }
        };
    });
}

// Two-state screen (file list, or the selected image) — same shape as
// DrawInstanceSettingsLogs(), texture instead of text lines. Reuses
// BigScreenGui::UploadQImage() (already used for real instance icons and
// the Microsoft login QR code — see GetInstanceIconTexture()/AccountLogin
// screen) rather than any new image-loading path. Loaded lazily, one
// screenshot at a time, only when actually opened — not a thumbnail grid
// (the desktop's own ScreenshotsPage thumbnails via a 4-thread pool, real
// but substantial extra machinery a gamepad list doesn't need).
// Loads the screenshot at `index` into the viewer globals — shared by the
// list's own MenuButton confirm handler and the gallery-style Left/Right
// navigation inside the viewer itself, so both go through one path.
void OpenScreenshotViewer(const std::vector<BigScreenScreenshotEntry>& screenshots, int index)
{
    if (index < 0 || index >= static_cast<int>(screenshots.size()))
        return;
    const BigScreenScreenshotEntry& shot = screenshots[static_cast<size_t>(index)];
    const QImage image(shot.path);
    g_selectedScreenshotTexture = image.isNull() ? nullptr : BigScreenGui::UploadQImage(image);
    g_selectedScreenshotPath = shot.path;
    g_selectedScreenshotIndex = index;
}

void DrawInstanceSettingsScreenshots()
{
    MinecraftInstance* inst = g_instanceSettingsTarget;

    if (inst != g_selectedScreenshotInstance) {
        g_selectedScreenshotPath.clear();
        g_selectedScreenshotTexture.reset();
        g_selectedScreenshotIndex = -1;
        g_selectedScreenshotInstance = inst;
    }

    // Needed in both branches now: the list to render itself, the viewer
    // to know how many screenshots exist for Left/Right to wrap around.
    // Re-scanning every frame (not cached) matches this function's own
    // existing list-branch behavior — a small directory, harmless.
    const std::vector<BigScreenScreenshotEntry> screenshots = GetInstanceScreenshots(inst);

    if (!g_selectedScreenshotPath.isEmpty()) {
        if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Back")) {
            g_selectedScreenshotPath.clear();
            g_selectedScreenshotTexture.reset();
            g_selectedScreenshotIndex = -1;
            return;
        }

        // Gallery-style browsing: Left/Right (D-pad or stick — the stick
        // is already mirrored onto these same GamepadDpad* keys, see the
        // per-frame stick-to-D-pad mirror in main()) moves to the
        // previous/next screenshot and wraps around at either end. Safe
        // to repurpose Left/Right here specifically because the viewer
        // has no vertical list of its own to navigate — unlike every
        // other screen, where these keys are already spoken for.
        if (screenshots.size() > 1 && g_selectedScreenshotIndex >= 0) {
            const int count = static_cast<int>(screenshots.size());
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false))
                OpenScreenshotViewer(screenshots, (g_selectedScreenshotIndex + 1) % count);
            else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false))
                OpenScreenshotViewer(screenshots, (g_selectedScreenshotIndex - 1 + count) % count);
        }

        if (screenshots.size() > 1) {
            const GamepadGlyphs glyphs = GetGamepadGlyphs();
            SetFooterHints({ { glyphs.dpad_lr, "Prev / Next" }, { glyphs.cancel(false), "Back" } });
        }

        if (g_selectedScreenshotTexture) {
            // Scale to fit the available area while preserving aspect
            // ratio (never upscale past the available width or height,
            // whichever is the binding constraint) and center
            // horizontally — same "fit inside a box" math as any image
            // viewer, nothing toolkit-specific to lean on here.
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH = ImGui::GetContentRegionAvail().y;
            const float texW = static_cast<float>(g_selectedScreenshotTexture->GetWidth());
            const float texH = static_cast<float>(g_selectedScreenshotTexture->GetHeight());
            float drawW = availW;
            float drawH = texH * (availW / texW);
            if (drawH > availH) {
                drawH = availH;
                drawW = texW * (availH / texH);
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (availW - drawW) * 0.5f));
            ImGui::Image(static_cast<ImTextureID>(g_selectedScreenshotTexture->GetNativeHandle()), ImVec2(drawW, drawH));
        } else {
            ImGui::TextUnformatted("Failed to load this screenshot.");
        }
        return;
    }

    if (screenshots.empty()) {
        ImGui::TextUnformatted("No screenshots yet.");
        return;
    }

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();
    const QLocale locale;

    for (size_t i = 0; i < screenshots.size(); ++i) {
        const BigScreenScreenshotEntry& shot = screenshots[i];
        const QString summary = locale.toString(shot.modified, QLocale::ShortFormat) + "  \xE2\x80\xA2  " + locale.formattedDataSize(shot.size);
        const QByteArray nameUtf8 = shot.fileName.toUtf8();
        const QByteArray summaryUtf8 = summary.toUtf8();

        ImGui::PushID(static_cast<int>(i));
        if (MenuButton(nameUtf8.constData(), summaryUtf8.constData()))
            OpenScreenshotViewer(screenshots, static_cast<int>(i));

        if (!anyDialogOpen && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
            ShowScreenshotActionsMenu(shot.path, shot.fileName);
        ImGui::PopID();
    }
}

struct InstanceSettingsSubTab {
    const char* name;
    void (*draw)();
};
struct InstanceTopTab {
    const char* name;
    const char* icon;
    const InstanceSettingsSubTab* subtabs;
    int subtabCount;
    // Mods/Resource/Texture/Shader/Data Packs and Worlds all show a list
    // where X opens a per-item actions menu — Settings doesn't. Drives
    // which footer hint row to show, replacing an earlier name-string
    // comparison ("Mods") that would've silently missed each new tab added
    // alongside this field.
    bool hasActionsMenu;
    // A-button hint text for hasActionsMenu tabs, or nullptr to omit the
    // A-button hint entirely (Worlds: nothing is bound to A, only X).
    // Ignored when !hasActionsMenu (Settings hardcodes its own "Toggle /
    // Change" wording below, unconditionally correct for every Settings
    // sub-tab).
    const char* primaryHint;
};

// Two-level structure, matching DrawSettings()' own Category (LB/RB) +
// Sub-tab (LT/RT) pattern exactly, rather than one flat row — per explicit
// feedback that mixing "Settings' own internal groupings" (General/Java/
// Tweaks/Commands/Notes — these were always one page's sub-tabs, matching
// the desktop's own "settings" tab) with "Mods" (a genuinely separate
// desktop page, like Resource/Texture/Shader Packs also are) in one flat
// row read as structurally wrong, not just a naming nitpick: Mods isn't a
// sibling of General/Java/etc., it's a sibling of the *whole* Settings
// category. Every later top-level category slots in here the same way
// Mods did — each its own InstanceTopTab entry, not another flat tab
// appended to Settings' own row.
static const InstanceSettingsSubTab kInstanceSettingsSubTabs[] = {
    { "General", &DrawInstanceSettingsGeneral },   { "Java", &DrawInstanceSettingsJava },
    { "Tweaks", &DrawInstanceSettingsTweaks },      { "Commands", &DrawInstanceSettingsCommands },
    { "Notes", &DrawInstanceSettingsNotes },
};
static const InstanceSettingsSubTab kInstanceModsSubTabs[] = {
    { "Mods", &DrawInstanceSettingsMods },
};
static const InstanceSettingsSubTab kInstanceResourcePacksSubTabs[] = {
    { "Resource Packs", &DrawInstanceSettingsResourcePacks },
};
static const InstanceSettingsSubTab kInstanceTexturePacksSubTabs[] = {
    { "Texture Packs", &DrawInstanceSettingsTexturePacks },
};
static const InstanceSettingsSubTab kInstanceShaderPacksSubTabs[] = {
    { "Shader Packs", &DrawInstanceSettingsShaderPacks },
};
static const InstanceSettingsSubTab kInstanceDataPacksSubTabs[] = {
    { "Data Packs", &DrawInstanceSettingsDataPacks },
};
static const InstanceSettingsSubTab kInstanceWorldsSubTabs[] = {
    { "Worlds", &DrawInstanceSettingsWorlds },
};
static const InstanceSettingsSubTab kInstanceLogsSubTabs[] = {
    { "Logs", &DrawInstanceSettingsLogs },
};
static const InstanceSettingsSubTab kInstanceVersionSubTabs[] = {
    { "Version", &DrawInstanceSettingsVersion },
};
static const InstanceSettingsSubTab kInstanceServersSubTabs[] = {
    { "Servers", &DrawInstanceSettingsServers },
};
static const InstanceSettingsSubTab kInstanceScreenshotsSubTabs[] = {
    { "Screenshots", &DrawInstanceSettingsScreenshots },
};

// Max entries BuildInstanceTopTabs() can produce (Settings, Mods, one of
// Resource/Texture Packs, Shader Packs, Data Packs, Worlds, Logs, Version,
// Servers, Screenshots) — sized for the caller's stack array, bump if a
// future category is added.
constexpr int kMaxInstanceTopTabs = 10;

// Built fresh each call rather than one static array, because which
// top-level categories exist depends on the instance: Resource Packs and
// Texture Packs are mutually exclusive on the real desktop side
// (ResourcePackPage::shouldDisplay()/TexturePackPage::shouldDisplay() gate
// on the instance's "texturepacks" trait — pre-1.6 Minecraft used texture
// packs, everything since uses resource packs, an instance is never both)
// — so BigScreen shows whichever one the desktop would, not both or a
// guess. Cheap (a handful of pointer-sized entries) to rebuild every call.
int BuildInstanceTopTabs(MinecraftInstance* inst, InstanceTopTab* out)
{
    int n = 0;
    out[n++] = { "Settings", "images/icons/settings.png", kInstanceSettingsSubTabs, static_cast<int>(std::size(kInstanceSettingsSubTabs)),
                 false, nullptr };
    out[n++] = { "Mods", "images/icons/tab_mods.png", kInstanceModsSubTabs, static_cast<int>(std::size(kInstanceModsSubTabs)), true,
                 "Toggle" };
    if (inst->traits().contains("texturepacks")) {
        out[n++] = { "Texture Packs", "images/icons/tab_resourcepacks.png", kInstanceTexturePacksSubTabs,
                      static_cast<int>(std::size(kInstanceTexturePacksSubTabs)), true, "Toggle" };
    } else {
        out[n++] = { "Resource Packs", "images/icons/tab_resourcepacks.png", kInstanceResourcePacksSubTabs,
                      static_cast<int>(std::size(kInstanceResourcePacksSubTabs)), true, "Toggle" };
    }
    out[n++] = { "Shader Packs", "images/icons/tab_shaderpacks.png", kInstanceShaderPacksSubTabs,
                 static_cast<int>(std::size(kInstanceShaderPacksSubTabs)), true, "Toggle" };
    // Real desktop behavior (InstancePageProvider only ever adds
    // GlobalDataPackPage, never the raw DataPackPage — confirmed by
    // reading it) — this tab, unlike every other resource list here, is
    // NOT always present: dataPackList() (MinecraftInstance.cpp) returns
    // nullptr unless "GlobalDataPacksEnabled" is on (default off — this is
    // a niche opt-in feature for mods that share data packs across
    // worlds, not the same thing as an ordinary per-world data pack, see
    // DrawInstanceSettingsGeneral()'s own comment on the toggle). Gating
    // here — rather than only null-checking inside
    // DrawInstanceSettingsDataPacks() — matches the desktop's own
    // shouldDisplay()-based tab visibility instead of showing a
    // permanently-empty tab for the common case.
    if (inst->settings()->get("GlobalDataPacksEnabled").toBool()) {
        out[n++] = { "Data Packs", "images/icons/tab_datapacks.png", kInstanceDataPacksSubTabs,
                      static_cast<int>(std::size(kInstanceDataPacksSubTabs)), true, "Toggle" };
    }
    out[n++] = { "Worlds", "images/icons/tab_worlds.png", kInstanceWorldsSubTabs, static_cast<int>(std::size(kInstanceWorldsSubTabs)), true,
                 nullptr };
    // hasActionsMenu=false: unlike Mods/Packs/Worlds, X doesn't open a
    // per-item menu here — pressing A on a log file directly enters its
    // viewer (see DrawInstanceSettingsLogs()). primaryHint="View" still
    // gets shown via the non-hasActionsMenu footer branch below, which
    // now checks primaryHint too instead of hardcoding "Toggle / Change"
    // unconditionally.
    out[n++] = { "Logs", "images/icons/tab_logs.png", kInstanceLogsSubTabs, static_cast<int>(std::size(kInstanceLogsSubTabs)), false, "View" };
    // hasActionsMenu=true: X on any row opens "Change Version", filtered
    // to versions compatible with the installed Minecraft version for
    // every component except Minecraft itself (see
    // ChangeComponentVersion()). primaryHint=nullptr: no single action
    // applies to every row the way "Toggle" does for Mods, so the
    // A-button hint is omitted entirely, same as Worlds.
    out[n++] = { "Version", "images/icons/tab_version.png", kInstanceVersionSubTabs, static_cast<int>(std::size(kInstanceVersionSubTabs)),
                 true, nullptr };
    // hasActionsMenu=true, primaryHint=nullptr: X opens Rename/Change
    // Address/Remove per row, plus a "+ Add" entry at the top of the list
    // itself (not an X-menu action, since it doesn't apply to any
    // existing row) — no single action applies to every row the way
    // "Toggle" does for Mods, so the A-button hint is omitted, same as
    // Worlds.
    out[n++] = { "Servers", "images/icons/tab_servers.png", kInstanceServersSubTabs, static_cast<int>(std::size(kInstanceServersSubTabs)),
                 true, nullptr };
    // hasActionsMenu=true, primaryHint="View": A opens the selected
    // screenshot (DrawInstanceSettingsScreenshots()'s own MenuButton
    // confirm handler), X opens Delete — same shape as Mods/Packs.
    out[n++] = { "Screenshots", "images/icons/tab_screenshots.png", kInstanceScreenshotsSubTabs,
                 static_cast<int>(std::size(kInstanceScreenshotsSubTabs)), true, "View" };
    return n;
}

int g_instanceTopTab = 0;
int g_instanceSubTab = 0;

void DrawInstanceSettings()
{
    // Shouldn't happen (only reachable via ShowInstanceActionsMenu(), which
    // always sets this first) — defensive fallback instead of dereferencing
    // null.
    if (!g_instanceSettingsTarget) {
        SetScreen(Screen::Instances);
        return;
    }

    InstanceTopTab allTabs[kMaxInstanceTopTabs];
    const int topTabCount = BuildInstanceTopTabs(g_instanceSettingsTarget, allTabs);
    // Defensive: switching between instances with different trait sets
    // (e.g. one has "texturepacks", another doesn't) could otherwise leave
    // a stale index pointing past the end of a shorter tab set.
    if (g_instanceTopTab >= topTabCount)
        g_instanceTopTab = 0;
    const InstanceTopTab& currentTop = allTabs[g_instanceTopTab];

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        if (currentTop.hasActionsMenu) {
            if (currentTop.primaryHint) {
                SetFooterHints({ { glyphs.confirm(false), currentTop.primaryHint },
                                  { glyphs.west, "Actions" },
                                  { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                                  { glyphs.cancel(false), "Back" } });
            } else {
                // Worlds: nothing is bound to A — omit that hint entirely
                // rather than advertise a button that does nothing.
                SetFooterHints({ { glyphs.west, "Actions" },
                                  { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                                  { glyphs.cancel(false), "Back" } });
            }
        } else if (currentTop.subtabCount > 1) {
            // primaryHint isn't set for any current multi-sub-tab category
            // (only Settings has subtabCount > 1, and its own sub-tabs are
            // all toggle/choice lists) — "Toggle / Change" is the correct
            // default there. Falls back to it whenever primaryHint is
            // unset, same as the single-sub-tab branch below.
            SetFooterHints({ { glyphs.confirm(false), currentTop.primaryHint ? currentTop.primaryHint : "Toggle / Change" },
                              { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                              { ICON_PF_XBOX_LT "/" ICON_PF_XBOX_RT, "Tab" },
                              { glyphs.cancel(false), "Back" } });
        } else if (currentTop.primaryHint && currentTop.primaryHint[0] == '\0') {
            // Explicit empty string (as opposed to nullptr, which falls
            // back to "Toggle / Change" below): a fully read-only tab with
            // nothing bound to A *or* X (Servers, v1 — see
            // DrawInstanceSettingsServers()) — showing neither hint beats
            // advertising a button that does nothing.
            SetFooterHints({ { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" }, { glyphs.cancel(false), "Back" } });
        } else {
            SetFooterHints({ { glyphs.confirm(false), currentTop.primaryHint ? currentTop.primaryHint : "Toggle / Change" },
                              { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Category" },
                              { glyphs.cancel(false), "Back" } });
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        g_instanceTopTab = (g_instanceTopTab + 1) % topTabCount;
        g_instanceSubTab = 0;
        QueueResetFocus(FocusResetType::Other);
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        g_instanceTopTab = (g_instanceTopTab - 1 + topTabCount) % topTabCount;
        g_instanceSubTab = 0;
        QueueResetFocus(FocusResetType::Other);
    }

    // Nothing to switch to with only one sub-tab (Mods, and now Resource/
    // Texture/Shader Packs too) — skip LT/RT handling and skip drawing the
    // sub-tab row at all below, matching
    // DrawSettings()' own identical rule (and root-caused fix — see its
    // history — for why a single-item sub-tab row isn't just harmless
    // clutter but can leave nav focus unreachable).
    if (currentTop.subtabCount > 1) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) {
            g_instanceSubTab = (g_instanceSubTab + 1) % currentTop.subtabCount;
            QueueResetFocus(FocusResetType::Other);
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) {
            g_instanceSubTab = (g_instanceSubTab - 1 + currentTop.subtabCount) % currentTop.subtabCount;
            QueueResetFocus(FocusResetType::Other);
        }
    }

    // topTabs is sized to the kMaxInstanceTopTabs *upper bound*, but
    // topTabCount (this instance's real tab count — e.g. 5 whenever Data
    // Packs is hidden, see BuildInstanceTopTabs()) can be smaller. Passing
    // the raw array to BeginScreen() would implicitly convert it to a
    // std::span sized by the array's compile-time extent (6), not
    // topTabCount — silently including trailing uninitialized stack
    // entries. Confirmed via gdb: this crashed with a SIGSEGV inside
    // DrawTopBar()'s icon-string handling, reading a garbage pointer from
    // exactly that uninitialized 6th slot, every time an instance had
    // fewer than kMaxInstanceTopTabs real tabs. Constructing the span
    // explicitly with the real count fixes it.
    TopBarTab topTabs[kMaxInstanceTopTabs];
    for (int i = 0; i < topTabCount; ++i)
        topTabs[i] = { allTabs[i].icon, i == g_instanceTopTab };
    const std::span<const TopBarTab> topTabsSpan(topTabs, static_cast<size_t>(topTabCount));

    // "Edit... — <instance name> — <category>", matching Settings' own
    // "Settings — <category>" pattern.
    const std::string screenTitle = StripMnemonic(MW("&Edit...")).toStdString() + " \xE2\x80\x94 " +
                                     g_instanceSettingsTarget->name().toStdString() + " \xE2\x80\x94 " + currentTop.name;

    if (BeginScreen(screenTitle.c_str(), true, topTabsSpan)) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "instance_settings")) {
            if (currentTop.subtabCount > 1) {
                // Same non-scrolling tab-row / scrolling-content two-child-
                // window split DrawSettings() uses for its own sub-tab row,
                // and for the same reasons (NavTab's own trailing
                // ImGui::SameLine() would otherwise carry into the first
                // setting row; keeps the tab row pinned while the content
                // below it scrolls). ImGuiChildFlags_NavFlattened on both:
                // without it, each nested BeginChild becomes its own
                // separate nav boundary instead of merging into the
                // parent's — D-pad nav couldn't reach into (or back out of)
                // the settings list at all.
                ImGui::BeginChild("instance_settings_subtabs",
                                   ImVec2(0.0f, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                                                     ImGui::GetStyle().FramePadding.y * 2.0f + LayoutScale(4.0f)),
                                   ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
                BeginNavBar();
                for (int i = 0; i < currentTop.subtabCount; ++i) {
                    if (NavTab(currentTop.subtabs[i].name, i == g_instanceSubTab, true, 150.0f, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                               UISecondaryColor)) {
                        g_instanceSubTab = i;
                        QueueResetFocus(FocusResetType::Other);
                    }
                }
                EndNavBar();
                ImGui::EndChild();
            }

            ImGui::BeginChild("instance_settings_content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
            BeginMenuButtons();
            ResetFocusHere();
            currentTop.subtabs[g_instanceSubTab].draw();
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

// "Update Pack" — offered in ShowInstanceActionsMenu() only for
// Modrinth-managed instances (inst->isManagedPack() &&
// getManagedPackType() == "modrinth"; CurseForge managed packs aren't
// supported — the online browsing/install machinery this reuses,
// sections 27-29 in CLAUDE.md, is Modrinth-only, no API key wired up for
// CurseForge). Real desktop equivalent:
// ModrinthManagedPackPage::parseManagedPack()/ManagedPackPage::updatePack()
// (launcher/ui/pages/instance/ManagedPackPage.cpp) — reuses the exact same
// getProjectVersions()/InstanceImportTask machinery the earlier Modrinth
// browsing rounds already built and proved, just seeded from the
// ALREADY-installed pack's own ID (getManagedPackID()) instead of a fresh
// search, and with "original_instance_id" in extraInfo (confirmed by
// reading InstanceImportTask::processModrinth() — the exact same field
// read there that this project's own online-install path leaves unset)
// so the real ModrinthCreationTask replaces this instance in place rather
// than creating a new sibling.
void CheckAndUpdateManagedPack(MinecraftInstance* inst)
{
    const std::string dialogTitle = TR("ManagedPackPage", "Update Pack").toStdString();

    ModPlatform::IndexedPack::Ptr pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = inst->getManagedPackID();

    ResourceAPI::VersionSearchArgs versionArgs;
    versionArgs.pack = pack;
    versionArgs.resourceType = ModPlatform::ResourceType::Modpack;

    QVector<ModPlatform::IndexedVersion> versions;
    bool succeeded = false;
    QString failReason;
    Task::Ptr versionsTask = ModrinthAPI::get().getProjectVersions(
        versionArgs, { [&versions, &succeeded](QVector<ModPlatform::IndexedVersion>& result) {
                          versions = result;
                          succeeded = true;
                      },
                       [&failReason](const QString& reason, int) { failReason = reason; }, [] {} });
    if (!versionsTask) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to check for updates.", false, "OK", "OK");
        return;
    }
    versionsTask->start();
    BigScreenDialogs::WaitForTask(versionsTask.get());

    if (!succeeded || versions.isEmpty()) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to check for updates: " + failReason.toStdString(), false, "OK", "OK");
        return;
    }

    // Real desktop labeling: ModrinthManagedPackPage::parseManagedPack()
    // appends " (Current)" to whichever returned version's raw `version`
    // field matches getManagedPackVersionName() — note this compares
    // against `version`, not `versionNumber`/`fileId` (the desktop's own
    // comment there explains why: a version's fileId in this API response
    // isn't always the same id recorded in the modpack format spec).
    const QString currentVersionName = inst->getManagedPackVersionName();
    std::vector<std::string> labels;
    for (const ModPlatform::IndexedVersion& v : versions) {
        QString label = v.getVersionDisplayString();
        if (v.version == currentVersionName)
            label = TR("ModrinthManagedPackPage", "%1 (Current)").arg(label);
        labels.push_back(label.toStdString());
    }

    const auto choice = BigScreenDialogs::Choose(dialogTitle, labels);
    if (!choice || *choice < 0 || static_cast<size_t>(*choice) >= static_cast<size_t>(versions.size()))
        return;
    const ModPlatform::IndexedVersion& chosenVersion = versions[static_cast<int>(*choice)];

    if (chosenVersion.version == currentVersionName) {
        // No real desktop equivalent for this specific confirmation (the
        // desktop just lets you click "Update Pack" on the already-
        // selected "(Current)" entry with no extra prompt) — added here
        // since re-running a full reinstall isn't free, worth a check
        // before doing it by accident on a gamepad.
        if (!BigScreenDialogs::Confirm(dialogTitle, "This is already the installed version. Reinstall it anyway?", false, "Reinstall",
                                        "Cancel"))
            return;
    }

    QMap<QString, QString> extraInfo{ { "pack_id", inst->getManagedPackID() },
                                       { "pack_version_id", chosenVersion.fileId.toString() },
                                       { "original_instance_id", inst->id() } };
    auto* importTask = new InstanceImportTask(QUrl(chosenVersion.downloadUrl), true, nullptr, extraInfo);
    // Keeps the instance's existing name as-is rather than replicating the
    // desktop's name.replace(oldVersionName, newVersionName) heuristic
    // (ManagedPackPage::updatePack()) — that string-replace can misfire on
    // a name a user has already customized to not contain the raw version
    // string, and BigScreen has no inline rename-preview UI to catch a bad
    // result before it's applied.
    importTask->setName(inst->name());
    importTask->setGroup(APPLICATION->instances()->getInstanceGroup(inst->id()));
    importTask->setIcon(inst->iconKey());
    importTask->setConfirmUpdate(false);

    unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(importTask));
    task->start();
    BigScreenDialogs::WaitForTask(task.get());

    if (!task->wasSuccessful()) {
        BigScreenDialogs::Confirm(
            TR("ManagedPackPage", "Update Failed").toStdString(),
            TR("ManagedPackPage", "The instance failed to update to pack version %1. Please check launcher logs for more information.")
                .arg(chosenVersion.version)
                .toStdString(),
            false, "OK", "OK");
        return;
    }
    BigScreenDialogs::Confirm(
        TR("ManagedPackPage", "Update Successful").toStdString(),
        TR("ManagedPackPage", "The instance updated to pack version %1 successfully.").arg(chosenVersion.version).toStdString(), false,
        "OK", "OK");
}

// X on a focused instance card opens this — a curated subset of the
// desktop's right-click context menu (MainWindow::showInstanceContextMenu),
// picking the actions that make sense without a mouse/keyboard: launching,
// killing, opening the console, editing settings (Screen::InstanceSettings,
// via DrawInstanceSettings() above — the "settings" tab of the desktop's
// full Edit Instance window, not the whole thing), copying, renaming,
// changing group, viewing the instance folder, exporting, and deleting.
// Not yet ported: mrpack/CurseForge pack export (ExportPackDialog —
// reconstructs a portable pack manifest, real modplatform-specific work
// beyond StartInstanceExport()'s plain zip), Create Shortcut.
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
// Copy, Rename, Change Group, Export, Delete) defers that through
// g_pendingAction rather than calling it directly; Launch/Open Console/
// View Folder don't need a dialog at all, so they run immediately.

// Reuses MMCZip::ExportToZipTask directly — the same Task
// ExportInstanceDialog::doExport() builds. Deliberately plain-zip-only
// (matches actionExportInstanceZip, not the Modrinth/.mrpack or
// CurseForge/Flame pack variants, which need to reconstruct a portable
// pack manifest — real modplatform-specific work, not a small follow-up).
// Also deliberately skips the desktop dialog's per-file include/exclude
// checkbox tree (FileIgnoreProxy) — collectFileListRecursively(...,
// nullptr) with a null filter includes everything, matching what a fresh
// "just export the whole instance" click would produce with nothing
// unchecked, the common case.
void StartInstanceExport(MinecraftInstance* inst)
{
    const std::string dialogTitle = TR("ExportInstanceDialog", "Export Instance").toStdString();
    const std::string initialDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation).toStdString();

    // select_directory=true: picks an existing folder (via the file
    // selector's own "<Use This Directory>" entry) rather than a filename
    // — BigScreen builds the actual output filename itself from the
    // instance's own name, matching the desktop dialog's own default
    // suggestion (FS::PathCombine(QDir::homePath(), name + ".zip")) minus
    // the ability to freely retype the filename, which needs a keyboard-
    // centric save dialog this project has no equivalent of.
    ImGuiFullscreen::OpenFileSelector(
        dialogTitle, true,
        [inst](const std::string& dir) {
            if (dir.empty())
                return;
            g_pendingAction = [inst, dir]() {
                CloseFileSelector();

                const QString sanitizedName = FS::RemoveInvalidFilenameChars(inst->name());
                const QString outputPath = FS::PathCombine(QString::fromStdString(dir), sanitizedName + ".zip");

                // Matches ExportInstanceDialog.cpp's own SaveIcon() —
                // copies a *custom* (non-built-in) icon file into the
                // instance root first, so it's included in the zip and the
                // re-imported instance keeps its icon. Simplified: skips
                // the built-in-theme-icon-as-pixmap fallback branch (rarer
                // — only matters for an icon that was never a real file to
                // begin with), the common "user added a custom icon image"
                // case is what this covers.
                const MMCIcon* mmcIcon = APPLICATION->icons()->icon(inst->iconKey());
                if (mmcIcon && !mmcIcon->isBuiltIn()) {
                    const QString iconPath = mmcIcon->getFilePath();
                    if (!iconPath.isEmpty()) {
                        const QFileInfo iconInfo(iconPath);
                        FS::copy(iconPath, FS::PathCombine(inst->instanceRoot(), iconInfo.fileName()))();
                    }
                }

                QFileInfoList files;
                if (!MMCZip::collectFileListRecursively(inst->instanceRoot(), nullptr, &files, nullptr)) {
                    BigScreenDialogs::Confirm(TR("ExportInstanceDialog", "Error").toStdString(),
                                               TR("ExportInstanceDialog", "Unable to export instance").toStdString(), false, "OK", "OK");
                    return;
                }

                unique_qobject_ptr<Task> task(new MMCZip::ExportToZipTask(outputPath, inst->instanceRoot(), files, "", true));
                task->start();
                BigScreenDialogs::WaitForTask(task.get());

                if (!task->wasSuccessful()) {
                    BigScreenDialogs::Confirm(TR("ExportInstanceDialog", "Error").toStdString(), task->failReason().toStdString(), false,
                                               "OK", "OK");
                }
            };
        },
        {}, initialDir);
}

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

    // Only for Modrinth-managed instances — see CheckAndUpdateManagedPack()'s
    // own comment for why CurseForge-managed packs aren't offered this.
    if (inst->isManagedPack() && inst->getManagedPackType() == "modrinth") {
        actions->push_back(
            { TR("ManagedPackPage", "Update Pack").toStdString(), [inst]() { g_pendingAction = [inst]() { CheckAndUpdateManagedPack(inst); }; } });
    }

    // Desktop's CopyInstanceDialog lets the user pick exactly what to
    // bring along (saves/mods/resource packs/screenshots/servers/...) and
    // whether to symlink/hardlink/clone instead of a plain copy
    // (InstanceCopyPrefs — 13 independent booleans). Deliberately not
    // exposed here: the default InstanceCopyPrefs{} (copy everything,
    // no link tricks) is already the common case ("just duplicate this
    // instance"), and building a 13-item checkable picker for the
    // advanced cases isn't worth it for how rarely they're used. New name
    // defaults to the original's own name, same as the desktop dialog's
    // own ui->instNameTextBox->setText(original->name()) — the user is
    // expected to change it, same expectation either UI.
    actions->push_back({ StripMnemonic(MW("Cop&y...")).toStdString(), [inst]() {
                             g_pendingAction = [inst]() {
                                 const auto name = BigScreenDialogs::InputString(TR("CopyInstanceDialog", "Copy Instance").toStdString(),
                                                                                  "Enter a name for the copy", inst->name().toStdString());
                                 if (!name || name->isEmpty())
                                     return;

                                 auto* copyTask = new InstanceCopyTask(inst, InstanceCopyPrefs());
                                 copyTask->setName(*name);
                                 copyTask->setGroup(APPLICATION->instances()->getInstanceGroup(inst->id()));
                                 copyTask->setIcon(inst->iconKey());

                                 unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(copyTask));
                                 task->start();
                                 BigScreenDialogs::WaitForTask(task.get());

                                 if (!task->wasSuccessful()) {
                                     BigScreenDialogs::Confirm(StripMnemonic(MW("Cop&y...")).toStdString(),
                                                                "Failed to copy instance: " + task->failReason().toStdString(), false, "OK",
                                                                "OK");
                                 }
                             };
                         } });

    // StartInstanceExport() opens the file selector itself (non-blocking,
    // same as OpenFileSelector's own reasoning elsewhere in this file) —
    // safe to call directly from this action's run(), no g_pendingAction
    // needed at this level (StartInstanceExport()'s own callback handles
    // the blocking part once a folder is actually picked).
    actions->push_back({ StripMnemonic(MW("E&xport...")).toStdString(), [inst]() { StartInstanceExport(inst); } });

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

// Reuses the exact same InstanceImportTask the desktop's ImportPage builds
// (ImportPage.cpp's on_modpackBtn_clicked()/updateState() local-file
// branch) — a zip or .mrpack (Modrinth pack) file, detected as either a
// Modrinth- or CurseForge-format pack from its own manifest inside the
// archive, or imported as a raw MultiMC/PrismLauncher-format instance zip
// if neither manifest is present (InstanceImportTask.cpp handles all
// three internally — nothing BigScreen-specific to replicate there).
// v1 gap, matches this project's own established pattern for it (see
// reauthenticateAccount()'s fallback and the CurseForge-blocked-mod-
// download note in CLAUDE.md's "Известные пробелы"): passing nullptr for
// the QWidget* parent means any native dialog InstanceImportTask's
// Modrinth/CurseForge sub-tasks might show mid-import (e.g. a
// CurseForge-blocked-file prompt) would have no parent — same class of
// gap as everywhere else in this codebase a QWidget* parent is passed as
// nullptr, not a new one introduced here.
void StartZipInstanceImport()
{
    const std::string initialDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation).toStdString();

    // OpenFileSelector() is the vendored toolkit's own real file-browser
    // popup (ImGuiFullscreen.cpp) — lists directories/files under
    // initial_directory, filtered by glob pattern, with normal MenuButton
    // gamepad navigation and a "<Parent Directory>" entry to go up one
    // level. Host::ShouldPreferHostFileSelector() (this project's own
    // compat shim, HostCompat.cpp) always returns false, so this always
    // uses that widget rather than trying to defer to some native OS
    // picker BigScreen has no equivalent of.
    ImGuiFullscreen::OpenFileSelector(
        TR("ImportPage", "Choose modpack").toStdString(), false,
        [](const std::string& path) {
            // Empty path means cancelled — DrawFileSelector() itself
            // already called CloseFileSelector() before invoking this,
            // matching OpenChoiceDialog's own cancel-vs-select asymmetry
            // (see ShowInstanceActionsMenu()'s comment): a REAL selection
            // leaves the popup open and expects the callback to close it,
            // which — same reentrancy reasoning as every other X/Y menu in
            // this file — has to be deferred through g_pendingAction
            // rather than done directly from in here.
            if (path.empty())
                return;
            g_pendingAction = [path]() {
                CloseFileSelector();

                const QString qpath = QString::fromStdString(path);
                const QFileInfo fi(qpath);
                const auto name = BigScreenDialogs::InputString(
                    "Instance Name", "Enter a name for the new instance", fi.completeBaseName().toStdString());
                if (!name || name->isEmpty())
                    return;

                auto* importTask = new InstanceImportTask(QUrl::fromLocalFile(qpath), false, nullptr, {});
                importTask->setName(*name);

                unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(importTask));
                task->start();
                BigScreenDialogs::WaitForTask(task.get());

                if (!task->wasSuccessful()) {
                    BigScreenDialogs::Confirm(StripMnemonic(MW("Add Instanc&e...")).toStdString(),
                                               "Failed to import instance: " + task->failReason().toStdString(), false, "OK", "OK");
                }
            };
        },
        { "*.zip", "*.mrpack" }, initialDir);
}

// Modrinth browse screen state + icon cache. Search results live here
// (not a local variable) so DrawModrinthBrowse() can redraw the same list
// across many frames — unlike every earlier Modrinth-related flow in this
// file (a single blocking Choose() call), this is a real persistent
// screen so pack icons can load in and search can re-run without
// re-entering a dialog each time.
struct ModrinthBrowseState {
    QList<ModPlatform::IndexedPack::Ptr> packs;
    QString query;  // empty = browsing by downloads, no search term
    QString lastError;
    // Modpack means "create a new instance" (InstallModrinthPack(), the
    // original use of this screen); any other type means "install one
    // resource into targetModel/targetInstance" (InstallModrinthResource())
    // — see DrawModrinthBrowse()'s install branch.
    ModPlatform::ResourceType resourceType = ModPlatform::ResourceType::Modpack;
    ResourceFolderModel* targetModel = nullptr;
    MinecraftInstance* targetInstance = nullptr;
    std::string screenTitle = "Modrinth";
    // Pagination — Modrinth's search endpoint takes a plain offset/limit
    // pair, not a page token (getSearchURL() in ModrinthAPI.h hardcodes
    // limit=25 itself, not exposed via SearchArgs — kModrinthPageSize
    // below just mirrors that constant for our own offset math). offset
    // is the current page's starting index (0 = first page); hasMore is
    // recomputed after every search from whether a *full* page came back
    // (Modrinth doesn't return a total count through the fields this
    // project already parses, so "got fewer than a full page" is the
    // signal for "this is the last page" — same heuristic a lot of
    // "Load More" UIs use when a real total isn't available).
    int offset = 0;
    bool hasMore = false;
};
ModrinthBrowseState g_modrinthBrowse;
constexpr int kModrinthPageSize = 25;

// Keyed by IndexedPack::logoName, matching the desktop's own cache key
// (ModpackListModel::requestLogo()/logoLoaded()). Loading is fire-and-
// forget/async — deliberately NOT run through BigScreenDialogs::WaitForTask
// like every other network call in this file, since blocking once per
// visible pack (up to 25) to show a browse list would freeze the screen
// for several seconds; a generic fallback icon while a real one streams in
// is a much better gamepad experience than a frozen list.
std::unordered_map<QString, std::shared_ptr<GSTexture>> g_modrinthIconCache;
std::unordered_set<QString> g_modrinthIconLoading;

// Fires at most one in-flight NetJob per logo (guarded by
// g_modrinthIconLoading) — same requestLogo()/metaEntryBase() pattern the
// desktop's ModpackListModel uses (ModrinthModel.cpp), just writing into
// this file's own GSTexture cache via BigScreenGui::UploadQImage() instead
// of a QIcon. The NetJob is a plain `new`, parented to nothing — it frees
// itself via deleteLater() from its own succeeded/failed handler, the same
// self-contained lifetime the reference code relies on; no global
// "in-flight jobs" container needed.
void RequestModrinthIcon(const ModPlatform::IndexedPack::Ptr& pack)
{
    const QString logo = pack->logoName;
    if (logo.isEmpty() || pack->logoUrl.isEmpty())
        return;
    if (g_modrinthIconCache.count(logo) || g_modrinthIconLoading.count(logo))
        return;
    g_modrinthIconLoading.insert(logo);

    MetaEntryPtr entry = APPLICATION->metacache()->resolveEntry("ModrinthModpacks", QString("logos/%1").arg(logo));
    auto* job = new NetJob(QString("Modrinth Icon Download %1").arg(logo), APPLICATION->network());
    job->addNetAction(Net::ApiRequest::makeCached(QUrl(pack->logoUrl), entry));

    const QString fullPath = entry->getFullPath();
    QObject::connect(job, &NetJob::succeeded, job, [logo, fullPath, job]() {
        job->deleteLater();
        g_modrinthIconLoading.erase(logo);
        const QImage image(fullPath);
        if (!image.isNull())
            g_modrinthIconCache[logo] = BigScreenGui::UploadQImage(image);
    });
    QObject::connect(job, &NetJob::failed, job, [logo, job](const QString&) {
        job->deleteLater();
        g_modrinthIconLoading.erase(logo);
    });

    job->start();
}

// Installs one chosen Modrinth modpack version as a brand-new instance —
// reuses the exact same InstanceImportTask/ModrinthCreationTask pipeline
// StartZipInstanceImport() already drives for a local .mrpack file
// (ModrinthPage::suggestCurrent() on the desktop builds the identical
// extraInfo map for its own "install from search" flow — confirmed by
// reading InstanceImportTask::processModrinth(), which pulls "pack_id"/
// "pack_version_id" back out of m_extra_info to construct the real
// ModrinthCreationTask internally). setIcon("default") makes
// processModrinth() pull the pack's own logo into the new instance
// automatically (installIcon(), same file) — no separate icon-download/
// cache glue needed for this path.
// Confirmed via reading ModrinthCreationTask::executeTask() that a plain
// install with no "original_instance_id" key takes the
// originalInstanceID().isEmpty() branch straight to createInstance() with
// no native QMessageBox anywhere in between — safe to drive headless/
// gamepad-only exactly like this.
void InstallModrinthPack(const ModPlatform::IndexedPack::Ptr& pack)
{
    const std::string dialogTitle = pack->name.toStdString();

    ResourceAPI::VersionSearchArgs versionArgs;
    versionArgs.pack = pack;
    versionArgs.resourceType = ModPlatform::ResourceType::Modpack;

    QVector<ModPlatform::IndexedVersion> versions;
    bool versionsSucceeded = false;
    QString versionsFailReason;
    Task::Ptr versionsTask = ModrinthAPI::get().getProjectVersions(
        versionArgs, { [&versions, &versionsSucceeded](QVector<ModPlatform::IndexedVersion>& result) {
                          versions = result;
                          versionsSucceeded = true;
                      },
                       [&versionsFailReason](const QString& reason, int) { versionsFailReason = reason; }, [] {} });
    if (!versionsTask) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to load versions for this pack.", false, "OK", "OK");
        return;
    }
    versionsTask->start();
    BigScreenDialogs::WaitForTask(versionsTask.get());

    if (!versionsSucceeded || versions.isEmpty()) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to load versions for this pack: " + versionsFailReason.toStdString(), false, "OK",
                                    "OK");
        return;
    }

    std::vector<std::string> versionLabels;
    for (const ModPlatform::IndexedVersion& v : versions)
        versionLabels.push_back(v.getVersionDisplayString().toStdString());

    const auto versionChoice = BigScreenDialogs::Choose(dialogTitle, versionLabels);
    if (!versionChoice || *versionChoice < 0 || static_cast<size_t>(*versionChoice) >= static_cast<size_t>(versions.size()))
        return;
    const ModPlatform::IndexedVersion& chosenVersion = versions[static_cast<int>(*versionChoice)];

    const auto name = BigScreenDialogs::InputString("Instance Name", "Enter a name for the new instance", pack->name.toStdString());
    if (!name || name->isEmpty())
        return;

    QMap<QString, QString> extraInfo{ { "pack_id", pack->addonId.toString() }, { "pack_version_id", chosenVersion.fileId.toString() } };
    auto* importTask = new InstanceImportTask(QUrl(chosenVersion.downloadUrl), true, nullptr, extraInfo);
    importTask->setName(*name);
    importTask->setIcon("default");

    unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(importTask));
    task->start();
    BigScreenDialogs::WaitForTask(task.get());

    if (!task->wasSuccessful())
        BigScreenDialogs::Confirm(dialogTitle, "Failed to install modpack: " + task->failReason().toStdString(), false, "OK", "OK");
}

// Mirrors ModFilterWidget::prepareBasicFilter()'s two defaults (the only
// part of that Qt-widget class actually needed here) — only meaningful
// for Mod (see both call sites below for why the other resource types
// don't use this). Shared by RunModrinthSearch() (filters what appears in
// the browse list itself, matching ModModel::createSearchArguments() on
// the desktop) and InstallModrinthResource() (filters the version list
// for one already-chosen pack, matching createVersionsArguments()) — a
// real gap caught by testing, not designed in from the start: an earlier
// version of this code only filtered the version list, so the browse
// list itself still showed e.g. Fabric-only mods (Sodium, Fabric API) for
// a NeoForge instance — confirmed live via a screenshot before this fix.
struct ModCompatFilter {
    std::optional<std::vector<Version>> versions;
    std::optional<ModPlatform::ModLoaderTypes> loaders;
};
ModCompatFilter GetModCompatibilityFilter(MinecraftInstance* inst)
{
    ModCompatFilter filter;
    if (!inst)
        return filter;

    // Same real bug DrawInstanceSettingsVersion() already had to work
    // around (see its own comment) — PackProfile doesn't auto-load, so
    // getComponentVersion()/getSupportedModLoaders() silently return
    // empty for an instance whose Version tab was never opened first.
    // Confirmed live via a temporary diagnostic: without this,
    // GetModCompatibilityFilter() derived an empty filter for the real
    // "automodpack" instance (NeoForge/1.21.1) even though its
    // mmc-pack.json is fully populated on disk — the object just hadn't
    // been parsed into yet. Same "once per distinct pointer" caching.
    PackProfile* profile = inst->getPackProfile();
    static PackProfile* lastProfile = nullptr;
    if (profile != lastProfile) {
        profile->reload(Net::Mode::Online);
        lastProfile = profile;
    }

    const QString mcVersion = profile->getComponentVersion("net.minecraft");
    if (!mcVersion.isEmpty())
        filter.versions = std::vector<Version>{ Version(mcVersion) };
    filter.loaders = profile->getSupportedModLoaders();
    return filter;
}

// Installs one chosen Modrinth resource (Mod/Resource/Shader/Data Pack)
// into g_modrinthBrowse.targetModel — the current instance's already-
// existing mods/resourcepacks/shaderpacks/datapacks folder, NOT a new
// instance (that's InstallModrinthPack() above, for Modpack only).
// Real desktop equivalent: ResourceDownloadDialog::addResource() +
// ResourceModel::addPack(), minus the QWidget dialog/ModFilterWidget/
// GetModDependenciesTask machinery around it — confirmed by reading
// ResourceDownloadTask.h/.cpp directly that dependency resolution lives
// entirely OUTSIDE this task (a separate opt-out convenience the desktop
// only runs for Mods, gated behind ResourceDownloadDialog::confirm()) and
// isn't needed for a correct, if manual, install: skipping it just means
// a mod that needs another mod won't load until the user installs that
// one too — identical to what already happens if someone drops a file
// into the folder by hand, the status quo this replaces.
void InstallModrinthResource(const ModPlatform::IndexedPack::Ptr& pack)
{
    ResourceFolderModel* model = g_modrinthBrowse.targetModel;
    MinecraftInstance* inst = g_modrinthBrowse.targetInstance;
    if (!model || !inst)
        return;

    const std::string dialogTitle = pack->name.toStdString();

    ResourceAPI::VersionSearchArgs versionArgs;
    versionArgs.pack = pack;
    versionArgs.resourceType = g_modrinthBrowse.resourceType;
    // Only Mods get filtered by MC version/loader compatibility — confirmed
    // by reading ModModel.cpp vs. ResourcePackResourceModel.cpp/
    // ShaderPackResourceModel.cpp/DataPackResourceModel.cpp directly: the
    // other three pass empty loaders/versions unconditionally, matching
    // the desktop's own behavior (it doesn't protect the user there
    // either — a resource pack/shader/data pack has no "loader", and MC
    // version compatibility for those is looser/self-evident from the
    // pack's own description in practice). This mirrors
    // ModFilterWidget::prepareBasicFilter()'s two defaults, the only part
    // of that Qt-widget class actually needed here.
    if (g_modrinthBrowse.resourceType == ModPlatform::ResourceType::Mod) {
        const ModCompatFilter filter = GetModCompatibilityFilter(inst);
        versionArgs.mcVersions = filter.versions;
        versionArgs.loaders = filter.loaders;
    }

    QVector<ModPlatform::IndexedVersion> versions;
    bool versionsSucceeded = false;
    QString versionsFailReason;
    Task::Ptr versionsTask = ModrinthAPI::get().getProjectVersions(
        versionArgs, { [&versions, &versionsSucceeded](QVector<ModPlatform::IndexedVersion>& result) {
                          versions = result;
                          versionsSucceeded = true;
                      },
                       [&versionsFailReason](const QString& reason, int) { versionsFailReason = reason; }, [] {} });
    if (!versionsTask) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to load versions for this pack.", false, "OK", "OK");
        return;
    }
    versionsTask->start();
    BigScreenDialogs::WaitForTask(versionsTask.get());

    if (!versionsSucceeded || versions.isEmpty()) {
        BigScreenDialogs::Confirm(dialogTitle,
                                    "No compatible versions found for this instance: " + versionsFailReason.toStdString(), false, "OK",
                                    "OK");
        return;
    }

    std::vector<std::string> versionLabels;
    for (const ModPlatform::IndexedVersion& v : versions)
        versionLabels.push_back(v.getVersionDisplayString().toStdString());

    const auto versionChoice = BigScreenDialogs::Choose(dialogTitle, versionLabels);
    if (!versionChoice || *versionChoice < 0 || static_cast<size_t>(*versionChoice) >= static_cast<size_t>(versions.size()))
        return;
    const ModPlatform::IndexedVersion& chosenVersion = versions[static_cast<int>(*versionChoice)];

    unique_qobject_ptr<Task> task(new ResourceDownloadTask(pack, chosenVersion, model));
    task->start();
    BigScreenDialogs::WaitForTask(task.get());

    if (!task->wasSuccessful()) {
        BigScreenDialogs::Confirm(dialogTitle, "Failed to download: " + task->failReason().toStdString(), false, "OK", "OK");
        return;
    }
    model->update();
}

// Runs (or re-runs, for a new search term or page) the actual Modrinth
// query and fills g_modrinthBrowse — shared by the initial browse-by-
// downloads load and by DrawModrinthBrowse()'s "Search"/"Clear search"/
// page-navigation actions. query empty means "no search term" (browse by
// downloads — see ModrinthAPI::getSearchURL(), which only appends
// query=... when args.search.has_value()). offset selects the page (0 =
// first); defaults to 0 so every *new* search (a fresh query, or clearing
// one) naturally starts back at page 1 — only the explicit page-nav
// actions in DrawModrinthBrowse() pass a nonzero offset while keeping the
// same query. Searches under g_modrinthBrowse.resourceType, set by the
// caller (StartModrinthBrowse()/StartResourceBrowse()) before this runs —
// Mod/ResourcePack/ShaderPack/DataPack all reuse this unchanged, just a
// different args.type (confirmed by reading Modrinth's own search-result
// parser, Modrinth::loadIndexedPack() — resource-type agnostic, same
// name/description/logoName/logoUrl fields for every type).
void RunModrinthSearch(const QString& query, int offset = 0)
{
    g_modrinthBrowse.query = query;
    g_modrinthBrowse.offset = offset;
    g_modrinthBrowse.packs.clear();
    g_modrinthBrowse.hasMore = false;
    g_modrinthBrowse.lastError.clear();

    ResourceAPI::SearchArgs args;
    args.type = g_modrinthBrowse.resourceType;
    args.offset = offset;
    args.sorting = ResourceAPI::SortingMethod{ .index = 2, .name = "downloads", .readableName = {} };
    if (!query.isEmpty())
        args.search = query;
    // See ModCompatFilter's comment — Mods only, matching
    // ModModel::createSearchArguments() on the desktop, so an
    // incompatible mod (wrong loader/MC version) doesn't even show up in
    // the browse list, not just fail later when a version is chosen.
    if (g_modrinthBrowse.resourceType == ModPlatform::ResourceType::Mod && g_modrinthBrowse.targetInstance) {
        const ModCompatFilter filter = GetModCompatibilityFilter(g_modrinthBrowse.targetInstance);
        args.versions = filter.versions;
        args.loaders = filter.loaders;
    }

    QList<ModPlatform::IndexedPack::Ptr> packs;
    bool succeeded = false;
    QString failReason;
    Task::Ptr searchTask = ModrinthAPI::get().searchProjects(
        args, { [&packs, &succeeded](QList<ModPlatform::IndexedPack::Ptr>& result) {
                   packs = result;
                   succeeded = true;
               },
                [&failReason](const QString& reason, int) { failReason = reason; }, [] {} });
    if (!searchTask) {
        g_modrinthBrowse.lastError = "Failed to search Modrinth. Check your internet connection.";
        return;
    }
    searchTask->start();
    BigScreenDialogs::WaitForTask(searchTask.get());

    if (!succeeded) {
        g_modrinthBrowse.lastError = "Failed to search Modrinth: " + failReason;
        return;
    }
    g_modrinthBrowse.packs = packs;
    g_modrinthBrowse.hasMore = packs.size() >= kModrinthPageSize;
    if (packs.isEmpty())
        g_modrinthBrowse.lastError = offset > 0 ? "No more results." : "No results found.";
}

// Y → Add Instance → "Modrinth" opens this — runs the initial
// browse-by-downloads query, then switches to Screen::ModrinthBrowse
// regardless of success/failure (the screen itself shows
// g_modrinthBrowse.lastError inline when the list is empty, same as any
// other real-data list screen in this file — Servers/Worlds/etc. all show
// their own "nothing here" text rather than a separate error dialog).
void StartModrinthBrowse()
{
    g_modrinthBrowse.resourceType = ModPlatform::ResourceType::Modpack;
    g_modrinthBrowse.targetModel = nullptr;
    g_modrinthBrowse.targetInstance = nullptr;
    g_modrinthBrowse.screenTitle = TR("ModrinthPage", "Modrinth").toStdString();
    RunModrinthSearch(QString());
    SetScreen(Screen::ModrinthBrowse);
}

// X → "Download ..." on any of the Mods/Resource/Shader/Data Packs
// Instance Settings tabs opens this — same browse screen as
// StartModrinthBrowse(), just targeting one resource type + the current
// instance's matching *List() model instead of creating a whole new
// instance. screenTitle reuses the real "Download Mods"/"Download Packs"
// text each DrawInstanceSettingsXxx() wrapper already passes into
// ShowResourceActionsMenu() for its menu entry — same string, now also
// used as this screen's title.
void StartResourceBrowse(MinecraftInstance* inst, ResourceFolderModel* model, ModPlatform::ResourceType type, const QString& screenTitle)
{
    g_modrinthBrowse.resourceType = type;
    g_modrinthBrowse.targetModel = model;
    g_modrinthBrowse.targetInstance = inst;
    g_modrinthBrowse.screenTitle = screenTitle.toStdString();
    RunModrinthSearch(QString());
    SetScreen(Screen::ModrinthBrowse);
}

// Scrollable list of real Modrinth modpacks — icon (streamed in async via
// RequestModrinthIcon(), falling back to the generic instances icon until
// it arrives), name, and author+description as the row's title/summary.
// A confirms (installs via InstallModrinthPack(), deferred through
// g_pendingAction since this fires from inside the list's own draw loop —
// same reentrancy reasoning as every X/Y menu action in this file), Y
// opens a search prompt.
// Returns where B/"< Back" should go: Instances for a modpack browse
// (StartModrinthBrowse(), reached from Y on Instances), or back to the
// Instance Settings tab it was opened from for any single-resource browse
// (StartResourceBrowse(), reached from X → "Download ..." on Mods/
// Resource/Shader/Data Packs).
Screen ModrinthBrowseBackTarget()
{
    return g_modrinthBrowse.resourceType == ModPlatform::ResourceType::Modpack ? Screen::Instances : Screen::InstanceSettings;
}

void DrawModrinthBrowse()
{
    const int currentPage = g_modrinthBrowse.offset / kModrinthPageSize + 1;
    const bool canGoPrev = g_modrinthBrowse.offset > 0;
    const bool canGoNext = g_modrinthBrowse.hasMore;

    {
        const GamepadGlyphs glyphs = GetGamepadGlyphs();
        SetFooterHints({ { glyphs.confirm(false), "Install" },
                          { glyphs.north, "Search" },
                          { ICON_PF_XBOX_LB "/" ICON_PF_XBOX_RB, "Page" },
                          { glyphs.cancel(false), "Back" } });
    }

    const bool anyDialogOpen = IsChoiceDialogOpen() || IsInputDialogOpen() || IsMessageBoxDialogOpen() || IsFileSelectorOpen();

    // Deliberately checked before BeginScreen()/drawing — same ordering
    // DrawSettings()/DrawInstanceSettings() use for their own LB/RB tab
    // switching, so this frame's content already reflects the new page
    // rather than lagging a frame behind. Deferred through g_pendingAction
    // since RunModrinthSearch() blocks (WaitForTask) and this check runs
    // mid-frame, same reasoning as the Y-search action below.
    if (!anyDialogOpen) {
        if (canGoNext && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
            const int nextOffset = g_modrinthBrowse.offset + kModrinthPageSize;
            g_pendingAction = [nextOffset]() { RunModrinthSearch(g_modrinthBrowse.query, nextOffset); };
        } else if (canGoPrev && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
            const int prevOffset = std::max(0, g_modrinthBrowse.offset - kModrinthPageSize);
            g_pendingAction = [prevOffset]() { RunModrinthSearch(g_modrinthBrowse.query, prevOffset); };
        }
    }

    if (BeginScreen(g_modrinthBrowse.screenTitle.c_str())) {
        if (BeginFullscreenColumnWindow(0.0f, 0.0f, "modrinth_browse")) {
            BeginMenuButtons();
            ResetFocusHere();

            if (MenuButtonWithoutSummary(ICON_FA_CHEVRON_LEFT " Back"))
                SetScreen(ModrinthBrowseBackTarget());

            if (!g_modrinthBrowse.query.isEmpty()) {
                const QString clearLabel = QString("\xC3\x97 ") + QObject::tr("Clear search (\"%1\")").arg(g_modrinthBrowse.query);
                if (MenuButtonWithoutSummary(clearLabel.toUtf8().constData()))
                    g_pendingAction = []() { RunModrinthSearch(QString()); };
            }

            // Page indicator — no real total-page count available (see
            // ModrinthBrowseState's comment on hasMore), so this reads
            // "Page N" rather than "Page N of M", plus which direction(s)
            // LB/RB currently do something. Uses ICON_FA_CHEVRON_LEFT/
            // _RIGHT (already loaded into the font atlas and already used
            // elsewhere in this file, e.g. the "< Back" buttons) rather
            // than the raw Unicode "◀"/"▶" (U+25C0/U+25B6, Geometric
            // Shapes block) an earlier version of this used — those were
            // never in any loaded glyph range (only Basic Latin/Latin-1 +
            // explicit Cyrillic + the FA/PromptFont icon ranges are — see
            // GuiManager.cpp's LoadFonts()), so they rendered as missing-
            // glyph boxes instead of real arrows. Built as a std::string
            // rather than QString::arg() — the icon macros are plain
            // const char* byte literals, awkward to interleave with
            // QString's %N substitution.
            {
                std::string label = QObject::tr("Page %1").arg(currentPage).toStdString();
                if (canGoPrev && canGoNext) {
                    label += "  (";
                    label += ICON_FA_CHEVRON_LEFT;
                    label += " LB   RB ";
                    label += ICON_FA_CHEVRON_RIGHT;
                    label += ")";
                } else if (canGoPrev) {
                    label += "  (";
                    label += ICON_FA_CHEVRON_LEFT;
                    label += " LB)";
                } else if (canGoNext) {
                    label += "  (RB ";
                    label += ICON_FA_CHEVRON_RIGHT;
                    label += ")";
                }
                ImGui::TextUnformatted(label.c_str());
            }

            if (g_modrinthBrowse.packs.isEmpty() && !g_modrinthBrowse.lastError.isEmpty())
                ImGui::TextUnformatted(g_modrinthBrowse.lastError.toUtf8().constData());

            for (const ModPlatform::IndexedPack::Ptr& pack : g_modrinthBrowse.packs) {
                RequestModrinthIcon(pack);

                GSTexture* icon = nullptr;
                if (auto it = g_modrinthIconCache.find(pack->logoName); it != g_modrinthIconCache.end())
                    icon = it->second.get();
                if (!icon)
                    icon = GetCachedTexture("images/icons/instances.png");

                const QByteArray nameUtf8 = pack->name.toUtf8();
                QString summary = pack->description;
                if (!pack->authors.isEmpty())
                    summary = pack->authors.first().name + " — " + summary;
                const QByteArray summaryUtf8 = summary.toUtf8();

                ImGui::PushID(pack->addonId.toString().toUtf8().constData());
                const bool pressed = icon != nullptr
                    ? MenuImageButton(nameUtf8.constData(), summaryUtf8.constData(),
                                       static_cast<ImTextureID>(icon->GetNativeHandle()), LayoutScale(56.0f, 56.0f), true, 66.0f)
                    : MenuButton(nameUtf8.constData(), summaryUtf8.constData());
                ImGui::PopID();

                if (pressed) {
                    ModPlatform::IndexedPack::Ptr chosen = pack;
                    const bool isModpack = g_modrinthBrowse.resourceType == ModPlatform::ResourceType::Modpack;
                    g_pendingAction = [chosen, isModpack]() {
                        if (isModpack)
                            InstallModrinthPack(chosen);
                        else
                            InstallModrinthResource(chosen);
                    };
                }
            }

            EndMenuButtons();
        }
        EndFullscreenColumnWindow();
    }
    EndFullscreenColumns();

    if (!anyDialogOpen && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
        g_pendingAction = []() {
            const auto query =
                BigScreenDialogs::InputString(g_modrinthBrowse.screenTitle, "Search", g_modrinthBrowse.query.toStdString());
            if (query)
                RunModrinthSearch(*query);
        };
    }
}

// Y on the Instances screen opens this. Three creation methods wired up now
// (Vanilla Minecraft, local zip/.mrpack import, online Modrinth browsing —
// see StartZipInstanceImport()/StartModrinthBrowse()'s comments) —
// structured as a proper menu so adding more (CurseForge, needs an API
// key — still not started) later is just more entries, not a redesign.
// Same non-blocking OpenChoiceDialog reasoning as ShowInstanceActionsMenu
// above.
void ShowAddInstanceMenu()
{
    ChoiceDialogOptions options;
    options.emplace_back("Vanilla Minecraft", false);
    options.emplace_back(TR("ImportPage", "Import").toStdString(), false);
    options.emplace_back(TR("ModrinthPage", "Modrinth").toStdString(), false);

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
                          } else if (index == 1) {
                              g_pendingAction = []() {
                                  CloseChoiceDialog();
                                  StartZipInstanceImport();
                              };
                          } else if (index == 2) {
                              g_pendingAction = []() {
                                  CloseChoiceDialog();
                                  StartModrinthBrowse();
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

            if (count == 0) {
                const char* text = "No instances yet — press Y to add one.";
                const ImVec2 textSize = g_medium_font.first->CalcTextSizeA(g_medium_font.second, FLT_MAX, 0.0f, text);
                ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f));
                ImGui::PushFont(g_medium_font.first, g_medium_font.second);
                ImGui::TextUnformatted(text);
                ImGui::PopFont();
            }

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

    // Real user report, root-caused by reading Dear ImGui's own source
    // (imgui_internal.h): "ImGuiKey_NavGamepadMenu" — Dear ImGui's own
    // built-in "hold to open a window-switcher overlay" shortcut — is
    // #define'd directly to ImGuiKey_GamepadFaceLeft, the exact same
    // physical button BigScreen uses everywhere for its own "X: Actions"
    // context menu. With ImGuiConfigFlags_NavEnableGamepad on (needed for
    // any gamepad nav at all), Dear ImGui's NavUpdateWindowing() reacts to
    // that same held button on its own, showing its built-in window-list
    // popup for as long as it's held — reported live as "holding X
    // anywhere briefly shows some popup" — and, since BigScreen's own
    // choice dialog only actually appears a frame or more after the press
    // (deferred via g_pendingAction/CloseChoiceDialog for reentrancy, see
    // DialogHelpers.h), a normal human button-hold duration is long enough
    // for ImGui's own windowing to activate first and interfere with (or
    // mask) BigScreen's own menu — matching the "X menu doesn't open"
    // half of the same report. This built-in feature (an Alt-Tab-style
    // switcher between simultaneously open ImGui windows) has no meaning
    // for BigScreen's own single-active-screen architecture, so it's
    // disabled outright rather than fighting a random hold-duration race
    // against it. ConfigNavWindowingWithGamepad lives on the *internal*
    // ImGuiContext (imgui_internal.h — no public ImGuiIO field controls
    // this specifically), transitively already included here via
    // ImGuiFullscreen.h.
    ImGui::GetCurrentContext()->ConfigNavWindowingWithGamepad = false;

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

    // Bridges LaunchController::offerToOpenAccountManager() (see
    // BigScreenLaunchController's override) — fires when a launch is
    // attempted with zero valid accounts and the user says they want to
    // add one now.
    BigScreenLaunchController::onOpenAccounts = []() { SetScreen(Screen::Accounts); };

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
    SDL_SetWindowFullscreen(window, WantFullscreen() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

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

    // Kept diagnostic (like BIGSCREEN_AUTOLAUNCH): BIGSCREEN_TEST_IMPORT=
    // <path to a zip> runs the exact same InstanceImportTask pipeline
    // StartZipInstanceImport() does — wrapInstanceTask()/start()/
    // WaitForTask() — skipping only the interactive file-selector/name-
    // prompt steps (name is fixed to a "-DELETE-ME" suffixed one so it's
    // unmistakable and safe to clean up immediately after, same pattern as
    // this project's earlier safe destructive-operation tests). Logs
    // success/failure and the resulting instance count so a real .zip's
    // outcome (even an expected failure, e.g. a plain world-save zip that
    // isn't a valid pack format) can be confirmed without needing gamepad/
    // keyboard input — this is exactly what confirmed the import pipeline
    // itself works end-to-end (real translated failReason, zero stray
    // instances) independent from the file-selector UI.
    if (const char* importPath = std::getenv("BIGSCREEN_TEST_IMPORT")) {
        QTimer::singleShot(500, &app, [path = QString::fromUtf8(importPath)]() {
            const int countBefore = APPLICATION->instances()->rowCount();
            SDL_Log("[test-import] instance count before: %d", countBefore);

            auto* importTask = new InstanceImportTask(QUrl::fromLocalFile(path), false, nullptr, {});
            importTask->setName("BigScreenTestImport-DELETE-ME");

            unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(importTask));
            task->start();
            BigScreenDialogs::WaitForTask(task.get());

            const int countAfter = APPLICATION->instances()->rowCount();
            SDL_Log("[test-import] wasSuccessful=%d failReason='%s' countAfter=%d", task->wasSuccessful(),
                    task->failReason().toUtf8().constData(), countAfter);

            if (task->wasSuccessful()) {
                InstanceList* instances = APPLICATION->instances();
                for (int i = 0; i < instances->rowCount(); ++i) {
                    MinecraftInstance* inst = instances->at(i);
                    if (inst && inst->name() == "BigScreenTestImport-DELETE-ME") {
                        SDL_Log("[test-import] cleaning up test instance");
                        instances->trashInstance(inst->id());
                        break;
                    }
                }
            }
        });
    }

    // Kept diagnostic (like BIGSCREEN_AUTOLAUNCH/BIGSCREEN_TEST_IMPORT):
    // BIGSCREEN_TEST_COPY=<source instance id> runs the same
    // InstanceCopyTask pipeline the "Copy..." action does, skipping only
    // the interactive name prompt (fixed to a "-DELETE-ME" name, cleaned
    // up immediately after — same safe pattern as BIGSCREEN_TEST_IMPORT
    // above). Confirmed live: real instance count 8->9->8, zero residue
    // left on disk after cleanup.
    if (const char* copySourceId = std::getenv("BIGSCREEN_TEST_COPY")) {
        QTimer::singleShot(500, &app, [id = QString::fromUtf8(copySourceId)]() {
            MinecraftInstance* source = APPLICATION->instances()->getInstanceById(id);
            if (!source) {
                SDL_Log("[test-copy] no such instance: %s", qUtf8Printable(id));
                return;
            }
            const int countBefore = APPLICATION->instances()->rowCount();
            SDL_Log("[test-copy] copying '%s', instance count before: %d", qUtf8Printable(source->name()), countBefore);

            auto* copyTask = new InstanceCopyTask(source, InstanceCopyPrefs());
            copyTask->setName("BigScreenTestCopy-DELETE-ME");
            copyTask->setGroup(APPLICATION->instances()->getInstanceGroup(source->id()));
            copyTask->setIcon(source->iconKey());

            unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(copyTask));
            task->start();
            BigScreenDialogs::WaitForTask(task.get());

            const int countAfter = APPLICATION->instances()->rowCount();
            SDL_Log("[test-copy] wasSuccessful=%d failReason='%s' countAfter=%d", task->wasSuccessful(),
                    task->failReason().toUtf8().constData(), countAfter);

            if (task->wasSuccessful()) {
                InstanceList* instances = APPLICATION->instances();
                for (int i = 0; i < instances->rowCount(); ++i) {
                    MinecraftInstance* inst = instances->at(i);
                    if (inst && inst->name() == "BigScreenTestCopy-DELETE-ME") {
                        SDL_Log("[test-copy] cleaning up test instance");
                        instances->trashInstance(inst->id());
                        break;
                    }
                }
            }
        });
    }

    // Kept diagnostic (like BIGSCREEN_TEST_COPY/BIGSCREEN_TEST_IMPORT):
    // BIGSCREEN_TEST_EXPORT=<source instance id>:<output dir> runs the
    // same MMCZip::ExportToZipTask pipeline StartInstanceExport() does,
    // skipping only the interactive file-selector step. Confirmed live: a
    // real small instance exported to a valid, correctly-structured zip
    // (verified with `unzip -l` — real minecraft/config, minecraft/logs,
    // instance.cfg, mmc-pack.json all present), with zero side effects on
    // the source instance's own folder (its icon is built-in, so the
    // icon-copy branch correctly no-ops).
    if (const char* exportSpec = std::getenv("BIGSCREEN_TEST_EXPORT")) {
        QTimer::singleShot(500, &app, [spec = QString::fromUtf8(exportSpec)]() {
            const int sep = spec.lastIndexOf(':');
            if (sep < 0) {
                SDL_Log("[test-export] bad spec, expected <instance id>:<output dir>");
                return;
            }
            const QString id = spec.left(sep);
            const QString dir = spec.mid(sep + 1);

            MinecraftInstance* inst = APPLICATION->instances()->getInstanceById(id);
            if (!inst) {
                SDL_Log("[test-export] no such instance: %s", qUtf8Printable(id));
                return;
            }

            const QString outputPath = FS::PathCombine(dir, FS::RemoveInvalidFilenameChars(inst->name()) + ".zip");
            SDL_Log("[test-export] exporting '%s' to '%s'", qUtf8Printable(inst->name()), qUtf8Printable(outputPath));

            QFileInfoList files;
            if (!MMCZip::collectFileListRecursively(inst->instanceRoot(), nullptr, &files, nullptr)) {
                SDL_Log("[test-export] collectFileListRecursively failed");
                return;
            }
            SDL_Log("[test-export] collected %d files", files.size());

            unique_qobject_ptr<Task> task(new MMCZip::ExportToZipTask(outputPath, inst->instanceRoot(), files, "", true));
            task->start();
            BigScreenDialogs::WaitForTask(task.get());

            const QFileInfo outFile(outputPath);
            SDL_Log("[test-export] wasSuccessful=%d failReason='%s' outputExists=%d outputSize=%lld", task->wasSuccessful(),
                    task->failReason().toUtf8().constData(), outFile.exists(), static_cast<long long>(outFile.size()));
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
            } else if (name == "instance_settings" || name == "instance_settings_java" || name == "instance_settings_commands" ||
                       name == "instance_settings_mods" || name == "instance_settings_resourcepacks" ||
                       name == "instance_settings_shaderpacks" || name == "instance_settings_datapacks" ||
                       name == "instance_settings_worlds" || name == "instance_settings_logs" ||
                       name == "instance_settings_version" || name == "instance_settings_servers" ||
                       name == "instance_settings_screenshots") {
                InstanceList* instances = APPLICATION->instances();
                if (instances->rowCount() > 0) {
                    g_instanceSettingsTarget = instances->at(0);

                    if (name == "instance_settings" || name == "instance_settings_java" || name == "instance_settings_commands") {
                        g_instanceTopTab = 0;  // "Settings"
                        g_instanceSubTab = (name == "instance_settings_java") ? 1 : (name == "instance_settings_commands") ? 3 : 0;
                    } else {
                        // Data Packs specifically needs
                        // "GlobalDataPacksEnabled" on to even appear (see
                        // BuildInstanceTopTabs()) — force it on for this
                        // diagnostic so the tab is actually reachable,
                        // same as a real user flipping the toggle in
                        // Settings would.
                        if (name == "instance_settings_datapacks")
                            g_instanceSettingsTarget->settings()->set("GlobalDataPacksEnabled", true);

                        // Look the target tab up by name in the real,
                        // per-instance tab set (BuildInstanceTopTabs())
                        // rather than a hardcoded index — that set isn't
                        // fixed-size (Data Packs is conditional; Resource
                        // vs. Texture Packs share one slot under different
                        // names), so a hardcoded index would silently
                        // land on the wrong tab whenever that set changes
                        // shape.
                        const char* wantedTab = (name == "instance_settings_mods")            ? "Mods"
                                                 : (name == "instance_settings_resourcepacks") ? nullptr  // matched below, either name
                                                 : (name == "instance_settings_shaderpacks")   ? "Shader Packs"
                                                 : (name == "instance_settings_datapacks")     ? "Data Packs"
                                                 : (name == "instance_settings_worlds")        ? "Worlds"
                                                 : (name == "instance_settings_logs")          ? "Logs"
                                                 : (name == "instance_settings_version")       ? "Version"
                                                 : (name == "instance_settings_servers")       ? "Servers"
                                                 : (name == "instance_settings_screenshots")   ? "Screenshots"
                                                                                                : nullptr;

                        InstanceTopTab tabs[kMaxInstanceTopTabs];
                        const int tabCount = BuildInstanceTopTabs(g_instanceSettingsTarget, tabs);
                        g_instanceTopTab = 0;
                        for (int i = 0; i < tabCount; ++i) {
                            const std::string tabName = tabs[i].name;
                            if ((wantedTab && tabName == wantedTab) ||
                                (name == "instance_settings_resourcepacks" && (tabName == "Resource Packs" || tabName == "Texture Packs"))) {
                                g_instanceTopTab = i;
                                break;
                            }
                        }
                        g_instanceSubTab = 0;
                    }
                    SetScreen(Screen::InstanceSettings);
                }
            } else if (name == "instance_actions") {
                InstanceList* instances = APPLICATION->instances();
                if (instances->rowCount() > 0) {
                    SetScreen(Screen::Instances);
                    ShowInstanceActionsMenu(instances->at(0));
                }
            } else if (name == "add_instance") {
                // Jumps straight to the Y-button "Add Instance" menu,
                // bypassing the Y keypress itself.
                SetScreen(Screen::Instances);
                ShowAddInstanceMenu();
            } else if (name == "modrinth_browse") {
                // Jumps straight to the Modrinth browse screen — runs the
                // real initial browse-by-downloads search itself
                // (StartModrinthBrowse()), for screenshotting the icon/
                // description list without needing gamepad input to
                // navigate Y → "Modrinth" first.
                StartModrinthBrowse();
            } else if (name == "instance_mods_browse") {
                // Same idea, but for the "Download ..." resource-browse
                // path (StartResourceBrowse()) instead of the whole-
                // modpack one — targets the first real instance's Mods
                // list, so the mod-loader/MC-version compatibility filter
                // (InstallModrinthResource()) is exercised against a real
                // instance's real PackProfile, not a mock.
                MinecraftInstance* inst = APPLICATION->instances()->getInstanceById("automodpack");
                if (!inst && APPLICATION->instances()->rowCount() > 0)
                    inst = APPLICATION->instances()->at(0);
                if (inst) {
                    g_instanceSettingsTarget = inst;
                    StartResourceBrowse(inst, inst->loaderModList(), ModPlatform::ResourceType::Mod, TR("ModFolderPage", "Download Mods"));
                }
            } else if (name == "add_instance_import") {
                // Jumps straight to the zip-import file selector — this is
                // exactly what caught the file selector's fixed-size-
                // overflow-at-150%-scale bug (ImGuiFullscreen.cpp's
                // DrawFileSelector()), kept for the same reason
                // instance_settings_* was kept: a real, reusable way to
                // reach this specific dialog for a screenshot.
                SetScreen(Screen::Instances);
                StartZipInstanceImport();
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
        // whatever else is on screen (a native Qt dialog on top of it, a
        // different app) also gets processed here, which reads as BigScreen
        // "stealing" input in the background. Deliberately a *different*
        // signal from SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS above: that
        // hint controls whether SDL's joystick subsystem *delivers*
        // controller events at all (needed because its own internal focus
        // tracking was unreliable under gamescope), while this checks
        // focus before *acting* on events that did arrive.
        //
        // SDL_WINDOW_INPUT_FOCUS alone turned out not to be reliable enough
        // on its own, though — confirmed live (a periodic diagnostic, no
        // other window ever touched) that on a real Wayland/KWin session,
        // once BigScreen actually switches to SDL_WINDOW_FULLSCREEN_DESKTOP
        // (which happens shortly after window creation, via a *second*
        // SDL_SetWindowFullscreen() call — see where BigScreenFullscreen is
        // applied), the flag reads correctly true for the very first sample
        // and then gets stuck false forever after, even with BigScreen the
        // only thing on screen and no competing window ever created — a
        // real quirk in how that compositor's xdg-shell handles the
        // windowed->fullscreen transition's focus renegotiation, not
        // something fixable from this side of the SDL/Wayland boundary.
        //
        // REGRESSION, found later on a real X11 session (confirmed live:
        // launched fullscreen, opened an unrelated konsole window on top —
        // sdlFocused correctly dropped to 0 the moment it stole focus, but
        // the fullscreen branch below was ignoring that flag entirely, so
        // windowFocused stayed 1 and BigScreen kept processing gamepad nav
        // in the background — exactly the "captures input while unfocused"
        // class of bug this whole mechanism exists to prevent). The
        // original fix over-corrected: it blanket-distrusts
        // SDL_WINDOW_INPUT_FOCUS for *every* fullscreen session because one
        // specific compositor's xdg-shell mishandles it, but on X11 (and
        // plausibly other Wayland compositors without that specific KWin
        // bug) the flag tracks real focus changes correctly and dropping it
        // entirely throws away a signal `noNativeDialogActive` alone can't
        // replace — that one only ever catches "a real Qt QWidget dialog is
        // up," never "some unrelated *other* application stole focus while
        // BigScreen is still the fullscreen window."
        //
        // Fixed by gating the distrust on the actual video backend
        // (SDL_GetCurrentVideoDriver(), the same probe the startup
        // [video-diag] log already uses) instead of on fullscreen alone —
        // computed once (the backend can't change mid-session): only
        // "wayland" specifically falls back to the QApplication-only check;
        // every other backend (x11, and presumably any Wayland compositor
        // without KWin's specific xdg-shell quirk) gets the same
        // `sdlFocused && noNativeDialogActive` check windowed mode already
        // used successfully.
        static const bool isWaylandBackend = []() {
            const char* driver = SDL_GetCurrentVideoDriver();
            return driver && std::string_view(driver) == "wayland";
        }();
        const bool isFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
        const bool sdlFocused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool noNativeDialogActive = QApplication::activeWindow() == nullptr;
        const bool windowFocused =
            (isFullscreen && isWaylandBackend) ? noNativeDialogActive : (sdlFocused && noNativeDialogActive);

        // Kept diagnostic: periodic focus-state log independent of gamepad
        // events — this is what caught the isWaylandBackend regression
        // above (real X11 session, sdlFocused correctly dropped to 0 when
        // an unrelated konsole window stole focus, but windowFocused
        // stayed 1 because the old code ignored sdlFocused unconditionally
        // whenever fullscreen). Doesn't need live gamepad activity to
        // trigger, unlike the existing per-event log lines below.
        if (qEnvironmentVariableIsSet("BIGSCREEN_DEBUG_FOCUS")) {
            static double lastLog = 0.0;
            const double now = ImGui::GetTime();
            if (now - lastLog > 1.0) {
                lastLog = now;
                SDL_Log("[focus-diag] isFullscreen=%d sdlFocused=%d noNativeDialogActive=%d windowFocused=%d", isFullscreen, sdlFocused,
                        noNativeDialogActive, windowFocused);
            }
        }

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
                SDL_Log("[gamepad] button down: %d windowFocused=%d", event.cbutton.button, windowFocused);
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
            static bool lastAppliedFullscreen = WantFullscreen();
            const bool wantFullscreen = WantFullscreen();
            if (wantFullscreen != lastAppliedFullscreen) {
                lastAppliedFullscreen = wantFullscreen;
                SDL_SetWindowFullscreen(window, wantFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();

        // Gating the SDL event queue (above) turned out not to be enough on
        // its own, for two stacked reasons — both confirmed live by the
        // same test (a real controller held down while the window lost
        // focus kept driving ImGui NavId changes indefinitely despite the
        // gate):
        //   1. imgui_impl_sdl2's gamepad handling is poll-based, not
        //      event-based — ImGui_ImplSDL2_NewFrame() just above directly
        //      reads the gamepad's *current* raw hardware state via
        //      SDL_GameControllerGetButton()/GetAxis() every frame
        //      (ImGui_ImplSDL2_UpdateGamepads(), called from inside
        //      NewFrame), completely bypassing the SDL event queue and
        //      therefore the isControllerEvent gate entirely.
        //   2. That poll doesn't set io.KeysData[].Down directly either —
        //      it calls io.AddKeyEvent(), which only *queues* the event
        //      (g.InputEventsQueue); the actual Down state isn't updated
        //      until ImGui::NewFrame() (further below) drains that queue.
        //      So calling io.ClearInputKeys() here (which only zeroes the
        //      already-applied KeysData[].Down, confirmed by reading its
        //      implementation) doesn't help on its own: the queued event
        //      from this same frame's poll is still sitting there, and
        //      gets applied moments later when NewFrame() processes it,
        //      silently undoing the clear. io.ClearEventsQueue() is what's
        //      needed too, to drop that queued event before it can ever be
        //      applied. Has to run *every* frame while unfocused, not just
        //      once on the focus-lost transition — the underlying hardware
        //      can still be held the whole time, and the poll re-queues a
        //      fresh event from it every frame regardless.
        if (!windowFocused) {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearEventsQueue();
        }

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
        // at all.
        //
        // Does NOT hold io.AddKeyEvent(key, true) continuously the way an
        // earlier version did, relying on Dear ImGui's own held-key repeat
        // timing (DownDuration-based) — that collided with
        // UpdateGamepads()'s own per-frame poll of the *real* D-pad
        // buttons, which unconditionally re-asserts the real (unpressed,
        // since the user is using the stick, not the D-pad) button state
        // through io.AddKeyEvent() on these exact same keys every single
        // frame. Since the last *applied* Down state (from the previous
        // frame, when this code's own "true" was applied) was true,
        // UpdateGamepads()'s "false" this frame is a genuine queued
        // transition, immediately followed by this code re-asserting
        // "true" — both land in the same frame's input queue, so
        // ImGui::NewFrame() processes false-then-true *every single
        // frame*, resetting DownDuration to ~0 each time instead of letting
        // it accumulate. Confirmed as the actual cause of "the stick reacts
        // way too many times per push": with DownDuration never advancing
        // past the "just pressed" threshold, ImGui's nav system fired a
        // move on every single frame the stick was held (~60/sec) rather
        // than once, then a deliberate delay, then a controlled repeat
        // rate.
        //
        // Fixed by not depending on Dear ImGui's hold-repeat mechanism for
        // the stick at all: this tracks its own held-direction state and
        // repeat timing (via ImGui::GetTime(), the same clock ImGui's own
        // timing uses), and fires each move as a brief true-then-false
        // *pulse* — a single supported keypress, indistinguishable from a
        // real quick tap — at moments this code controls, rather than
        // holding the key down and trusting ImGui to pace repeats while
        // something else keeps re-triggering the down edge underneath it.
        // Only one axis is ever active at a time (whichever is more
        // deflected), not both simultaneously on a diagonal push — nothing
        // in BigScreen's own screens needs true 2D diagonal nav (every list
        // is a single row or a single column), and letting a slightly
        // diagonal push fire both an X and a Y move at once was itself
        // part of "moves further than intended for one push".
        //
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

            ImGuiKey dir = ImGuiKey_None;
            if (std::abs(axisX) >= std::abs(axisY)) {
                if (axisX < -kStickDeadZone)
                    dir = ImGuiKey_GamepadDpadLeft;
                else if (axisX > kStickDeadZone)
                    dir = ImGuiKey_GamepadDpadRight;
            } else {
                if (axisY < -kStickDeadZone)
                    dir = ImGuiKey_GamepadDpadUp;
                else if (axisY > kStickDeadZone)
                    dir = ImGuiKey_GamepadDpadDown;
            }

            static ImGuiKey lastDir = ImGuiKey_None;
            static double nextFireTime = 0.0;
            const double now = ImGui::GetTime();
            constexpr double kInitialDelay = 0.35; // before the first repeat
            constexpr double kRepeatRate = 0.12;   // between repeats while held

            if (dir == ImGuiKey_None) {
                lastDir = ImGuiKey_None;
            } else if (dir != lastDir || now >= nextFireTime) {
                io.AddKeyEvent(dir, true);
                io.AddKeyEvent(dir, false);
                nextFireTime = now + (dir != lastDir ? kInitialDelay : kRepeatRate);
                lastDir = dir;
            }
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
                case Screen::ModrinthBrowse:
                    DrawModrinthBrowse();
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
                SDL_Log("[gamepad] ImGui NavId changed: %u -> %u (source=%d) windowFocused=%d", lastNavId, navId,
                        static_cast<int>(ImGui::GetCurrentContext()->NavInputSource), windowFocused);
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
