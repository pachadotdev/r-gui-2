// terminalwidget_win.cpp
// Superseded by terminalwidget.cpp which handles all platforms.
// This file is no longer compiled. Kept for reference only.
#if 0
// terminalwidget_win.cpp (original)
// Windows terminal implementation using ConPTY (Windows 10 build 17763+).
// ConPTY is the same API used by VS Code and Windows Terminal: it creates
// a real PTY via CreatePseudoConsole(), connects bidirectional pipes, and
// lets us run any shell or R session inside a Qt widget.

#include "terminalwidget.h"

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <consoleapi.h>       // CreatePseudoConsole, HPCON
#include <processthreadsapi.h>

#include <atomic>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>

// ── Internal PTY state (PIMPL keeps Win32 types out of the header) ────────────

struct TerminalWidget::ConPtyImpl {
    HPCON  hPCon    = nullptr;
    HANDLE hPtyIn   = INVALID_HANDLE_VALUE; // we write  → shell stdin
    HANDLE hPtyOut  = INVALID_HANDLE_VALUE; // we read   ← shell stdout/err
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hThread  = INVALID_HANDLE_VALUE;
    int    cols = 80;
    int    rows = 24;
};

// ── Background reader thread ──────────────────────────────────────────────────

class PtyReaderThread : public QThread {
    Q_OBJECT
public:
    explicit PtyReaderThread(HANDLE h, QObject *parent = nullptr)
        : QThread(parent), handle(h) {}

    void requestStop() { stopped.store(true); }

signals:
    void dataReady(QByteArray data);

protected:
    void run() override {
        char buf[4096];
        DWORD n = 0;
        while (!stopped.load()) {
            if (!ReadFile(handle, buf, sizeof(buf), &n, nullptr) || n == 0)
                break;
            emit dataReady(QByteArray(buf, static_cast<int>(n)));
        }
    }

private:
    HANDLE handle;
    std::atomic<bool> stopped{false};
};

// ── ANSI 256-color palette ────────────────────────────────────────────────────

static QColor ansiColor(int idx)
{
    static const QColor std16[16] = {
        { 28, 28, 28},  // 0  Black
        {215, 95, 95},  // 1  Red
        { 95,175, 95},  // 2  Green
        {215,175, 95},  // 3  Yellow
        { 95,135,215},  // 4  Blue
        {175, 95,175},  // 5  Magenta
        { 95,175,175},  // 6  Cyan
        {208,208,208},  // 7  White
        {128,128,128},  // 8  Bright Black
        {255,135,135},  // 9  Bright Red
        {135,215,135},  // 10 Bright Green
        {255,215,135},  // 11 Bright Yellow
        {135,175,215},  // 12 Bright Blue
        {215,135,215},  // 13 Bright Magenta
        {135,215,215},  // 14 Bright Cyan
        {255,255,215},  // 15 Bright White
    };
    if (idx >= 0 && idx < 16) return std16[idx];

    // 6×6×6 colour cube (indices 16–231)
    if (idx >= 16 && idx <= 231) {
        int i = idx - 16;
        int b = i % 6; i /= 6;
        int g = i % 6;
        int r = i / 6;
        auto c6 = [](int v) { return v ? 55 + 40 * v : 0; };
        return QColor(c6(r), c6(g), c6(b));
    }
    // Grayscale ramp (indices 232–255)
    if (idx >= 232 && idx <= 255) {
        int v = 8 + 10 * (idx - 232);
        return QColor(v, v, v);
    }
    return Qt::white;
}

// ── Constructor ───────────────────────────────────────────────────────────────

