// SPDX-License-Identifier: GPL-3.0-only
#include "DialogHelpers.h"

#include "ImGui/ImGuiFullscreen.h"
#include "tasks/Task.h"

#include <QCoreApplication>

using namespace ImGuiFullscreen;

namespace BigScreenDialogs {

std::function<void()> PumpFrame;
int BlockingDepth = 0;

namespace {

// Renders frames and pumps Qt's own queued events until `resolved` is set
// (by the dialog's callback firing) or `stillOpen()` reports the dialog
// closed without answering. See DialogHelpers.h for why this isn't a
// QEventLoop::exec() call, and why it holds a BlockingGuard while it spins.
template <typename StillOpenFn>
void WaitForDialog(bool& resolved, StillOpenFn&& stillOpen)
{
    BlockingGuard guard;
    while (!resolved && stillOpen()) {
        QCoreApplication::processEvents();
        if (PumpFrame)
            PumpFrame();
    }
}

}  // namespace

std::optional<QString> InputString(const std::string& title, const std::string& message, const std::string& defaultValue,
                                    const std::string& okButtonText, bool isPassword)
{
    bool resolved = false;
    std::optional<QString> result;

    OpenInputStringDialog(
        title, message, std::string(), okButtonText,
        [&](std::string text) {
            resolved = true;
            result = QString::fromStdString(text);
        },
        defaultValue, isPassword ? InputFilterType::Password : InputFilterType::None);

    WaitForDialog(resolved, IsInputDialogOpen);
    return result;
}

bool Confirm(const std::string& title, const std::string& message, bool defaultValue, const std::string& yesButtonText,
             const std::string& noButtonText)
{
    bool resolved = false;
    bool result = defaultValue;

    OpenConfirmMessageDialog(
        title, message,
        [&](bool yes) {
            resolved = true;
            result = yes;
        },
        defaultValue, yesButtonText, noButtonText);

    WaitForDialog(resolved, IsMessageBoxDialogOpen);
    return result;
}

std::optional<int> Choose(const std::string& title, const std::vector<std::string>& options)
{
    ChoiceDialogOptions choiceOptions;
    choiceOptions.reserve(options.size());
    for (const std::string& option : options)
        choiceOptions.emplace_back(option, false);

    bool resolved = false;
    std::optional<int> result;

    OpenChoiceDialog(title, false, std::move(choiceOptions), [&](s32 index, const std::string&, bool) {
        resolved = true;
        result = index;
    });

    WaitForDialog(resolved, IsChoiceDialogOpen);
    return result;
}

void WaitForTask(Task* task)
{
    BlockingGuard guard;
    while (task->isRunning()) {
        QCoreApplication::processEvents();
        if (PumpFrame)
            PumpFrame();
    }
}

}  // namespace BigScreenDialogs
