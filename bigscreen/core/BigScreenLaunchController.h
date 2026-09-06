// SPDX-License-Identifier: GPL-3.0-only
// A LaunchController that answers its own interactive prompts (offline
// player name, play-demo confirmation, account reauthentication, kill
// confirmation) with ImGuiFullscreen dialogs instead of QMessageBox/QDialog
// — see the BigScreen plan (M2's "known gap", closed here). Pass
// BigScreenLaunchController::create as the controllerFactory argument to
// Application::launch().
#pragma once

#include "LaunchController.h"

#include <functional>

class BigScreenLaunchController : public LaunchController {
    Q_OBJECT

   public:
    static LaunchController* create() { return new BigScreenLaunchController(); }

    // Set once from bigscreen/main.cpp so showInstanceConsole() (below) can
    // route into BigScreen's own Console screen — this file has no access
    // to that screen's state (a plain anonymous-namespace global in
    // main.cpp), so a callback is the simplest bridge.
    static std::function<void(MinecraftInstance*)> onShowConsole;
    // Same bridge, for offerToOpenAccountManager() below — routes to
    // BigScreen's own Screen::Accounts.
    static std::function<void()> onOpenAccounts;
    // Same bridge, for reauthenticateAccount() below — runs the same
    // device-code+QR login flow BigScreen's own Accounts screen uses
    // (Screen::AccountLogin, see bigscreen/main.cpp's StartLogin()), but
    // blocking: returns the freshly-logged-in account on success, or
    // nullptr on failure/cancel — matching MSALoginDialog::newAccount()'s
    // contract, which this replaces. Reason is passed through only to seed
    // the status text shown while waiting.
    static std::function<MinecraftAccountPtr(const QString& reason)> onReauthenticate;

   protected:
    bool askPlayDemo() const override;
    QString askOfflineName(const QString& playerName, bool* ok = nullptr) override;
    bool reauthenticateAccount(const MinecraftAccountPtr& account, const QString& reason) override;
    bool confirmKillInstance() override;
    bool waitForTask(Task* task) override;
    void showInstanceConsole(const QString& page = QString()) override;
    bool offerToOpenAccountManager() override;
    MinecraftAccountPtr selectAccountToUse(bool* useAsDefault) override;
    bool checkJvmArgsValid(const QString& jvmargs) override;
    void profilerCheckFailed(const QString& profilerName, const QString& error) override;
    void profilerReadyToLaunch(const QString& message) override;
    void profilerAbortedLaunch(const QString& message) override;
};