TerminalWidget::TerminalWidget(const QString &shell, QWidget *parent)
    : QPlainTextEdit(parent)
    , pty(new ConPtyImpl)
{
    currentTheme = ThemeManager::instance().currentTheme();

    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setCursorWidth(2);

    QFont f;
    f.setFamily("Hack Nerd Font Mono");
    f.setPointSize(10);
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    setFont(f);

    setTheme(currentTheme);

    // Resolve shell.
    // When "R" is passed (as mainwindow does), find Rterm.exe which is the
    // Windows equivalent of `R --interactive`.
    if (!shell.isEmpty() && shell != "R" && shell != "r") {
        shellPath = shell;
    } else if (shell == "R" || shell == "r") {
        // Try R_HOME env first, then PATH
        QString rHome = QString::fromLocal8Bit(qgetenv("R_HOME"));
        QString rterm;
        if (!rHome.isEmpty())
            rterm = rHome + "/bin/x64/Rterm.exe";
        if (rterm.isEmpty() || !QFileInfo::exists(rterm))
            rterm = QStandardPaths::findExecutable("Rterm");
        if (rterm.isEmpty() || !QFileInfo::exists(rterm)) {
            // Fallback: ask R where it lives
            QProcess probe;
            probe.start("R", {"--slave", "-e", "cat(R.home('bin'))"});
            if (probe.waitForFinished(3000)) {
                QString binDir = QString::fromLocal8Bit(probe.readAllStandardOutput()).trimmed();
                rterm = binDir + "/Rterm.exe";
            }
        }
        shellPath = QFileInfo::exists(rterm) ? rterm : "Rterm.exe";
    } else {
        // Default shell: prefer PowerShell 7 (pwsh), fall back to Windows PowerShell 5
        QString pwsh = QStandardPaths::findExecutable("pwsh");
        shellPath = pwsh.isEmpty() ? QStandardPaths::findExecutable("powershell") : pwsh;
        if (shellPath.isEmpty()) shellPath = "powershell.exe";
    }

    // Verify ConPTY is available (Win10 1809+)
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel || !GetProcAddress(hKernel, "CreatePseudoConsole")) {
        insertPlainText("Error: Windows 10 version 1809 (build 17763) or later is required "
                        "for the embedded terminal.\r\n");
        return;
    }

    startPty(shellPath, {});
}

TerminalWidget::~TerminalWidget()
{
    if (!pty) return;
    if (pty->hProcess != INVALID_HANDLE_VALUE) {
        TerminateProcess(pty->hProcess, 0);
        WaitForSingleObject(pty->hProcess, 1000);
        CloseHandle(pty->hProcess);
    }
    if (pty->hThread  != INVALID_HANDLE_VALUE) CloseHandle(pty->hThread);
    if (pty->hPCon)                             ClosePseudoConsole(pty->hPCon);
    if (pty->hPtyIn   != INVALID_HANDLE_VALUE)  CloseHandle(pty->hPtyIn);
    if (pty->hPtyOut  != INVALID_HANDLE_VALUE)  CloseHandle(pty->hPtyOut);
    delete pty;
}

// ── PTY startup ───────────────────────────────────────────────────────────────

