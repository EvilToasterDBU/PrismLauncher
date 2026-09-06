// SPDX-License-Identifier: GPL-3.0-only
#include "BigScreenLaunchController.h"
#include "DialogHelpers.h"

#include "Application.h"
#include "minecraft/auth/AccountList.h"
#include "net/NetUtils.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"

#include <QCoreApplication>
#include <QRegularExpression>

std::function<void(MinecraftInstance*)> BigScreenLaunchController::onShowConsole;
std::function<void()> BigScreenLaunchController::onOpenAccounts;
std::function<MinecraftAccountPtr(const QString&)> BigScreenLaunchController::onReauthenticate;

namespace {
// This class's own tr() would use "BigScreenLaunchController" as the
// translation context — a brand-new class with no translations of its own.
// Every string below is copied verbatim from LaunchController.cpp's
// original prompts (the ones these methods override), which already have
// real, existing translations under the "LaunchController" context — this
// looks those up directly instead, so BigScreen picks up the same
// translations the desktop UI already has for this exact wording.
QString LC(const char* sourceText)
{
    return QCoreApplication::translate("LaunchController", sourceText);
}

// Same one-line helper as bigscreen/main.cpp's own StripMnemonic() (internal
// linkage there too, so not shared directly) — the original "&Launch"
// button text carries a Qt '&' mnemonic marker that has no meaning for a
// gamepad-driven button label and would otherwise render as a literal '&'.
QString StripMnemonic(QString text)
{
    return text.remove(QChar('&'));
}
}  // namespace

bool BigScreenLaunchController::askPlayDemo() const
{
    QString message = m_accountToUse
        ? LC("This account does not own Minecraft.\nYou need to purchase the game first to play the full version.")
        : LC("No account was selected for launch.");
    message += LC("\n\nDo you want to play the demo?");
    return BigScreenDialogs::Confirm(LC("Play demo?").toStdString(), message.toStdString(), false, LC("Play Demo").toStdString(),
                                      LC("Cancel").toStdString());
}

QString BigScreenLaunchController::askOfflineName(const QString& playerName, bool* ok)
{
    if (ok != nullptr)
        *ok = false;

    QString title = LC("Player name");
    QString message;
    switch (m_actualLaunchMode) {
        case LaunchMode::Normal:
            Q_ASSERT(false);
            return {};
        case LaunchMode::Demo:
            message = LC("Choose your demo mode player name");
            break;
        case LaunchMode::Offline:
            if (m_wantedLaunchMode == LaunchMode::Normal) {
                const auto netErr = m_accountToUse->accountData()->networkError;
                if (Net::isServerError(netErr)) {
                    title = LC("Auth servers offline");
                    message = LC("The Minecraft authentication servers are currently unavailable, launching in offline mode.\n\n");
                } else {
                    title = LC("No internet connection");
                    message = LC("You are not connected to the Internet, launching in offline mode.\n\n");
                }
            }
            message += LC("Choose your offline mode player name");
            break;
    }

    const QString lastOfflinePlayerName = APPLICATION->settings()->get("LastOfflinePlayerName").toString();
    const QString usedName = lastOfflinePlayerName.isEmpty() ? playerName : lastOfflinePlayerName;

    const auto result = BigScreenDialogs::InputString(title.toStdString(), message.toStdString(), usedName.toStdString(), "OK");
    if (!result || result->isEmpty())
        return {};

    APPLICATION->settings()->set("LastOfflinePlayerName", *result);
    if (ok != nullptr)
        *ok = true;
    return *result;
}

bool BigScreenLaunchController::reauthenticateAccount(const MinecraftAccountPtr& account, const QString& reason)
{
    const bool wantsReauth = BigScreenDialogs::Confirm(
        LC("Account refresh failed").toStdString(), LC("%1. Do you want to reauthenticate this account?").arg(reason).toStdString(),
        true);
    if (!wantsReauth || account->accountType() != AccountType::MSA)
        return false;

    if (!onReauthenticate)
        return false;

    auto* accounts = APPLICATION->accounts();
    const bool isDefault = accounts->defaultAccount() == account;
    // Runs BigScreen's own device-code + QR login (bigscreen/main.cpp's
    // BlockingReauthenticate(), reusing the exact same Screen::AccountLogin
    // flow the Accounts screen's own "+ Add Account" already drives) —
    // blocks (via the same frame-pumping pattern every other override here
    // uses) until the user completes it on another device, fails, or
    // cancels with B. Replaces the earlier fallback to the native
    // MSALoginDialog::newAccount(), which required mouse/keyboard.
    MinecraftAccountPtr newAccount = onReauthenticate(reason);
    if (!newAccount)
        return false;

    accounts->removeAccount(accounts->index(accounts->findAccountByProfileId(account->profileId())));
    accounts->addAccount(newAccount);
    if (isDefault)
        accounts->setDefaultAccount(newAccount);
    if (m_accountToUse == account)
        m_accountToUse = newAccount;

    return true;
}

