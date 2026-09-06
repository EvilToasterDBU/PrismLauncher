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

#include <functional>
#include <memory>
#include <optional>

#include "BaseInstance.h"
#include "InstanceTask.h"
#include "minecraft/MinecraftInstance.h"
#include "modplatform/flame/FileResolvingTask.h"

#include "net/NetJob.h"

#include "ui/dialogs/BlockedModsDialog.h"

class FlameCreationTask : public InstanceTask {
    Q_OBJECT

   public:
    FlameCreationTask(const QString& stagingPath,
                      bool trustedSource,
                      SettingsObject* globalSettings,
                      QWidget* parent,
                      QString id,
                      QString versionId,
                      const QString& originalInstanceId = {})
        : m_parent(parent), m_trustedSource(trustedSource), m_managedId(std::move(id)), m_managedVersionId(std::move(versionId))
    {
        setStagingPath(stagingPath);
        setParentSettings(globalSettings);

        m_originalInstanceId = originalInstanceId;
    }

    bool abort() override;

    void createInstance();
    void executeTask() override;

    // Set once, globally, by a front-end without QWidgets (e.g. BigScreen —
    // see bigscreen/main.cpp's BigScreen{OptionalMod,BlockedMods}Dialog) to
    // replace the two native dialogs below with something else. Unset by
    // default, in which case idResolverSucceeded() runs its original
    // OptionalModDialog/BlockedModsDialog QDialog::exec() calls unchanged —
    // this class was `final` and had no such extension point before; adding
    // these two hooks is the smallest change that avoids every call site
    // needing to know which concrete class to construct (the way e.g.
    // LaunchController's controllerFactory does), since FlameCreationTask is
    // constructed directly in several places (InstanceImportTask's format
    // dispatch among them), not through one factory function.
    //
    // overrideOptionalModDialog: given the optional files' relative paths,
    // returns the subset to actually install, or std::nullopt to abort the
    // whole task (matching OptionalModDialog::exec() == QDialog::Rejected).
    static std::function<std::optional<QStringList>(const QStringList& optionalFiles)> overrideOptionalModDialog;
    // overrideBlockedModsDialog: given the blocked mods list (to be mutated
    // in place exactly like BlockedModsDialog does — matched/localPath/move
    // — copyBlockedMods() reads those same fields afterward regardless of
    // which UI populated them), returns whether to proceed (true) or abort
    // the whole task (false, matching BlockedModsDialog::exec() == 0).
    static std::function<bool(const QString& title, const QString& text, QList<BlockedMod>& mods)> overrideBlockedModsDialog;

   private slots:
    void idResolverSucceeded();
    void setupDownloadJob();
    void copyBlockedMods(const QList<BlockedMod>& blockedMods);
    void validateOtherResources();
    QString getVersionForLoader(const QString& uid, const QString& loaderType, const QString& version, const QString& mcVersion);
    void finishInstall();

   private:
    void setManagedPack(BaseInstance* instance);

    [[nodiscard]] bool promptForUntrustedMods();

   private:
    QWidget* m_parent = nullptr;
    bool m_trustedSource;

    shared_qobject_ptr<Flame::FileResolvingTask> m_modIdResolver;
    Flame::Manifest m_pack;

    // Handle to allow aborting
    Task::Ptr m_processUpdateFileInfoJob = nullptr;
    NetJob::Ptr m_filesJob = nullptr;

    QString m_managedId, m_managedVersionId;

    QList<std::pair<QString, QString>> m_otherResources;

    std::optional<BaseInstance*> m_oldInstance{};
    std::unique_ptr<MinecraftInstance> m_newInstance{};

    QStringList m_selectedOptionalMods;
};
