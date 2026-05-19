#include "AnsiTerminalWidget.h"

#include <QFont>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>

static constexpr int MAX_ROWS = 5000;          // hard cap to keep memory sane

// ---------------------------------------------------------------------------

AnsiTerminalWidget::AnsiTerminalWidget(QWidget* parent)
    : QTextEdit(parent)
    , _defaultFg("#dcdcdc")
{
    setReadOnly(true);
    setAcceptRichText(false);
    setLineWrapMode(QTextEdit::NoWrap);
    setStyleSheet("QTextEdit { background:#1e1e1e; color:#dcdcdc; }");

    QFont mono;
    mono.setFamily("Monospace");
    mono.setStyleHint(QFont::TypeWriter);
    setFont(mono);

    _format.setForeground(_defaultFg);
    _screen.append(QList<Cell>{});                        // start with one empty row
}

void AnsiTerminalWidget::clearTerminal()
{
    _screen.clear();
    _screen.append(QList<Cell>{});
    _row = _col = 0;
    _state = State::Normal;
    _csi.clear();
    _utf8Acc.clear();
    _format = QTextCharFormat();
    _format.setForeground(_defaultFg);
    clear();
}

void AnsiTerminalWidget::feed(const QByteArray& chunk)
{
    if (chunk.isEmpty()) return;

    auto* vsb = verticalScrollBar();
    const bool atBottom = vsb->value() >= vsb->maximum() - 4;

    for (char c : chunk) processByte(uchar(c));

    render();

    if (atBottom) vsb->setValue(vsb->maximum());
}

// ---------------------------------------------------------------------------
// Byte stream → screen buffer

void AnsiTerminalWidget::processByte(uchar c)
{
    if (_state == State::Normal) {
        if (c == 0x1B) { _state = State::Esc; return; }
        if (c == '\r') { _col = 0; return; }
        if (c == '\n') { _col = 0; newLine(); return; }   // treat LF as CRLF
        if (c == '\b') { if (_col > 0) --_col; return; }
        if (c == '\t') {
            const int next = (_col + 8) & ~7;             // 8-char tab stops
            while (_col < next) putChar(' ');
            return;
        }
        if (c < 0x20) return;

        // UTF-8 multibyte assembly
        if (!_utf8Acc.isEmpty() || c >= 0x80) {
            _utf8Acc.append(char(c));
            const QString s = QString::fromUtf8(_utf8Acc);
            const bool incomplete =
                s.isEmpty() || s.endsWith(QChar::ReplacementCharacter);
            if (!incomplete) {
                for (QChar ch : s) putChar(ch);
                _utf8Acc.clear();
            } else if (_utf8Acc.size() >= 4) {
                putChar(QChar::ReplacementCharacter);
                _utf8Acc.clear();
            }
            return;
        }

        putChar(QChar(c));
        return;
    }

    if (_state == State::Esc) {
        switch (c) {
        case '[': _state = State::Csi; _csi.clear();      return;
        case 'D': /* IND  */ newLine();                                   break;
        case 'E': /* NEL  */ _col = 0; newLine();                         break;
        case 'M': /* RI   */ if (_row > 0) --_row;                        break;
        case ']': /* OSC  */ /* swallow until ST or BEL – we don’t need it */ break;
        default: break;
        }
        _state = State::Normal;
        return;
    }

    // CSI: collect parameters then dispatch on final byte
    if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == ':') {
        _csi.append(char(c));
        return;
    }
    dispatchCsi(char(c));
    _state = State::Normal;
    _csi.clear();
}

// ---------------------------------------------------------------------------

void AnsiTerminalWidget::putChar(QChar ch)
{
    ensureRow(_row);
    auto& line = _screen[_row];
    while (line.size() <= _col)
        line.append(Cell{ QChar(' '), QTextCharFormat() });
    line[_col] = { ch, _format };
    ++_col;
}

void AnsiTerminalWidget::newLine()
{
    ++_row;
    ensureRow(_row);
}