bool BigScreenLaunchController::confirmKillInstance()
{
    // "Kill"/"Cancel" as the button labels (rather than generic Yes/No) are
    // BigScreen's own choice — the desktop equivalent uses plain
    // QMessageBox::Yes/No here, so there's no existing translation for
    // those two specifically to reuse.
    return BigScreenDialogs::Confirm(
        LC("Kill Minecraft?").toStdString(),
        LC("This can cause the instance to get corrupted and should only be used if Minecraft is frozen for some reason").toStdString(),
        false, "Kill", "Cancel");
}

bool BigScreenLaunchController::waitForTask(Task* task)
{
    // No visible progress UI here (v1 gap — see the BigScreen plan; a small
    // "Downloading..." banner would be nicer) — this just has to keep the
    // render loop alive while `task` (already started by the caller, either
    // an account refresh or a TaskStepWrapper-wrapped launch step such as
    // MinecraftLoadAndCheck / library downloads, which every single launch
    // goes through) runs to completion. The base class's default instead
    // blocks on the real ProgressDialog (QDialog::exec()) — a native QWidget
    // that pops up with a null parent and no way to click its "Abort"
    // button via gamepad, which is why *every* launch looked hung/crashed
    // on this front end specifically. Reached only from within the queued
    // Task::start() dispatch (see Application::launch()'s
    // Qt::QueuedConnection) or Qt's own signal delivery for `finished`/
    // `requestProgress` — never mid-frame — so pumping frames here is safe;
    // see DialogHelpers.h for why that distinction matters.
    BigScreenDialogs::WaitForTask(task);
    return task->getState() != Task::State::AbortedByUser;
}

void BigScreenLaunchController::showInstanceConsole(const QString&)
{
    // No real InstanceWindow QWidget here — BigScreen has none at all.
    // Route to the SDL/ImGui Console screen instead, via the callback
    // main.cpp registered at startup.
    if (onShowConsole)
        onShowConsole(instance());
}

bool BigScreenLaunchController::offerToOpenAccountManager()
{
    const bool wantsToOpen = BigScreenDialogs::Confirm(
        LC("No Accounts").toStdString(),
        LC("In order to play Minecraft, you must have at least one Microsoft "
           "account which owns Minecraft logged in. "
           "Would you like to open the account manager to add an account now?")
            .toStdString(),
        false);
    if (wantsToOpen && onOpenAccounts)
        onOpenAccounts();
    // Unlike the desktop's real "Yes" path — which opens a *modal* Settings
    // dialog that blocks until closed, so decideAccount() can fall through
    // and re-check accounts right after — BigScreen's Screen::Accounts
    // switch doesn't block at all. There's no way to have picked (let alone
    // logged into) an account by the time this returns either way, so this
    // always aborts the current launch attempt; the user just re-launches
    // once they've actually added an account on the Accounts screen.
    return false;
}

