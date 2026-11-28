#ifndef VTSCREEN_H
#define VTSCREEN_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>
#include <functional>

// Minimal VT100/xterm screen model: a cell grid plus scrollback.
// Handles what R/readline, shells and ConPTY emit (cursor movement, erase,
// scroll regions, SGR colours); everything else is parsed and ignored.

struct VtCell {
    // Colour encoding: -1 = default, 0..255 = palette, 0x01000000|rgb = truecolor.
    enum Flags : quint8 { Bold = 1, Underline = 2, Reverse = 4, Italic = 8 };
    char32_t ch = U' ';
    qint32   fg = -1;
    qint32   bg = -1;
    quint8   flags = 0;
};

class VtScreen
{
public:
    using Line = QVector<VtCell>;

    explicit VtScreen(int cols = 80, int rows = 24, int maxScrollback = 5000);

    void resize(int cols, int rows);
    void feed(const QString &text);
    void clearAll();

    // Called with bytes that must be written back to the child (DSR/DA replies).
    void setReplyHandler(std::function<void(const QByteArray &)> h) { m_reply = std::move(h); }

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int lineCount() const { return m_lines.size(); }
    // Lines discarded from the top of scrollback so far; lets callers keep
    // absolute positions stable across trimming.
    qint64 droppedLines() const { return m_dropped; }
    const Line &line(int index) const { return m_lines.at(index); }
    int cursorLine() const { return m_lines.size() - m_rows + m_row; }
    int cursorCol() const { return m_col; }

private:
    enum class State { Ground, Esc, EscIntermediate, Csi, Osc, OscEsc };

    int  base() const { return m_lines.size() - m_rows; }
    Line blankLine() const;
    Line &row(int r) { return m_lines[base() + r]; }
    void put(char32_t ch);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int n);
    void scrollDown(int n);
    void eraseCells(int r, int from, int to);
    void csi(char finalByte);
    void sgr();
    void reply(const QByteArray &b) { if (m_reply) m_reply(b); }
    int  param(int i, int def) const;

    int m_cols, m_rows, m_maxScrollback;
    QList<Line> m_lines;
    qint64 m_dropped = 0;

    int  m_row = 0, m_col = 0;
    bool m_wrapPending = false;
    int  m_top = 0, m_bottom = 0;
    int  m_savedRow = 0, m_savedCol = 0;

    qint32 m_fg = -1, m_bg = -1;
    quint8 m_flags = 0;

    State m_state = State::Ground;
    QVector<int> m_params;
    int  m_curParam = -1;
    char m_priv = 0;
    QChar m_highSurrogate;

    std::function<void(const QByteArray &)> m_reply;
};

#endif // VTSCREEN_H
