#include "TerminalView.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QKeyEvent>
#include <QTextCursor>

TerminalView::TerminalView(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    _out = new QPlainTextEdit;
    _out->setReadOnly(true);
    _out->setMaximumBlockCount(10000);
    _out->setStyleSheet(
        "QPlainTextEdit { background:#0f1419; color:#e6e1cf;"
        " font-family:monospace; font-size:11px;"
        " border:none; border-radius:0px; }");
    _out->viewport()->installEventFilter(this);
    v->addWidget(_out, 1);

    auto* rowHost = new QWidget;
    rowHost->setStyleSheet("background:#0f1419; border-radius:0px;");
    auto* row = new QHBoxLayout(rowHost);
    row->setContentsMargins(6, 2, 6, 4);
    row->setSpacing(4);

    auto* prompt = new QLabel("isis>");
    prompt->setStyleSheet(
        "QLabel { color:#ffb454; font-family:monospace; font-size:11px; }");
    _in = new QLineEdit;
    _in->setStyleSheet(
        "QLineEdit { background:#0f1419; color:#e6e1cf; border:none;"
        " font-family:monospace; font-size:11px; }");
    _in->installEventFilter(this);

    row->addWidget(prompt);
    row->addWidget(_in, 1);
    v->addWidget(rowHost);

    _histIdx = 0;
    connect(_in, &QLineEdit::returnPressed, this, [this]{
        const QString text = _in->text();
        _in->clear();
        if (!text.isEmpty()) {
            _history.append(text);
            if (_history.size() > 500) _history.removeFirst();
        }
        _histIdx = _history.size();
        _draft.clear();
        emit lineEntered(text);
    });
}

void TerminalView::appendOutput(const QString& text)
{
    if (text.isEmpty()) return;
    _out->moveCursor(QTextCursor::End);
    _out->insertPlainText(text);
    _out->ensureCursorVisible();
}

void TerminalView::appendStatusLine(const QString& s)
{
    appendOutput(QString("\n\x1B[?25h[ASTRA] %1\n").arg(s));
}

void TerminalView::clearOutput() { _out->clear(); }

void TerminalView::setInputEnabled(bool on) { _in->setEnabled(on); }

void TerminalView::focusInput() { _in->setFocus(); }

bool TerminalView::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == _out->viewport() &&
    ev->type() == QEvent::MouseButtonRelease)
    {
        _in->setFocus();
        return false;
    }
    if (obj == _in && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);

        if (_hotkeyMode) {
            if (ke->key() == Qt::Key_Escape) return true;
            if (ke->key() == Qt::Key_Return ||
                ke->key() == Qt::Key_Enter)
            {
                emit lineEntered(QString());   // bare newline
                return true;
            }
            const QString txt = ke->text();
            if (txt.isEmpty()) return QWidget::eventFilter(obj, ev);
            for (const QChar& c : txt) {
                if (c.isPrint() || c == '\t') emit lineEntered(QString(c));
            }
            _in->clear();
            return true;
        }

        if (ke->key() == Qt::Key_Up) {
            if (_history.isEmpty()) return true;
            if (_histIdx == _history.size()) _draft = _in->text();
            if (_histIdx > 0) --_histIdx;
            _in->setText(_history[_histIdx]);
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            if (_history.isEmpty()) return true;
            if (_histIdx < _history.size() - 1) {
                ++_histIdx;
                _in->setText(_history[_histIdx]);
            } else {
                _histIdx = _history.size();
                _in->setText(_draft);
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TerminalView::setHotkeyMode(bool on)
{
    _hotkeyMode = on;
    _in->setPlaceholderText(on
        ? "Hotkey mode — keys sent immediately (Esc to drop)"
        : QString());
}