void TerminalWidget::startPty(const QString &shell, const QStringList &args)
{
    // Compute initial size in columns/rows from current widget dimensions
    QFontMetrics fm(font());
    int charW = fm.horizontalAdvance('M');
    int charH = fm.height();
    pty->cols = qMax(40, viewport()->width()  / qMax(1, charW));
    pty->rows = qMax(10, viewport()->height() / qMax(1, charH));

    COORD sz = { static_cast<SHORT>(pty->cols), static_cast<SHORT>(pty->rows) };

    // Pipe pair 1: parent writes → PTY reads  (shell's stdin)
    // Pipe pair 2: PTY writes   → parent reads (shell's stdout + stderr)
    HANDLE hIn_r,  hIn_w;
    HANDLE hOut_r, hOut_w;
    if (!CreatePipe(&hIn_r,  &hIn_w,  nullptr, 0) ||
        !CreatePipe(&hOut_r, &hOut_w, nullptr, 0)) {
        insertPlainText("Error: CreatePipe failed.\r\n");
        return;
    }

    HRESULT hr = CreatePseudoConsole(sz, hIn_r, hOut_w, 0, &pty->hPCon);
    // PTY now owns the read/write ends; close our duplicates
    CloseHandle(hIn_r);
    CloseHandle(hOut_w);

    if (FAILED(hr)) {
        CloseHandle(hIn_w);
        CloseHandle(hOut_r);
        insertPlainText(QString("Error: CreatePseudoConsole failed (HRESULT 0x%1).\r\n")
                            .arg(static_cast<unsigned>(hr), 8, 16, QLatin1Char('0')));
        return;
    }

    pty->hPtyIn  = hIn_w;
    pty->hPtyOut = hOut_r;

    // Set RGUI2_PLOT_DIR so R's plot-capture hook works on Windows too
    QString plotDir = QDir::tempPath() + "/rgui2_plots";
    QDir().mkpath(plotDir);
    SetEnvironmentVariableW(L"RGUI2_PLOT_DIR", plotDir.toStdWString().c_str());

    // If launching R/Rterm, inject the same init script the Unix build uses
    QString baseName = QFileInfo(shell).baseName().toLower();
    if (baseName == "r" || baseName == "rterm") {
        QString initPath = QDir::tempPath()
            + QString("/rgui2_init_%1.R").arg(QCoreApplication::applicationPid());
        QFile f(initPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "local({\n"
                << "  orig_prof <- Sys.getenv('RGUI2_ORIGINAL_R_PROFILE_USER')\n"
                << "  if (nzchar(orig_prof) && file.exists(orig_prof)) {\n"
                << "    source(orig_prof)\n"
                << "  } else {\n"
                << "    if (file.exists('.Rprofile')) source('.Rprofile')\n"
                << "    else if (file.exists(file.path(Sys.getenv('USERPROFILE'), '.Rprofile')))\n"
                << "      source(file.path(Sys.getenv('USERPROFILE'), '.Rprofile'))\n"
                << "  }\n"
                << "  if (requireNamespace('rgui2', quietly=TRUE)) {\n"
                << "    library(rgui2)\n"
                << "    rgui2::init_monitor(file.path(tempdir(), 'rgui2_env.json'))\n"
                << "  }\n"
                << "  local({\n"
                << "    plot_dir <- Sys.getenv('RGUI2_PLOT_DIR', unset=file.path(tempdir(),'rgui2_plots'))\n"
                << "    dir.create(plot_dir, showWarnings=FALSE, recursive=TRUE)\n"
                << "    index_file <- file.path(plot_dir, 'rgui2_plot_index.txt')\n"
                << "    snap_counter <- 0L; last_snap_size <- -1L\n"
                << "    options(device = function(width=7, height=5, ...) {\n"
                << "      grDevices::png(file.path(plot_dir, sprintf('rgui2_dev_%06d.png', snap_counter+1L)),\n"
                << "        width=round(width*96), height=round(height*96), res=96, ...)\n"
                << "      grDevices::dev.control(displaylist='enable')\n"
                << "    })\n"
                << "    addTaskCallback(function(expr, value, ok, visible) {\n"
                << "      if (grDevices::dev.cur() != 1L) {\n"
                << "        tryCatch({\n"
                << "          rec <- grDevices::recordPlot()\n"
                << "          if (length(rec[[1]]) > 0L) {\n"
                << "            curr <- file.path(plot_dir, 'rgui2_current.png')\n"
                << "            grDevices::png(curr, width=800L, height=600L, res=96L)\n"
                << "            grDevices::replayPlot(rec); grDevices::dev.off()\n"
                << "            ns <- file.size(curr)\n"
                << "            if (!identical(ns, last_snap_size)) {\n"
                << "              last_snap_size <<- ns; snap_counter <<- snap_counter+1L\n"
                << "              sf <- file.path(plot_dir, sprintf('rgui2_snap_%06d.png', snap_counter))\n"
                << "              file.copy(curr, sf, overwrite=TRUE); writeLines(sf, index_file)\n"
                << "            }\n"
                << "          }\n"
                << "        }, error=function(e) NULL)\n"
                << "      }; TRUE\n"
                << "    }, name='rgui2_plot_capture')\n"
                << "  })\n"
                << "})\n";
            f.close();

            QString origProf = QString::fromLocal8Bit(qgetenv("R_PROFILE_USER"));
            if (!origProf.isEmpty())
                SetEnvironmentVariableW(L"RGUI2_ORIGINAL_R_PROFILE_USER",
                                        origProf.toStdWString().c_str());
            SetEnvironmentVariableW(L"R_PROFILE_USER",
                                    initPath.toStdWString().c_str());
        }
    }

    // Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);

    auto *attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attrSize));
    if (!attrList) {
        insertPlainText("Error: HeapAlloc failed.\r\n");
        return;
    }
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0,
                              PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              pty->hPCon, sizeof(HPCON),
                              nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb  = sizeof(si);
    si.lpAttributeList = attrList;

    // Build quoted command line
    QString cmd = shell.contains(' ') ? '"' + shell + '"' : shell;
    for (const QString &a : args)
        cmd += ' ' + (a.contains(' ') ? '"' + a + '"' : a);
    std::wstring wCmd = cmd.toStdWString();

    PROCESS_INFORMATION pi{};
    BOOL launched = CreateProcessW(
        nullptr,
        wCmd.data(),
        nullptr, nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr,    // inherit parent environment (includes RGUI2_PLOT_DIR etc.)
        nullptr,    // inherit working directory
        &si.StartupInfo,
        &pi);

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    if (!launched) {
        insertPlainText(QString("Error: Could not launch '%1' (Win32 error %2).\r\n")
                            .arg(shell).arg(GetLastError()));
        return;
    }

    pty->hProcess = pi.hProcess;
    pty->hThread  = pi.hThread;

    // Start background reader thread
    auto *reader = new PtyReaderThread(pty->hPtyOut, this);
    connect(reader, &PtyReaderThread::dataReady,
            this,   &TerminalWidget::onPtyData,
            Qt::QueuedConnection);
    connect(reader, &PtyReaderThread::finished, reader, &QObject::deleteLater);
    reader->start();
}

