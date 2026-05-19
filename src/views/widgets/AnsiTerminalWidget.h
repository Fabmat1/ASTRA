#pragma once

#include <QTextEdit>
#include <QTextCharFormat>
#include <QByteArray>
#include <QList>

/**
 * Minimal but correct VT100-style terminal in a QTextEdit.
 *
 * It maintains its own 2-D screen buffer (rows × cells). All ANSI/VT
 * sequences mutate the buffer; after every chunk the buffer is rendered
 * into the QTextEdit. This makes things like rich.live.Live() work
 * correctly: cursor-up, erase-in-line and overwriting all behave the
 * way a real terminal behaves.
 */
class AnsiTerminalWidget : public QTextEdit
{
    Q_OBJECT
public:
    explicit AnsiTerminalWidget(QWidget* parent = nullptr);

    void feed(const QByteArray& chunk);
    void feed(const QString& s) { feed(s.toUtf8()); }
    void clearTerminal();

private:
    enum class State { Normal, Esc, Csi };
    struct Cell {
        QChar           ch{' '};
        QTextCharFormat fmt;
    };

    void processByte(uchar c);
    void dispatchCsi(char final);
    void applySgr(const QList<int>& nums);

    void putChar(QChar ch);
    void newLine();           // LF (+ implicit CR)
    void ensureRow(int row);

    void render();

    static QColor colorFor(int code);
    static QColor xterm256(int idx);

    QList<QList<Cell>> _screen;     // [row][col]
    int                _row   = 0;
    int                _col   = 0;
    State              _state = State::Normal;
    QByteArray         _csi;
    QByteArray         _utf8Acc;
    QTextCharFormat    _format;
    QColor             _defaultFg;
};