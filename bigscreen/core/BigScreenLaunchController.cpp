// SPDX-License-Identifier: GPL-3.0-only
#include "BigScreenLaunchController.h"
#include "DialogHelpers.h"

#include "Application.h"
#include "minecraft/auth/AccountList.h"
#include "net/NetUtils.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"
#include "ui/dialogs/MSALoginDialog.h"

#include <QCoreApplication>

std::function<void(MinecraftInstance*)> BigScreenLaunchController::onShowConsole;

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

    // BigScreen's own device-code + QR login (bigscreen/main.cpp,
    // Screen::AccountLogin) isn't reusable from here without more plumbing
    // to share it between the two files — this path is only hit on token
    // failure, rare enough that falling back to the normal Qt login dialog
    // for the actual re-login form is an acceptable v1 gap. TODO: share it.
    auto* accounts = APPLICATION->accounts();
    const bool isDefault = accounts->defaultAccount() == account;
    MinecraftAccountPtr newAccount = MSALoginDialog::newAccount(nullptr);
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