void AnsiTerminalWidget::ensureRow(int row)
{
    while (_screen.size() <= row) _screen.append(QList<Cell>{});

    // Keep memory bounded; trim from the top.
    if (_screen.size() > MAX_ROWS) {
        const int excess = _screen.size() - MAX_ROWS;
        _screen.erase(_screen.begin(), _screen.begin() + excess);
        _row = std::max(0, _row - excess);
    }
}

// ---------------------------------------------------------------------------

void AnsiTerminalWidget::dispatchCsi(char final)
{
    // DEC private modes (?25l, ?25h, ?1049h, ...): accept and ignore
    if (!_csi.isEmpty() && _csi[0] == '?') return;

    QList<int> nums;
    if (_csi.isEmpty()) {
        nums << 0;
    } else {
        for (const auto& p : _csi.split(';')) nums << p.toInt();
    }
    const int n  = nums.value(0, 0);
    const int n1 = std::max(1, n);

    switch (final) {
    case 'm': applySgr(nums); break;

    case 'A': _row = std::max(0, _row - n1);                 break;  // CUU
    case 'B': _row += n1; ensureRow(_row);                   break;  // CUD
    case 'C': _col += n1;                                    break;  // CUF
    case 'D': _col = std::max(0, _col - n1);                 break;  // CUB
    case 'E': _col = 0; _row += n1; ensureRow(_row);         break;  // CNL
    case 'F': _col = 0; _row = std::max(0, _row - n1);       break;  // CPL
    case 'G': _col = std::max(0, n1 - 1);                    break;  // CHA
    case 'd': _row = std::max(0, n1 - 1); ensureRow(_row);   break;  // VPA

    case 'H': case 'f': {                                            // CUP / HVP
        const int r  = std::max(1, nums.value(0, 1));
        const int co = std::max(1, nums.value(1, 1));
        _row = r - 1;
        _col = co - 1;
        ensureRow(_row);
        break;
    }

    case 'K': {                                                      // EL
        ensureRow(_row);
        auto& line = _screen[_row];
        if (n == 0) {                                  // cursor → EOL
            if (_col < line.size()) line.resize(_col);
        } else if (n == 1) {                           // BOL → cursor
            for (int i = 0; i <= _col && i < line.size(); ++i)
                line[i] = Cell{ QChar(' '), QTextCharFormat() };
        } else {                                       // 2: entire line
            line.clear();
        }
        break;
    }

    case 'J': {                                                      // ED
        if (n == 0) {                                  // cursor → end of screen
            ensureRow(_row);
            auto& line = _screen[_row];
            if (_col < line.size()) line.resize(_col);
            while (_screen.size() > _row + 1) _screen.removeLast();
        } else if (n == 1) {                           // start → cursor
            for (int r = 0; r <= _row && r < _screen.size(); ++r) {
                if (r < _row) _screen[r].clear();
                else {
                    auto& line = _screen[r];
                    for (int i = 0; i <= _col && i < line.size(); ++i)
                        line[i] = Cell{ QChar(' '), QTextCharFormat() };
                }
            }
        } else {                                       // 2/3: clear all
            _screen.clear();
            _screen.append(QList<Cell>{});
            _row = _col = 0;
        }
        break;
    }

    default: break;          // SU/SD/SCP/RCP/... — not needed for rich
    }
}

// ---------------------------------------------------------------------------

