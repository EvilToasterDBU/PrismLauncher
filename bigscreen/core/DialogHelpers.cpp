// SPDX-License-Identifier: GPL-3.0-only
#include "DialogHelpers.h"

#include "ImGui/ImGuiFullscreen.h"
#include "tasks/Task.h"

#include <QCoreApplication>

using namespace ImGuiFullscreen;

namespace BigScreenDialogs {

std::function<void()> PumpFrame;
int BlockingDepth = 0;
QString CurrentTaskStatus;
qint64 CurrentTaskProgress = 0;
qint64 CurrentTaskTotalProgress = 0;

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

void Alert(const std::string& title, const std::string& message, const std::string& buttonText)
{
    bool resolved = false;
    OpenInfoMessageDialog(
        title, message, [&resolved] { resolved = true; }, buttonText);
    WaitForDialog(resolved, IsMessageBoxDialogOpen);
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

std::vector<bool> ChooseMultiple(const std::string& title, const std::vector<std::string>& options, std::vector<bool> initiallyChecked)
{
    if (initiallyChecked.size() != options.size())
        initiallyChecked.assign(options.size(), true);

    ChoiceDialogOptions choiceOptions;
    choiceOptions.reserve(options.size());
    for (size_t i = 0; i < options.size(); ++i)
        choiceOptions.emplace_back(options[i], initiallyChecked[i]);

    std::vector<bool> checked = std::move(initiallyChecked);
    bool done = false;

    // index < 0 is the checkable dialog's own "closed via B/Escape without
    // picking a row" signal (DrawChoiceDialog()'s cancel branch) — here
    // that's not a cancel, it's "done, use whatever's checked", so it just
    // ends the wait loop rather than discarding `checked`. A real row
    // toggle (index >= 0) updates that one entry and leaves the dialog
    // open for more.
    OpenChoiceDialog(title, true, std::move(choiceOptions), [&checked, &done](s32 index, const std::string&, bool state) {
        if (index < 0) {
            done = true;
            return;
        }
        if (index >= 0 && static_cast<size_t>(index) < checked.size())
            checked[static_cast<size_t>(index)] = state;
    });

    WaitForDialog(done, IsChoiceDialogOpen);
    return checked;
}

void WaitForTask(Task* task)
{
    BlockingGuard guard;
    while (task->isRunning()) {
        CurrentTaskStatus = task->getStatus();
        CurrentTaskProgress = task->getProgress();
        CurrentTaskTotalProgress = task->getTotalProgress();
        QCoreApplication::processEvents();
        if (PumpFrame)
            PumpFrame();
    }
    CurrentTaskStatus.clear();
    CurrentTaskProgress = 0;
    CurrentTaskTotalProgress = 0;
}

}  // namespace BigScreenDialogs