// ── PTY I/O ───────────────────────────────────────────────────────────────────

void TerminalWidget::writeToPty(const QByteArray &data)
{
    if (!pty || pty->hPtyIn == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(pty->hPtyIn, data.constData(),
              static_cast<DWORD>(data.size()), &written, nullptr);
}

void TerminalWidget::onPtyData(const QByteArray &data)
{
    processVtData(data);
}

// ── ANSI SGR colour handler ───────────────────────────────────────────────────

void TerminalWidget::applyAnsiSgr(const QList<int> &params)
{
    int i = 0;
    while (i < params.size()) {
        int p = params[i];
        switch (p) {
        case  0: currentFormat = QTextCharFormat(); break;
        case  1: currentFormat.setFontWeight(QFont::Bold);   break;
        case  2: currentFormat.setFontWeight(QFont::Light);  break;
        case  3: currentFormat.setFontItalic(true);          break;
        case  4: currentFormat.setFontUnderline(true);       break;
        case 22: currentFormat.setFontWeight(QFont::Normal); break;
        case 23: currentFormat.setFontItalic(false);         break;
        case 24: currentFormat.setFontUnderline(false);      break;
        case 39: currentFormat.clearForeground();            break;
        case 49: currentFormat.clearBackground();            break;
        default:
            if      (p >= 30  && p <= 37)  { currentFormat.setForeground(ansiColor(p - 30));       }
            else if (p >= 40  && p <= 47)  { currentFormat.setBackground(ansiColor(p - 40));       }
            else if (p >= 90  && p <= 97)  { currentFormat.setForeground(ansiColor(p - 90  + 8)); }
            else if (p >= 100 && p <= 107) { currentFormat.setBackground(ansiColor(p - 100 + 8)); }
            else if ((p == 38 || p == 48) && i + 1 < params.size()) {
                bool fg = (p == 38);
                if (params[i+1] == 5 && i + 2 < params.size()) {          // 256-colour
                    QColor c = ansiColor(params[i+2]);
                    if (fg) currentFormat.setForeground(c); else currentFormat.setBackground(c);
                    i += 2;
                } else if (params[i+1] == 2 && i + 4 < params.size()) {  // truecolour
                    QColor c(params[i+2], params[i+3], params[i+4]);
                    if (fg) currentFormat.setForeground(c); else currentFormat.setBackground(c);
                    i += 4;
                }
            }
        }
        ++i;
    }
}

// ── VT/ANSI output parser ─────────────────────────────────────────────────────
// Handles the subset needed for PowerShell + R:
//   SGR colours, cursor movement, erase-in-line/display, UTF-8 multibyte.
// Characters that overwrite existing content (via \r) use delete-then-insert
// so the QPlainTextEdit document stays coherent.

void TerminalWidget::processVtData(const QByteArray &incoming)
{
    // Prepend any leftover partial sequence from the previous chunk
    QByteArray buf = vtBuffer + incoming;
    vtBuffer.clear();

    QTextCursor cur = textCursor();
    cur.movePosition(QTextCursor::End);

    int i = 0;
    const int len = buf.size();

    while (i < len) {
        const unsigned char ch = static_cast<unsigned char>(buf[i]);

        // ── ESC sequences ────────────────────────────────────────────────────
        if (ch == 0x1B) {
            if (i + 1 >= len) { vtBuffer = buf.mid(i); break; }

            const unsigned char next = static_cast<unsigned char>(buf[i + 1]);

            if (next == '[') {
                // CSI: ESC [ <params> <cmd>   (cmd is 0x40–0x7E)
                int j = i + 2;
                while (j < len && buf[j] >= 0x20 && buf[j] < 0x40) ++j;
                if (j >= len) { vtBuffer = buf.mid(i); break; }

                const char cmd = buf[j];
                const QString paramStr = QString::fromLatin1(buf.mid(i + 2, j - (i + 2)));

                // Parse semicolon-delimited integers (empty parts → 0)
                QList<int> params;
                for (const QString &s : paramStr.split(';')) {
                    bool ok;
                    int v = s.toInt(&ok);
                    params << (ok ? v : 0);
                }

                switch (cmd) {
                case 'm':  // SGR
                    if (params.isEmpty()) params << 0;
                    applyAnsiSgr(params);
                    break;

                case 'J': { // Erase in Display
                    int n = params.isEmpty() ? 0 : params[0];
                    if (n == 2 || n == 3) {
                        cur.select(QTextCursor::Document);
                        cur.removeSelectedText();
                    }
                    break;
                }
                case 'K': { // Erase in Line
                    int n = params.isEmpty() ? 0 : params[0];
                    if (n == 0) {
                        cur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                        cur.removeSelectedText();
                    } else if (n == 2) {
                        cur.movePosition(QTextCursor::StartOfBlock);
                        cur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                        cur.removeSelectedText();
                    }
                    break;
                }
                case 'A': { // Cursor Up
                    int n = params.isEmpty() ? 1 : qMax(1, params[0]);
                    cur.movePosition(QTextCursor::PreviousBlock, QTextCursor::MoveAnchor, n);
                    break;
                }
                case 'B': { // Cursor Down
                    int n = params.isEmpty() ? 1 : qMax(1, params[0]);
                    cur.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, n);
                    break;
                }
                case 'C': { // Cursor Forward
                    int n = params.isEmpty() ? 1 : qMax(1, params[0]);
                    cur.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, n);
                    break;
                }
                case 'D': { // Cursor Back
                    int n = params.isEmpty() ? 1 : qMax(1, params[0]);
                    cur.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor, n);
                    break;
                }
                case 'P': { // Delete N Characters
                    int n = params.isEmpty() ? 1 : qMax(1, params[0]);
                    cur.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, n);
                    cur.removeSelectedText();
                    break;
                }
                // Ignore all other CSI sequences (cursor save/restore, scroll etc.)
                default: break;
                }

                i = j + 1;

            } else if (next == ']') {
                // OSC: ESC ] <text> BEL  or  ESC ] <text> ESC '\'
                // Used for window title etc. - ignore content.
                int j = i + 2;
                while (j < len) {
                    if (buf[j] == '\007') { i = j + 1; goto next_char; }
                    if (j + 1 < len && buf[j] == '\033' && buf[j+1] == '\\') { i = j + 2; goto next_char; }
                    ++j;
                }
                // Didn't find terminator - buffer and wait for more data
                vtBuffer = buf.mid(i);
                goto flush;

            } else {
                // 2-char ESC sequence (ESC c, ESC =, ESC >, etc.) - skip
                i += 2;
            }

        // ── C0 control characters ─────────────────────────────────────────────
        } else if (ch == '\r') {
            cur.movePosition(QTextCursor::StartOfBlock);
            ++i;

        } else if (ch == '\n') {
            cur.movePosition(QTextCursor::End);
            cur.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
            ++i;

        } else if (ch == '\b') {
            cur.movePosition(QTextCursor::PreviousCharacter);
            ++i;

        } else if (ch == '\a') {
            QApplication::beep();
            ++i;

        } else if (ch == '\t') {
            // Advance to next 8-column tab stop
            int col    = cur.positionInBlock();
            int spaces = 8 - (col % 8);
            cur.insertText(QString(spaces, ' '), currentFormat);
            ++i;

        // ── Printable / UTF-8 multibyte ──────────────────────────────────────
        } else if (ch >= 0x20) {
            // Determine UTF-8 sequence length
            int seqLen = 1;
            if      ((ch & 0xE0) == 0xC0) seqLen = 2;
            else if ((ch & 0xF0) == 0xE0) seqLen = 3;
            else if ((ch & 0xF8) == 0xF0) seqLen = 4;

            if (i + seqLen > len) { vtBuffer = buf.mid(i); break; }

            const QString s = QString::fromUtf8(buf.constData() + i, seqLen);

            // Overwrite mode: if not at end of block, replace existing chars
            const QTextBlock blk = cur.block();
            if (cur.positionInBlock() < blk.length() - 1) {
                cur.movePosition(QTextCursor::NextCharacter,
                                 QTextCursor::KeepAnchor, s.size());
                cur.insertText(s, currentFormat);
            } else {
                cur.insertText(s, currentFormat);
            }
            i += seqLen;

        } else {
            ++i; // skip other control chars
        }

        next_char:;
    }

    flush:
    setTextCursor(cur);

    // Auto-scroll to bottom
    QScrollBar *sb = verticalScrollBar();
    sb->setValue(sb->maximum());

    // Keep the document from growing without bound (cap at 10 000 blocks)
    QTextDocument *doc = document();
    while (doc->blockCount() > 10000) {
        QTextCursor trim(doc->begin());
        trim.select(QTextCursor::BlockUnderCursor);
        trim.removeSelectedText();
        trim.deleteChar();
    }
}

