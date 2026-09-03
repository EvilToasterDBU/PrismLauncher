// SPDX-License-Identifier: GPL-3.0-only
#include "BigScreenLaunchController.h"
#include "DialogHelpers.h"

#include "Application.h"
#include "minecraft/auth/AccountList.h"
#include "net/NetUtils.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"
#include "ui/dialogs/MSALoginDialog.h"

std::function<void(MinecraftInstance*)> BigScreenLaunchController::onShowConsole;

bool BigScreenLaunchController::askPlayDemo() const
{
    const std::string message = m_accountToUse
        ? tr("This account does not own Minecraft.\nYou need to purchase the game first to play the full "
             "version.\n\nDo you want to play the demo?")
              .toStdString()
        : tr("No account was selected for launch.\n\nDo you want to play the demo?").toStdString();
    return BigScreenDialogs::Confirm(tr("Play demo?").toStdString(), message, false, tr("Play Demo").toStdString(),
                                      tr("Cancel").toStdString());
}

QString BigScreenLaunchController::askOfflineName(const QString& playerName, bool* ok)
{
    if (ok != nullptr)
        *ok = false;

    QString title = tr("Player name");
    QString message;
    switch (m_actualLaunchMode) {
        case LaunchMode::Normal:
            Q_ASSERT(false);
            return {};
        case LaunchMode::Demo:
            message = tr("Choose your demo mode player name");
            break;
        case LaunchMode::Offline:
            if (m_wantedLaunchMode == LaunchMode::Normal) {
                const auto netErr = m_accountToUse->accountData()->networkError;
                if (Net::isServerError(netErr)) {
                    title = tr("Auth servers offline");
                    message = tr("The Minecraft authentication servers are currently unavailable, launching in offline mode.\n\n");
                } else {
                    title = tr("No internet connection");
                    message = tr("You are not connected to the Internet, launching in offline mode.\n\n");
                }
            }
            message += tr("Choose your offline mode player name");
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
    const bool wantsReauth = BigScreenDialogs::Confirm(tr("Account refresh failed").toStdString(),
                                                        tr("%1. Do you want to reauthenticate this account?").arg(reason).toStdString(),
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
    return BigScreenDialogs::Confirm(
        tr("Kill Minecraft?").toStdString(),
        tr("This can cause the instance to get corrupted and should only be used if Minecraft is frozen for some reason").toStdString(),
        false, tr("Kill").toStdString(), tr("Cancel").toStdString());
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
