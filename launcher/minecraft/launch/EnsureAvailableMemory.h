// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Octol1ttle <l1ttleofficial@outlook.com>
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

#pragma once

#include <functional>

#include "launch/LaunchStep.h"
#include "minecraft/MinecraftInstance.h"

class EnsureAvailableMemory : public LaunchStep {
    Q_OBJECT

   public:
    explicit EnsureAvailableMemory(LaunchTask* parent, MinecraftInstance* instance);
    ~EnsureAvailableMemory() override = default;

    void executeTask() override;
    bool canAbort() const override { return false; }

    // Set once, globally, by a front-end without QWidgets (e.g. BigScreen)
    // to replace the native CustomMessageBox::selectable(...)->exec() below
    // with something else. Given the warning's title and body text, returns
    // true to launch anyway or false to abort — matching the real dialog's
    // own Yes/No outcome. Unset by default, in which case executeTask()
    // shows that dialog unchanged. A plain static callback (same pattern as
    // FlameCreationTask::overrideBlockedModsDialog) rather than a virtual
    // method: this step is constructed directly in
    // MinecraftInstance::createLaunchTask(), not through a factory a
    // front-end could swap.
    static std::function<bool(const QString& title, const QString& text)> overrideLowMemoryDialog;

   private:
    MinecraftInstance* m_instance;
};
