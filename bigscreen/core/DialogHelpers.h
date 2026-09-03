// SPDX-License-Identifier: GPL-3.0-only
// Blocking wrappers around ImGuiFullscreen's popup dialogs, for code (like
// LaunchController's overridable prompts — see BigScreenLaunchController)
// that's written to call a dialog and get an answer back synchronously, the
// same way it would call QDialog::exec(). ImGuiFullscreen's dialogs are
// callback-based and only render across future frames, so these need to
// keep BigScreen's own render loop pumping while they wait.
//
// This does NOT use a nested QEventLoop::exec() (an earlier version did,
// following ProgressDialog::execWithTask()'s pattern) — that hangs here.
// BigScreen's render loop isn't driven by app.exec() directly the way the
// normal Qt Widgets UI is; it's a QTimer ticking during it (see
// bigscreen/main.cpp). A nested QEventLoop::exec() pumps that same QTimer,
// which re-enters the *same* frame-rendering call while it's already
// mid-frame (these dialogs are invoked either from inside that very frame
// callback — Settings' pickers — or from LaunchController's callbacks
// during Task::start(), itself reached via app.exec()'s own queue) —
// ImGui doesn't support a NewFrame()/Render() pair nesting inside another,
// and the result is exactly the freeze this was rewritten to fix.
// Instead, PumpFrame (set once from main.cpp) directly re-invokes the same
// frame-rendering function main.cpp's QTimer calls, in a plain loop here,
// with QCoreApplication::processEvents() alongside it to keep Qt's own
// queued signals/slots (network replies, Task::start() itself, ...) moving
// — no event loop nesting, no re-entrancy.
//
// ImGuiFullscreen's dialogs only invoke their callback when the user picks
// an option — closing/cancelling without one (Escape, clicking outside)
// does not call back at all. Each helper here also polls the dialog's
// Is*DialogOpen() so a closed-without-answering dialog still resolves
// (as a cancel) instead of looping forever.
#pragma once

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class Task;

namespace BigScreenDialogs {

// Set once from bigscreen/main.cpp, before anything could call the helpers
// below: renders one SDL/ImGui frame exactly like the normal per-frame
// QTimer tick does (same function, in fact).
extern std::function<void()> PumpFrame;

// >0 while a helper below (or LaunchController::waitForTask — see
// BigScreenLaunchController) is blocking by pumping frames in a loop.
// main.cpp's frame function checks this and, while set, skips the normal
// per-screen input handling (Instances/Settings/etc.) in favor of a plain
// "please wait" placeholder. Without this, the screen underneath stays
// fully interactive during the "blocking" wait — pumping frames does not
// stop SDL_PollEvent from delivering a gamepad press, so e.g. a stray A
// press on the Instances screen while a launch's waitForTask() is already
// pumping frames would fire *another* Application::launch() call on
// whatever instance happens to be focused, recursively nesting a second
// task's own frame-pumping wait inside the first — unbounded, chaotic
// reentrancy against shared state (Application's m_instanceExtras map,
// LaunchTask's step vector, ...), unlike the ImGui NewFrame()/Render()
// reentrancy this design otherwise avoids. The real ProgressDialog this
// replaced didn't have this problem because QDialog::exec() is a genuine
// OS-level modal — nothing else could receive input while it was up.
extern int BlockingDepth;

// RAII helper for the above — increment on construction, decrement on
// destruction, so an early return/exception still balances it.
struct BlockingGuard {
    BlockingGuard() { ++BlockingDepth; }
    ~BlockingGuard() { --BlockingDepth; }
    BlockingGuard(const BlockingGuard&) = delete;
    BlockingGuard& operator=(const BlockingGuard&) = delete;
};

// Returns the entered text, or nullopt if cancelled/closed without input.
std::optional<QString> InputString(const std::string& title,
                                    const std::string& message,
                                    const std::string& defaultValue,
                                    const std::string& okButtonText = "OK");

// Returns the user's choice; if closed without answering, returns
// defaultValue. yesButtonText/noButtonText let a two-choice prompt use
// wording other than literal Yes/No (e.g. "Play Demo" / "Cancel").
bool Confirm(const std::string& title,
             const std::string& message,
             bool defaultValue,
             const std::string& yesButtonText = "Yes",
             const std::string& noButtonText = "No");

// Returns the selected option's index, or nullopt if cancelled/closed.
std::optional<int> Choose(const std::string& title, const std::vector<std::string>& options);

// Blocks (via the same PumpFrame loop as the dialogs above, under the same
// BlockingGuard) until `task` finishes running — for waiting on an
// arbitrary Task, not just a dialog answer. Shared by
// LaunchController::waitForTask (see BigScreenLaunchController) and
// BigScreen's own instance-creation flow (Y: Add Instance in main.cpp),
// which both need to wait for a Task the same way. Does not start the
// task — call task->start() first.
void WaitForTask(Task* task);

}  // namespace BigScreenDialogs
