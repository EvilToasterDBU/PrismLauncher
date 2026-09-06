// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#pragma once
#include <tools/BaseProfiler.h>

#include "minecraft/MinecraftInstance.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/launch/MinecraftTarget.h"

class InstanceWindow;

enum class LaunchDecision { Undecided, Continue, Abort };

class LaunchController : public Task {
    Q_OBJECT
   public:
    void executeTask() override;

    LaunchController();
    ~LaunchController() override = default;

    void setInstance(MinecraftInstance* instance) { m_instance = instance; }

    MinecraftInstance* instance() const { return m_instance; }

    void setLaunchMode(const LaunchMode mode) { m_wantedLaunchMode = mode; }

    void setOfflineName(const QString& offlineName) { m_offlineName = offlineName; }

    void setProfiler(BaseProfilerFactory* profiler) { m_profiler = profiler; }

    void setParentWidget(QWidget* widget) { m_parentWidget = widget; }

    void setTargetToJoin(MinecraftTarget::Ptr targetToJoin) { m_targetToJoin = std::move(targetToJoin); }

    void setAccountToUse(MinecraftAccountPtr accountToUse) { m_accountToUse = std::move(accountToUse); }

    QString id() const { return m_instance->id(); }

    bool abort() override;

   protected:
    // Overridable so a front-end without QWidgets (e.g. BigScreen) can
    // substitute its own UI for these — see bigscreen/core/BigScreenLaunchController.*.
    // Default implementations are exactly what they always were (QMessageBox
    // et al, parented to m_parentWidget); overriding does not change
    // behavior for the normal Qt Widgets UI.
    virtual bool askPlayDemo() const;
    virtual QString askOfflineName(const QString& playerName, bool* ok = nullptr);
    virtual bool reauthenticateAccount(const MinecraftAccountPtr& account, const QString& reason);
    virtual bool confirmKillInstance();
    // Called from decideLaunchMode() when an account's token refresh is
    // already in progress and launch has to wait for it. Default blocks on
    // the real ProgressDialog (a QDialog::exec()); BigScreen overrides this
    // since a modal QWidget dialog with a null parent and no way to click
    // its "Abort" button via gamepad just makes the launch look hung.
    // Returns false if the wait was aborted (by the user, or however the
    // override chooses to interpret that) — decideLaunchMode() then aborts
    // the whole launch, matching the old inline behavior.
    virtual bool waitForTask(Task* task);
    // Called when the instance's console should become visible to the user
    // (on launch, if ShowConsole is set; on failure, if ShowConsoleOnError
    // is set). Default shows the real InstanceWindow QWidget — BigScreen
    // overrides this since it has no QWidgets at all and already shows a
    // console of its own (Screen::Console); without the override, a
    // genuine native window would pop up over BigScreen's fullscreen SDL
    // window, and closing *that* stray window was taking the whole
    // application down with it via Qt's normal window-close handling.
    virtual void showInstanceConsole(const QString& page = QString());
    // Called from decideAccount() when no valid account exists at all.
    // Default shows the real "No Accounts" QMessageBox and, on Yes, opens
    // the desktop account-manager settings dialog — matching the original
    // inline code exactly (including that the "Yes" path does NOT return
    // early, only "No" does; decideAccount() replicates that by only
    // returning early when this returns false).
    virtual bool offerToOpenAccountManager();
    // Called from decideAccount() when there are valid accounts but none
    // resolved as the one to use (no instance-specific or global default).
    // Default shows the real ProfileSelectDialog. *useAsDefault is set to
    // whether the picked account should become the global default
    // (mirrors ProfileSelectDialog::useAsGlobalDefault()).
    virtual MinecraftAccountPtr selectAccountToUse(bool* useAsDefault);
    // Called first thing in executeTask(), before anything else — checks
    // the instance's JvmArgs for a handful of known-unsafe patterns (manual
    // -Xmx/-Xms/PermSize memory flags that conflict with the dedicated
    // memory settings, or a "-version:" flag). Default calls the real
    // JavaCommon::checkJVMArgs(jvmargs, m_parentWidget), which shows a
    // native warning QMessageBox and returns false if a problem was found.
    virtual bool checkJvmArgsValid(const QString& jvmargs);
    // The three profiler-related prompts in readyForLaunch() (only reached
    // when a launch profiler like JProfiler/JVisualVM is configured for the
    // instance — an advanced, rare setup). Defaults are exactly the
    // original inline QMessageBox calls; BigScreen overrides all three with
    // its own single-button info dialog (see
    // bigscreen/core/BigScreenLaunchController.*), since none of them had
    // ever been touched by the earlier LaunchController virtual-method
    // refactor and previously would have popped up a native, gamepad-
    // unusable QWidget over BigScreen's fullscreen SDL window.
    virtual void profilerCheckFailed(const QString& profilerName, const QString& error);
    virtual void profilerReadyToLaunch(const QString& message);
    virtual void profilerAbortedLaunch(const QString& message);

   private:
    void login();
    void launchInstance();
    void decideAccount();
    LaunchDecision decideLaunchMode();

   private slots:
    void readyForLaunch();

    void onSucceeded();
    void onFailed(QString reason);
    void onProgressRequested(Task* task);

   protected:
    // protected rather than private so overrides of the dialog methods
    // above can make informed decisions (e.g. wording an offline-name
    // prompt around m_actualLaunchMode) — see BigScreenLaunchController.
    LaunchMode m_wantedLaunchMode = LaunchMode::Normal;
    LaunchMode m_actualLaunchMode = LaunchMode::Normal;
    BaseProfilerFactory* m_profiler = nullptr;
    QString m_offlineName;
    MinecraftInstance* m_instance = nullptr;
    QWidget* m_parentWidget = nullptr;
    InstanceWindow* m_console = nullptr;
    MinecraftAccountPtr m_accountToUse = nullptr;
    AuthSessionPtr m_session = nullptr;
    LaunchTask* m_launcher = nullptr;
    MinecraftTarget::Ptr m_targetToJoin = nullptr;
};