void AnsiTerminalWidget::applySgr(const QList<int>& nums)
{
    if (nums.isEmpty() || (nums.size() == 1 && nums.first() == 0)) {
        _format = QTextCharFormat();
        _format.setForeground(_defaultFg);
        return;
    }

    for (int i = 0; i < nums.size(); ++i) {
        const int v = nums[i];

        // 256-colour and truecolour (38;5;n / 38;2;r;g;b and 48;...)
        if ((v == 38 || v == 48) && i + 1 < nums.size()) {
            const int mode = nums[i + 1];
            QColor col;
            if (mode == 5 && i + 2 < nums.size()) {
                col = xterm256(nums[i + 2]); i += 2;
            } else if (mode == 2 && i + 4 < nums.size()) {
                col = QColor(nums[i + 2], nums[i + 3], nums[i + 4]); i += 4;
            } else continue;
            if (col.isValid()) {
                if (v == 38) _format.setForeground(col);
                else         _format.setBackground(col);
            }
            continue;
        }

        switch (v) {
        case 0:  _format = QTextCharFormat();
                 _format.setForeground(_defaultFg);            break;
        case 1:  _format.setFontWeight(QFont::Bold);           break;
        case 22: _format.setFontWeight(QFont::Normal);         break;
        case 3:  _format.setFontItalic(true);                  break;
        case 23: _format.setFontItalic(false);                 break;
        case 4:  _format.setFontUnderline(true);               break;
        case 24: _format.setFontUnderline(false);              break;
        case 39: _format.setForeground(_defaultFg);            break;
        case 49: _format.clearBackground();                    break;
        default:
            if (QColor c = colorFor(v); c.isValid()) {
                if ((v >= 40 && v <= 47) || (v >= 100 && v <= 107))
                    _format.setBackground(c);
                else
                    _format.setForeground(c);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Screen → QTextEdit

void AnsiTerminalWidget::render()
{
    QTextDocument* doc = document();
    doc->setMaximumBlockCount(0);            // we manage size ourselves
    QSignalBlocker blocker(doc);             // avoid flicker / signal spam
    doc->clear();

    QTextCursor cur(doc);
    for (int r = 0; r < _screen.size(); ++r) {
        if (r > 0) cur.insertBlock();
        const auto& line = _screen[r];
        if (line.isEmpty()) continue;

        QString run;
        QTextCharFormat curFmt = line.first().fmt;
        for (const Cell& cell : line) {
            if (cell.fmt == curFmt) {
                run.append(cell.ch);
            } else {
                cur.insertText(run, curFmt);
                run    = QString(cell.ch);
                curFmt = cell.fmt;
            }
        }
        if (!run.isEmpty()) cur.insertText(run, curFmt);
    }
}

// ---------------------------------------------------------------------------

QColor AnsiTerminalWidget::colorFor(int code)
{
    switch (code) {
    case 30: case 40:  case 90: case 100: return QColor("#666666");
    case 31: case 41:                     return QColor("#cd3131");
    case 91: case 101:                    return QColor("#f14c4c");
    case 32: case 42:                     return QColor("#0dbc79");
    case 92: case 102:                    return QColor("#23d18b");
    case 33: case 43:                     return QColor("#c19c00");
    case 93: case 103:                    return QColor("#f5f543");
    case 34: case 44:                     return QColor("#2472c8");
    case 94: case 104:                    return QColor("#3b8eea");
    case 35: case 45:                     return QColor("#bc3fbc");
    case 95: case 105:                    return QColor("#d670d6");
    case 36: case 46:                     return QColor("#11a8cd");
    case 96: case 106:                    return QColor("#29b8db");
    case 37: case 47:                     return QColor("#e5e5e5");
    case 97: case 107:                    return QColor("#ffffff");
    }
    return QColor();
}

QColor AnsiTerminalWidget::xterm256(int idx)
{
    if (idx < 0 || idx > 255) return QColor();
    if (idx < 16) {
        static const char* base[16] = {
            "#000000","#cd3131","#0dbc79","#c19c00",
            "#2472c8","#bc3fbc","#11a8cd","#e5e5e5",
            "#666666","#f14c4c","#23d18b","#f5f543",
            "#3b8eea","#d670d6","#29b8db","#ffffff"
        };
        return QColor(base[idx]);
    }
    if (idx < 232) {
        idx -= 16;
        static const int v[6] = { 0, 95, 135, 175, 215, 255 };
        return QColor(v[(idx/36)%6], v[(idx/6)%6], v[idx%6]);
    }
    const int g = 8 + (idx - 232) * 10;
    return QColor(g, g, g);
}