MinecraftAccountPtr BigScreenLaunchController::selectAccountToUse(bool* useAsDefault)
{
    if (useAsDefault)
        *useAsDefault = false;

    AccountList* accounts = APPLICATION->accounts();
    const int count = accounts->count();
    if (count == 0)
        return nullptr;

    // Same "Offline account"/"Microsoft account" summary text the Accounts
    // screen's own list already uses (main.cpp) — not from
    // ProfileSelectDialog (a plain QListView with a custom item delegate,
    // no separate translatable summary string to reuse).
    std::vector<std::string> labels;
    std::vector<MinecraftAccountPtr> validAccounts;
    for (int i = 0; i < count; ++i) {
        MinecraftAccountPtr account = accounts->at(i);
        if (!account)
            continue;
        const QString kind = account->accountType() == AccountType::Offline ? QObject::tr("Offline account") : QObject::tr("Microsoft account");
        labels.push_back((account->profileName() + "  \xE2\x80\x94  " + kind).toStdString());
        validAccounts.push_back(account);
    }
    if (labels.empty())
        return nullptr;

    const auto choice = BigScreenDialogs::Choose(LC("Which account would you like to use?").toStdString(), labels);
    if (!choice || *choice < 0 || static_cast<size_t>(*choice) >= validAccounts.size())
        return nullptr;

    // BigScreen has no separate "use as global default" checkbox UI the
    // way ProfileSelectDialog's picker does (GlobalDefaultCheckbox) —
    // always setting the picked account as default is the simplest,
    // lowest-friction choice for a single-user handheld: once you've
    // picked once, launching again shouldn't ask again.
    if (useAsDefault)
        *useAsDefault = true;
    return validAccounts[static_cast<size_t>(*choice)];
}

bool BigScreenLaunchController::checkJvmArgsValid(const QString& jvmargs)
{
    // Same detection regexes as JavaCommon::checkJVMArgs() (JavaCommon.cpp)
    // — real strings copied verbatim from there too, but looked up under
    // context "QObject" rather than LC()'s usual "LaunchController":
    // JavaCommon::checkJVMArgs() is a plain namespace function (not a
    // class member), and its QObject::tr(...) calls — confirmed live via a
    // temporary translation-context probe, not guessed — resolve under
    // "QObject" itself, the same context any tr() call outside an
    // enclosing class ends up filed under.
    static const QRegularExpression memRegex("-Xm[sx]");
    static const QRegularExpression versionRegex("-version:.*");

    if (jvmargs.contains("-XX:PermSize=") || jvmargs.contains(memRegex) || jvmargs.contains("-XX-MaxHeapSize") ||
        jvmargs.contains("-XX:InitialHeapSize")) {
        BigScreenDialogs::Alert(
            QCoreApplication::translate("QObject", "JVM arguments warning").toStdString(),
            QCoreApplication::translate("QObject", "You tried to manually set a JVM memory option (using \"-XX:PermSize\", "
                                                    "\"-XX-MaxHeapSize\", \"-XX:InitialHeapSize\", \"-Xmx\" "
                                                    "or \"-Xms\").\n"
                                                    "There are dedicated boxes for these in the settings (Java tab, in the Memory group at "
                                                    "the top).\n"
                                                    "This message will be displayed until you remove them from the JVM arguments.")
                .toStdString());
        return false;
    }
    if (jvmargs.contains(versionRegex)) {
        BigScreenDialogs::Alert(
            QCoreApplication::translate("QObject", "JVM arguments warning").toStdString(),
            QCoreApplication::translate("QObject", "You tried to pass required Java version argument to the JVM (using "
                                                    "\"-version:xxx\"). This is not safe and will not be "
                                                    "allowed.\n"
                                                    "This message will be displayed until you remove this from the JVM arguments.")
                .toStdString());
        return false;
    }
    return true;
}

void BigScreenLaunchController::profilerCheckFailed(const QString& profilerName, const QString& error)
{
    BigScreenDialogs::Alert(LC("Error!").toStdString(), LC("Profiler check for %1 failed: %2").arg(profilerName, error).toStdString());
}

void BigScreenLaunchController::profilerReadyToLaunch(const QString& message)
{
    // Single-button Alert() rather than Confirm() — the original is an
    // "acknowledge to continue" prompt, not a real yes/no choice (there's
    // no way to *not* proceed here short of aborting the whole launch,
    // which is what B/Confirm's own dialog-close path would do anyway).
    BigScreenDialogs::Alert(LC("Waiting.").toStdString(),
                             LC("The game launch is delayed until you press the "
                                "button. This is the right time to setup the profiler, as the "
                                "profiler server is running now.\n\n%1")
                                 .arg(message)
                                 .toStdString(),
                             StripMnemonic(LC("&Launch")).toStdString());
}

void BigScreenLaunchController::profilerAbortedLaunch(const QString& message)
{
    BigScreenDialogs::Alert(LC("Error").toStdString(), LC("Couldn't start the profiler: %1").arg(message).toStdString());
}
