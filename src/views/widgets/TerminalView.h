#pragma once

#include <QWidget>
#include <QStringList>

class QPlainTextEdit;
class QLineEdit;

// Minimal in-app terminal: a read-only output pane plus an input line
// with command history (Up/Down). Emits lineEntered() when the user
// presses Enter. No ANSI/escape handling — good enough for line-based
// REPLs like ISIS over pipes.
class TerminalView : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget* parent = nullptr);

    void appendOutput(const QString& text);   // inserts verbatim
    void appendStatusLine(const QString& s);  // styled [ASTRA] note
    void clearOutput();
    void setInputEnabled(bool on);
    void focusInput();
    void setHotkeyMode(bool on);
    bool hotkeyMode() const { return _hotkeyMode; }

signals:
    void lineEntered(const QString& line);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QPlainTextEdit* _out = nullptr;
    QLineEdit*      _in  = nullptr;
    QStringList     _history;
    int             _histIdx = 0;
    QString         _draft;
    bool _hotkeyMode = false;
};