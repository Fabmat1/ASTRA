#pragma once

/*  Helpers shared by the UI tools.
 *
 *  runUiAction() is how a tool presses anything that might open a dialog:
 *  the action runs from the event loop rather than inside the tool call, and
 *  the call finishes when the action returns or as soon as a modal dialog or
 *  popup appears (which would otherwise block it until a human answers). The
 *  result says which windows appeared and what is waiting for input, so the
 *  agent can answer the dialog with dialog_respond / ui_set_value.          */

#include "ai/Tool.h"

#include <QByteArray>
#include <QJsonObject>
#include <functional>

class QWidget;

namespace astra::ai { class AiRuntime; }

namespace astra::ai::ui {

class GuiAiHost;

/// Runs `action` from the event loop and finishes `call` with its result
/// plus "new_windows" and "waiting_for_input". `settleMs` is how long to
/// wait after the action for windows it opened to show.
void runUiAction(const ToolCallPtr& call, std::function<QJsonObject()> action, int settleMs = 300);

/// PNG of a widget, scaled down to at most maxWidth pixels wide.
QByteArray grabPng(QWidget* w, int maxWidth = 1600);

/// Briefly outlines a widget the agent is about to act on, so the user sees it.
void flashWidget(QWidget* w);

/// Registration of the window-side tools.
void registerWindowTools(AiRuntime& rt, GuiAiHost* host);
void registerPlotTools(AiRuntime& rt, GuiAiHost* host);
void registerInspectorTools(AiRuntime& rt, GuiAiHost* host);

} // namespace astra::ai::ui