// ── Keyboard input ────────────────────────────────────────────────────────────

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();

    // Ctrl+Shift+C/V → copy/paste (don't send to PTY)
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        if (key == Qt::Key_C) {
            QApplication::clipboard()->setText(textCursor().selectedText());
            event->accept(); return;
        }
        if (key == Qt::Key_V) {
            writeToPty(QApplication::clipboard()->text().toUtf8());
            event->accept(); return;
        }
    }

    QByteArray data;

    // Ctrl+<letter> → ASCII control codes (Ctrl+C = 0x03, etc.)
    if ((mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier) && !(mods & Qt::AltModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            data = QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
        } else if (key == Qt::Key_Space)        { data = QByteArray(1, '\0'); }
          else if (key == Qt::Key_BracketLeft)  { data = "\x1b"; }
          else if (key == Qt::Key_Backslash)    { data = "\x1c"; }
          else if (key == Qt::Key_BracketRight) { data = "\x1d"; }
    }

    if (data.isEmpty()) {
        switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:     data = "\r";       break;
        case Qt::Key_Backspace: data = "\x7f";     break;
        case Qt::Key_Tab:       data = "\t";       break;
        case Qt::Key_Escape:    data = "\x1b";     break;
        case Qt::Key_Up:        data = "\x1b[A";   break;
        case Qt::Key_Down:      data = "\x1b[B";   break;
        case Qt::Key_Right:     data = "\x1b[C";   break;
        case Qt::Key_Left:      data = "\x1b[D";   break;
        case Qt::Key_Home:      data = "\x1b[H";   break;
        case Qt::Key_End:       data = "\x1b[F";   break;
        case Qt::Key_PageUp:    data = "\x1b[5~";  break;
        case Qt::Key_PageDown:  data = "\x1b[6~";  break;
        case Qt::Key_Insert:    data = "\x1b[2~";  break;
        case Qt::Key_Delete:    data = "\x1b[3~";  break;
        case Qt::Key_F1:        data = "\x1bOP";   break;
        case Qt::Key_F2:        data = "\x1bOQ";   break;
        case Qt::Key_F3:        data = "\x1bOR";   break;
        case Qt::Key_F4:        data = "\x1bOS";   break;
        case Qt::Key_F5:        data = "\x1b[15~"; break;
        case Qt::Key_F6:        data = "\x1b[17~"; break;
        case Qt::Key_F7:        data = "\x1b[18~"; break;
        case Qt::Key_F8:        data = "\x1b[19~"; break;
        case Qt::Key_F9:        data = "\x1b[20~"; break;
        case Qt::Key_F10:       data = "\x1b[21~"; break;
        case Qt::Key_F11:       data = "\x1b[23~"; break;
        case Qt::Key_F12:       data = "\x1b[24~"; break;
        default:
            if (!event->text().isEmpty())
                data = event->text().toUtf8();
            break;
        }
    }

    if (!data.isEmpty()) {
        writeToPty(data);
        event->accept();
    } else {
        event->ignore();
    }
}

