#include "vtscreen.h"

#include <algorithm>

VtScreen::VtScreen(int cols, int rows, int maxScrollback)
    : m_cols(qMax(1, cols)), m_rows(qMax(1, rows)), m_maxScrollback(maxScrollback)
{
    m_bottom = m_rows - 1;
    for (int i = 0; i < m_rows; ++i)
        m_lines.append(blankLine());
}

VtScreen::Line VtScreen::blankLine() const
{
    return Line(m_cols);
}

void VtScreen::clearAll()
{
    m_lines.clear();
    for (int i = 0; i < m_rows; ++i)
        m_lines.append(blankLine());
    m_row = m_col = 0;
    m_wrapPending = false;
}

void VtScreen::resize(int cols, int rows)
{
    cols = qMax(1, cols);
    rows = qMax(1, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    if (rows > m_rows) {
        int grow = rows - m_rows;
        // Pull history back into view first so the cursor keeps its place.
        int pull = qMin(grow, m_lines.size() - m_rows);
        for (int i = 0; i < grow - pull; ++i)
            m_lines.append(Line(cols));
        m_row += pull;
    } else if (rows < m_rows) {
        int shrink = m_rows - rows;
        // Keep the cursor visible: lines above it go to scrollback, the
        // rest are dropped from the bottom.
        int pushTop = qMax(0, qMin(shrink, m_row - rows + 1));
        int dropBottom = shrink - pushTop;
        m_lines.erase(m_lines.end() - dropBottom, m_lines.end());
        m_row -= pushTop;
    }
    m_rows = rows;

    // Pad/trim only the live screen rows; scrollback keeps its original width.
    for (int r = 0; r < m_rows; ++r)
        row(r).resize(cols);
    m_cols = cols;

    m_top = 0;
    m_bottom = m_rows - 1;
    m_row = qBound(0, m_row, m_rows - 1);
    m_col = qBound(0, m_col, m_cols - 1);
    m_wrapPending = false;
}

int VtScreen::param(int i, int def) const
{
    if (i >= m_params.size() || m_params.at(i) <= 0)
        return def;
    return m_params.at(i);
}

void VtScreen::put(char32_t ch)
{
    if (m_wrapPending) {
        m_col = 0;
        lineFeed();
        m_wrapPending = false;
    }
    VtCell &c = row(m_row)[m_col];
    c.ch = ch;
    c.fg = m_fg;
    c.bg = m_bg;
    c.flags = m_flags;
    if (m_col == m_cols - 1)
        m_wrapPending = true;
    else
        ++m_col;
}

void VtScreen::lineFeed()
{
    if (m_row == m_bottom)
        scrollUp(1);
    else if (m_row < m_rows - 1)
        ++m_row;
}

void VtScreen::reverseIndex()
{
    if (m_row == m_top)
        scrollDown(1);
    else if (m_row > 0)
        --m_row;
}

void VtScreen::scrollUp(int n)
{
    n = qMin(n, m_bottom - m_top + 1);
    for (int i = 0; i < n; ++i) {
        if (m_top == 0 && m_bottom == m_rows - 1) {
            m_lines.append(blankLine());
            // Trim scrollback beyond the cap.
            while (m_lines.size() - m_rows > m_maxScrollback) {
                m_lines.removeFirst();
                ++m_dropped;
            }
        } else {
            m_lines.removeAt(base() + m_top);
            m_lines.insert(base() + m_bottom, blankLine());
        }
    }
}

void VtScreen::scrollDown(int n)
{
    n = qMin(n, m_bottom - m_top + 1);
    for (int i = 0; i < n; ++i) {
        m_lines.removeAt(base() + m_bottom);
        m_lines.insert(base() + m_top, blankLine());
    }
}

void VtScreen::eraseCells(int r, int from, int to)
{
    Line &l = row(r);
    from = qMax(0, from);
    to = qMin(to, m_cols - 1);
    for (int c = from; c <= to; ++c)
        l[c] = VtCell();
}

void VtScreen::feed(const QString &text)
{
    for (QChar qc : text) {
        char32_t ch = qc.unicode();

        if (m_state == State::Ground) {
            if (qc.isHighSurrogate()) { m_highSurrogate = qc; continue; }
            if (qc.isLowSurrogate() && !m_highSurrogate.isNull()) {
                ch = QChar::surrogateToUcs4(m_highSurrogate, qc);
                m_highSurrogate = QChar();
            }
        }

        // C0 controls act in every state except inside an OSC string.
        if (ch < 0x20 && m_state != State::Osc && m_state != State::OscEsc) {
            switch (ch) {
            case '\n': case 0x0B: case 0x0C: lineFeed(); m_wrapPending = false; break;
            case '\r': m_col = 0; m_wrapPending = false; break;
            case '\b': if (m_col > 0) --m_col; m_wrapPending = false; break;
            case '\t':
                m_col = qMin(m_cols - 1, ((m_col / 8) + 1) * 8);
                m_wrapPending = false;
                break;
            case 0x1B: m_state = State::Esc; break;
            default: break;  // BEL and the rest
            }
            continue;
        }

        switch (m_state) {
        case State::Ground:
            if (ch >= 0x20 && ch != 0x7F)
                put(ch);
            break;

        case State::Esc:
            m_state = State::Ground;
            switch (ch) {
            case '[':
                m_state = State::Csi;
                m_params.clear();
                m_curParam = -1;
                m_priv = 0;
                break;
            case ']': m_state = State::Osc; break;
            case '(': case ')': case '*': case '+': case '#': case '%':
                m_state = State::EscIntermediate;
                break;
            case '7': m_savedRow = m_row; m_savedCol = m_col; break;
            case '8': m_row = qMin(m_savedRow, m_rows - 1);
                      m_col = qMin(m_savedCol, m_cols - 1);
                      m_wrapPending = false; break;
            case 'M': reverseIndex(); break;
            case 'D': lineFeed(); break;
            case 'E': m_col = 0; lineFeed(); break;
            case 'c': clearAll(); break;
            default: break;
            }
            break;

        case State::EscIntermediate:
            m_state = State::Ground;
            break;

        case State::Csi:
            if (ch >= '0' && ch <= '9') {
                m_curParam = (m_curParam < 0 ? 0 : m_curParam) * 10 + int(ch - '0');
                if (m_curParam > 100000) m_curParam = 100000;
            } else if (ch == ';' || ch == ':') {
                m_params.append(m_curParam < 0 ? 0 : m_curParam);
                m_curParam = -1;
            } else if (ch == '?' || ch == '>' || ch == '=' || ch == '<') {
                m_priv = char(ch);
            } else if (ch >= 0x20 && ch <= 0x2F) {
                // intermediates (e.g. DECSCUSR "q") — ignored
            } else if (ch >= 0x40 && ch <= 0x7E) {
                if (m_curParam >= 0 || !m_params.isEmpty())
                    m_params.append(m_curParam < 0 ? 0 : m_curParam);
                csi(char(ch));
                m_state = State::Ground;
            } else {
                m_state = State::Ground;
            }
            break;

        case State::Osc:
            if (ch == 0x07)
                m_state = State::Ground;
            else if (ch == 0x1B)
                m_state = State::OscEsc;
            break;

        case State::OscEsc:
            m_state = State::Ground;  // ESC \ terminator (or aborted string)
            break;
        }
    }
}

void VtScreen::csi(char f)
{
    if (m_priv && f != 'c') {
        // Private modes (?25h cursor, ?1049h alt screen, ...) are ignored.
        return;
    }

    const int n = param(0, 1);
    switch (f) {
    case 'A': m_row = qMax(m_top, m_row - n); m_wrapPending = false; break;
    case 'B': m_row = qMin(m_bottom, m_row + n); m_wrapPending = false; break;
    case 'C': m_col = qMin(m_cols - 1, m_col + n); m_wrapPending = false; break;
    case 'D': m_col = qMax(0, m_col - n); m_wrapPending = false; break;
    case 'E': m_row = qMin(m_bottom, m_row + n); m_col = 0; m_wrapPending = false; break;
    case 'F': m_row = qMax(m_top, m_row - n); m_col = 0; m_wrapPending = false; break;
    case 'G': m_col = qBound(0, n - 1, m_cols - 1); m_wrapPending = false; break;
    case 'd': m_row = qBound(0, n - 1, m_rows - 1); m_wrapPending = false; break;
    case 'H': case 'f':
        m_row = qBound(0, param(0, 1) - 1, m_rows - 1);
        m_col = qBound(0, param(1, 1) - 1, m_cols - 1);
        m_wrapPending = false;
        break;
    case 'J': {
        const int mode = m_params.isEmpty() ? 0 : m_params.at(0);
        if (mode == 0) {
            eraseCells(m_row, m_col, m_cols - 1);
            for (int r = m_row + 1; r < m_rows; ++r) eraseCells(r, 0, m_cols - 1);
        } else if (mode == 1) {
            for (int r = 0; r < m_row; ++r) eraseCells(r, 0, m_cols - 1);
            eraseCells(m_row, 0, m_col);
        } else if (mode == 2 || mode == 3) {
            for (int r = 0; r < m_rows; ++r) eraseCells(r, 0, m_cols - 1);
            if (mode == 3) {
                m_dropped += m_lines.size() - m_rows;
                m_lines.erase(m_lines.begin(), m_lines.begin() + (m_lines.size() - m_rows));
            }
        }
        break;
    }
    case 'K': {
        const int mode = m_params.isEmpty() ? 0 : m_params.at(0);
        if (mode == 0) eraseCells(m_row, m_col, m_cols - 1);
        else if (mode == 1) eraseCells(m_row, 0, m_col);
        else if (mode == 2) eraseCells(m_row, 0, m_cols - 1);
        break;
    }
    case 'L':
        if (m_row >= m_top && m_row <= m_bottom) {
            const int k = qMin(n, m_bottom - m_row + 1);
            for (int i = 0; i < k; ++i) {
                m_lines.removeAt(base() + m_bottom);
                m_lines.insert(base() + m_row, blankLine());
            }
        }
        break;
    case 'M':
        if (m_row >= m_top && m_row <= m_bottom) {
            const int k = qMin(n, m_bottom - m_row + 1);
            for (int i = 0; i < k; ++i) {
                m_lines.removeAt(base() + m_row);
                m_lines.insert(base() + m_bottom, blankLine());
            }
        }
        break;
    case 'P': {
        Line &l = row(m_row);
        const int k = qMin(n, m_cols - m_col);
        l.remove(m_col, k);
        l.resize(m_cols);
        break;
    }
    case '@': {
        Line &l = row(m_row);
        const int k = qMin(n, m_cols - m_col);
        l.insert(m_col, k, VtCell());
        l.resize(m_cols);
        break;
    }
    case 'X': eraseCells(m_row, m_col, m_col + n - 1); break;
    case 'S': scrollUp(n); break;
    case 'T': scrollDown(n); break;
    case 'm': sgr(); break;
    case 'r': {
        int t = param(0, 1) - 1;
        int b = param(1, m_rows) - 1;
        if (t < b && b < m_rows) { m_top = t; m_bottom = b; }
        else { m_top = 0; m_bottom = m_rows - 1; }
        m_row = m_col = 0;
        m_wrapPending = false;
        break;
    }
    case 's': m_savedRow = m_row; m_savedCol = m_col; break;
    case 'u': m_row = qMin(m_savedRow, m_rows - 1);
              m_col = qMin(m_savedCol, m_cols - 1);
              m_wrapPending = false; break;
    case 'n':
        if (!m_priv && (m_params.isEmpty() ? 0 : m_params.at(0)) == 6)
            reply(QByteArray("\x1b[") + QByteArray::number(m_row + 1) + ';'
                  + QByteArray::number(m_col + 1) + 'R');
        break;
    case 'c':
        if (!m_priv)
            reply("\x1b[?1;2c");
        break;
    default: break;
    }
}

void VtScreen::sgr()
{
    if (m_params.isEmpty()) m_params.append(0);
    for (int i = 0; i < m_params.size(); ++i) {
        const int p = m_params.at(i);
        if (p == 0) { m_fg = m_bg = -1; m_flags = 0; }
        else if (p == 1) m_flags |= VtCell::Bold;
        else if (p == 3) m_flags |= VtCell::Italic;
        else if (p == 4) m_flags |= VtCell::Underline;
        else if (p == 7) m_flags |= VtCell::Reverse;
        else if (p == 22) m_flags &= ~VtCell::Bold;
        else if (p == 23) m_flags &= ~VtCell::Italic;
        else if (p == 24) m_flags &= ~VtCell::Underline;
        else if (p == 27) m_flags &= ~VtCell::Reverse;
        else if (p >= 30 && p <= 37) m_fg = p - 30;
        else if (p == 39) m_fg = -1;
        else if (p >= 40 && p <= 47) m_bg = p - 40;
        else if (p == 49) m_bg = -1;
        else if (p >= 90 && p <= 97) m_fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) m_bg = p - 100 + 8;
        else if (p == 38 || p == 48) {
            qint32 &target = (p == 38) ? m_fg : m_bg;
            const int mode = (i + 1 < m_params.size()) ? m_params.at(i + 1) : 0;
            if (mode == 5 && i + 2 < m_params.size()) {
                target = qBound(0, m_params.at(i + 2), 255);
                i += 2;
            } else if (mode == 2 && i + 4 < m_params.size()) {
                target = 0x01000000 | (qBound(0, m_params.at(i + 2), 255) << 16)
                                    | (qBound(0, m_params.at(i + 3), 255) << 8)
                                    |  qBound(0, m_params.at(i + 4), 255);
                i += 4;
            } else {
                break;
            }
        }
    }
}
