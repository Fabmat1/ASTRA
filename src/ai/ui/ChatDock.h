#pragma once

/*  The built-in AI chat: a dock on the right of the main window (it can be
 *  floated), with the conversation, the agent's tool calls as expandable
 *  lines, images inline, and a composer at the bottom: the message box
 *  (grows with its text; images can be pasted or dropped in), the
 *  permission mode as a small coloured pill, and send / stop.
 *
 *  Until an endpoint is set up, the chat shows a faded example conversation
 *  with the setup card on top of it, so the user sees what they get.        */

#include "ai/Tool.h"

#include <QDockWidget>
#include <QHash>
#include <QImage>
#include <QPlainTextEdit>
#include <QPointer>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;

namespace astra::ai { class ChatSession; struct LlmUsage; }

namespace astra::ai::ui {

class AiController;

/// The message box: Enter sends, Shift+Enter breaks the line, grows up to a
/// few lines, and takes images from the clipboard or from dropped files.
class ChatInput : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ChatInput(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void sendRequested();
    void imageAdded(const QImage& image);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void fitHeight();
};

class ChatDock : public QDockWidget {
    Q_OBJECT
public:
    ChatDock(AiController* controller, QWidget* parent);

    void focusInput();
    void stop();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    struct Entry {
        enum class Kind { User, Assistant, Tool, Error, Note } kind;
        QString text;          ///< markdown for Assistant, plain otherwise
        QString toolId, tool, args, result;
        bool    toolDone = false, toolOk = true, expanded = false;
        QStringList images;    ///< resource names
    };

    void send();
    void render();
    void scheduleRender();
    void updateState();
    void updateModePill();
    void newChat();
    void showHistoryMenu();
    void showModeMenu();
    void loadConversation(const QString& id);
    void addAttachment(const QImage& image);
    void clearAttachments();
    void placeSetupCard();
    QString addImage(const QImage& image);   ///< returns the resource name
    QString currentContext() const;
    QString entryHtml(const Entry& e) const;
    QString exampleHtml() const;

    AiController*     _controller;
    ChatSession*      _session;
    QWidget*          _body;
    QWidget*          _chatPage;
    QFrame*           _setupCard;
    QTextBrowser*     _view;
    ChatInput*        _input;
    QWidget*          _attachBar;
    QHBoxLayout*      _attachLayout;
    QToolButton*      _sendBtn;
    QToolButton*      _modePill;
    QLabel*           _modelLabel;
    QLabel*           _usageLabel;
    QToolButton*      _historyBtn;
    QToolButton*      _newBtn;
    QTimer*           _renderTimer;
    std::vector<Entry> _entries;
    QHash<QString, int> _toolEntry;
    int               _imageCounter = 0;
    QHash<QString, QImage> _images;
    QList<QImage>     _attachments;
    bool              _ready = false;
    bool              _restyling = false;
};

} // namespace astra::ai::ui
