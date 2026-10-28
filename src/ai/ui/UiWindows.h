#pragma once

/*  How AI tools name and find ASTRA's windows.
 *
 *  Every visible top-level window gets a short id ("w3") the first time a
 *  tool sees it, kept as a dynamic property for the window's lifetime.
 *  Windows that know what they show say so through the properties
 *  "astra.kind" (star_detail, plot_tool, ...) and "astra.starId".           */

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

class QWidget;

namespace astra::ai::ui {

QString  windowId(QWidget* w);
QWidget* windowById(const QString& id);
/// Visible top-level windows, main window first.
QList<QWidget*> visibleWindows();
QJsonObject describeWindow(QWidget* w);
QJsonArray  describeWindows();
/// The modal dialog or popup menu waiting for input, if any.
QWidget*    blockingWidget();
QJsonObject blockingState();
/// True for ASTRA's own AI controls (approval card, chat, AI settings) and
/// everything inside them: agents must never operate those, or they could
/// approve their own requests or raise their own permissions.
bool        isAiInternal(const QWidget* w);
/// A window given by id, or the active/main window when id is empty.
QWidget*    resolveWindow(const QString& id);

} // namespace astra::ai::ui