// ── Resize → update PTY window size ──────────────────────────────────────────

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    if (!pty || !pty->hPCon) return;

    QFontMetrics fm(font());
    int charW = fm.horizontalAdvance('M');
    int charH = fm.height();
    int newCols = qMax(10, viewport()->width()  / qMax(1, charW));
    int newRows = qMax(3,  viewport()->height() / qMax(1, charH));

    if (newCols != pty->cols || newRows != pty->rows) {
        pty->cols = newCols;
        pty->rows = newRows;
        COORD sz = { static_cast<SHORT>(newCols), static_cast<SHORT>(newRows) };
        ResizePseudoConsole(pty->hPCon, sz);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void TerminalWidget::setTheme(const EditorTheme &theme)
{
    currentTheme = theme;
    QPalette p = palette();
    p.setColor(QPalette::Base, theme.background);
    p.setColor(QPalette::Text, theme.foreground);
    setPalette(p);
    // Reset default text colour for all future output
    currentFormat = QTextCharFormat();
    currentFormat.setForeground(theme.foreground);
}

void TerminalWidget::setArgs(const QStringList &)
{
    // Args must be passed before startPty; no-op after launch.
}

void TerminalWidget::writeToShell(const QString &text)
{
    writeToPty(text.toUtf8());
}

void TerminalWidget::executeCommand(const QString &command)
{
    writeToPty((command + "\r").toUtf8());
}

void TerminalWidget::executeCommandSilent(const QString &command)
{
    // Write to a temp file and source() with echo=FALSE (same trick as Unix build)
    QString tmpPath = QDir::tempPath()
        + QString("/rgui2_cmd_%1.R").arg(QCoreApplication::applicationPid());
    QFile f(tmpPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << command << "\n";
        f.close();
        tmpPath.replace('\\', '/');
        writeToPty(QString("source('%1', echo=FALSE, print.eval=FALSE, local=FALSE)\r")
                       .arg(tmpPath).toUtf8());
    } else {
        writeToPty((command + "\r").toUtf8());
    }
}

void TerminalWidget::executeRCode(const QString &code)
{
    if (!code.contains('\n')) {
        writeToPty((code.trimmed() + "\r").toUtf8());
        return;
    }
    QString tmpPath = QDir::tempPath()
        + QString("/rgui2_run_%1.R").arg(QCoreApplication::applicationPid());
    QFile f(tmpPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << code;
        f.close();
        tmpPath.replace('\\', '/');
        writeToPty(QString("source('%1', echo=TRUE, max.deparse.length=Inf)\r")
                       .arg(tmpPath).toUtf8());
    } else {
        writeToPty((code + "\r").toUtf8());
    }
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);

    QAction *copy = menu->addAction(tr("Copy"));
    copy->setShortcut(QKeySequence("Ctrl+Shift+C"));
    copy->setEnabled(textCursor().hasSelection());
    connect(copy, &QAction::triggered, this,
            [this]() { QApplication::clipboard()->setText(textCursor().selectedText()); });

    QAction *paste = menu->addAction(tr("Paste"));
    paste->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(paste, &QAction::triggered, this,
            [this]() { writeToPty(QApplication::clipboard()->text().toUtf8()); });

    menu->addSeparator();

    QAction *clear = menu->addAction(tr("Clear"));
    connect(clear, &QAction::triggered, this, [this]() {
        // Send VT clear-screen sequence; also clear the QPlainTextEdit buffer
        writeToPty("\x1b[2J\x1b[H");
        QTextCursor c = textCursor();
        c.select(QTextCursor::Document);
        c.removeSelectedText();
    });

    menu->exec(event->globalPos());
    delete menu;
}

// Required so CMake AUTOMOC sees the Q_OBJECT class defined in this .cpp file
#include "terminalwidget_win.moc"

#endif // Q_OS_WIN
#endif // 0
