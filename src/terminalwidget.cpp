// terminalwidget.cpp  –  xterm.js-based terminal for all platforms.
// PTY backend: POSIX forkpty on Linux/macOS, ConPTY on Windows.

#include "terminalwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMenu>
#include <QProcessEnvironment>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <atomic>

// ── Platform-specific headers ─────────────────────────────────────────────────

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <consoleapi.h>
#  include <processthreadsapi.h>
#  include <QProcess>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/ioctl.h>
#  include <sys/wait.h>
#  include <termios.h>
#  ifdef Q_OS_MACOS
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#endif

// ── ConPtyImpl (Windows only) ─────────────────────────────────────────────────

#ifdef Q_OS_WIN
struct TerminalWidget::ConPtyImpl {
    HPCON  hPCon    = nullptr;
    HANDLE hPtyIn   = INVALID_HANDLE_VALUE;  // we write  → shell stdin
    HANDLE hPtyOut  = INVALID_HANDLE_VALUE;  // we read   ← shell stdout/err
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hThread  = INVALID_HANDLE_VALUE;
    int    cols = 80;
    int    rows = 24;
};
#endif

// ── PtyReaderThread ───────────────────────────────────────────────────────────
// Reads raw bytes from the PTY and emits them; platform-specific handle type.

class PtyReaderThread : public QThread {
    Q_OBJECT
public:
#ifdef Q_OS_WIN
    explicit PtyReaderThread(HANDLE h, QObject *parent = nullptr)
        : QThread(parent), handle(h) {}
#else
    explicit PtyReaderThread(int fd, QObject *parent = nullptr)
        : QThread(parent), fd(fd) {}
#endif

    void requestStop() { stopped.store(true); }

signals:
    void dataReady(QByteArray data);

protected:
    void run() override {
        char buf[4096];
#ifdef Q_OS_WIN
        DWORD n = 0;
        while (!stopped.load()) {
            if (!ReadFile(handle, buf, sizeof(buf), &n, nullptr) || n == 0)
                break;
            emit dataReady(QByteArray(buf, static_cast<int>(n)));
        }
#else
        while (!stopped.load()) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            emit dataReady(QByteArray(buf, static_cast<int>(n)));
        }
#endif
    }

private:
#ifdef Q_OS_WIN
    HANDLE handle;
#else
    int fd;
#endif
    std::atomic<bool> stopped{false};
};

// ── Shared R init-script helper ───────────────────────────────────────────────
// Writes the R profile init script and populates env list entries.

static QString setupREnv(QStringList &env, const QProcessEnvironment &sysEnv,
                         const QString &plotDir)
{
    QString pid      = QString::number(QCoreApplication::applicationPid());
    QString initPath = QDir::tempPath() + "/rgui2_init_" + pid + ".R";
    QFile f(initPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};

    QTextStream out(&f);
    out << "local({\n"
        << "  orig_prof <- Sys.getenv('RGUI2_ORIGINAL_R_PROFILE_USER')\n"
        << "  if (nzchar(orig_prof) && file.exists(orig_prof)) {\n"
        << "    source(orig_prof)\n"
        << "  } else {\n"
        << "    if (file.exists('.Rprofile')) source('.Rprofile')\n"
        << "    else if (file.exists(file.path(Sys.getenv('HOME'), '.Rprofile')))\n"
        << "      source(file.path(Sys.getenv('HOME'), '.Rprofile'))\n"
        << "  }\n"
        << "  if (requireNamespace('rgui2', quietly=TRUE)) {\n"
        << "    library(rgui2)\n"
        << "    rgui2::init_monitor(Sys.getenv('RGUI2_ENV_FILE', unset='/tmp/rgui2_env.json'))\n"
        << "    tryCatch(rgui2::init_help_pane(\n"
        << "      port_file  = Sys.getenv('RGUI2_HELP_PORT_FILE',  unset='/tmp/rgui2_help_port'),\n"
        << "      queue_file = Sys.getenv('RGUI2_HELP_QUEUE_FILE', unset='/tmp/rgui2_help_queue'),\n"
        << "      url_file   = Sys.getenv('RGUI2_HELP_URL_FILE',   unset='/tmp/rgui2_help_url')\n"
        << "    ), error = function(e) NULL)\n"
        << "  }\n"
        << "\n"
        << "  # ── R GUI plot capture ────────────────────────────────────────────────────\n"
        << "  local({\n"
        << "    plot_dir <- Sys.getenv('RGUI2_PLOT_DIR', unset = file.path(tempdir(), 'rgui2_plots'))\n"
        << "    dir.create(plot_dir, showWarnings = FALSE, recursive = TRUE)\n"
        << "    index_file <- file.path(plot_dir, 'rgui2_plot_index.txt')\n"
        << "    dev_counter  <- 0L\n"
        << "    snap_counter <- 0L\n"
        << "    last_snap_size <- -1L\n"
        << "\n"
        << "    options(device = function(width = 7, height = 5, ...) {\n"
        << "      dev_counter <<- dev_counter + 1L\n"
        << "      fname <- file.path(plot_dir, sprintf('rgui2_dev_%06d.png', dev_counter))\n"
        << "      grDevices::png(fname,\n"
        << "                     width  = round(width  * 96),\n"
        << "                     height = round(height * 96),\n"
        << "                     res    = 96, ...)\n"
        << "      grDevices::dev.control(displaylist = 'enable')\n"
        << "      invisible(NULL)\n"
        << "    })\n"
        << "\n"
        << "    addTaskCallback(function(expr, value, ok, visible) {\n"
        << "      if (grDevices::dev.cur() != 1L) {\n"
        << "        tryCatch({\n"
        << "          recorded <- grDevices::recordPlot()\n"
        << "          if (length(recorded[[1]]) > 0L) {\n"
        << "            curr_file <- file.path(plot_dir, 'rgui2_current.png')\n"
        << "            grDevices::png(curr_file, width=800L, height=600L, res=96L)\n"
        << "            grDevices::replayPlot(recorded)\n"
        << "            grDevices::dev.off()\n"
        << "            new_size <- file.size(curr_file)\n"
        << "            if (!identical(new_size, last_snap_size)) {\n"
        << "              last_snap_size <<- new_size\n"
        << "              snap_counter <<- snap_counter + 1L\n"
        << "              snap_file <- file.path(plot_dir,\n"
        << "                                     sprintf('rgui2_snap_%06d.png', snap_counter))\n"
        << "              file.copy(curr_file, snap_file, overwrite = TRUE)\n"
        << "              writeLines(snap_file, index_file)\n"
        << "            }\n"
        << "          }\n"
        << "        }, error = function(e) NULL)\n"
        << "      }\n"
        << "      TRUE\n"
        << "    }, name = 'rgui2_plot_capture')\n"
        << "  })\n"
        << "})\n";
    f.close();

    QString origProf = sysEnv.value("R_PROFILE_USER");
    if (!origProf.isEmpty())
        env << "RGUI2_ORIGINAL_R_PROFILE_USER=" + origProf;
    env << "R_PROFILE_USER=" + initPath;
    env << "RGUI2_PLOT_DIR=" + plotDir;
    env << "RGUI2_ENV_FILE=" + QDir::tempPath() + "/rgui2_env.json";
    return initPath;
}

// ── TerminalBridge implementation ─────────────────────────────────────────────

void TerminalBridge::sendInput(const QString &data)
{
    if (tw) tw->writeToPty(data.toUtf8());
}

void TerminalBridge::resizePty(int cols, int rows)
{
    if (tw) tw->doResize(cols, rows);
}

void TerminalBridge::fontSizeUp()    { if (tw) emit tw->fontSizeAdjustRequested(+1); }
void TerminalBridge::fontSizeDown()  { if (tw) emit tw->fontSizeAdjustRequested(-1); }
void TerminalBridge::fontSizeReset() { if (tw) emit tw->fontSizeAdjustRequested(0);  }

void TerminalBridge::ready()
{
    if (!tw || tw->ptyStarted) return;
    tw->ptyStarted = true;

    // Start the PTY with a default 80×24 size.
    // JS will call resizePty() with the actual FitAddon-measured dimensions immediately after.
    tw->startPty();
    tw->doResize(80, 24);

    // Apply the current editor theme.
    tw->setTheme(tw->currentTheme);
}

// ── Constructor ───────────────────────────────────────────────────────────────

TerminalWidget::TerminalWidget(const QString &shell, QWidget *parent)
    : QWebEngineView(parent)
{
    currentTheme = ThemeManager::instance().currentTheme();

    // ── Resolve shell path ──────────────────────────────────────────────────
#ifdef Q_OS_WIN
    if (!shell.isEmpty() && shell != "R" && shell != "r") {
        shellPath = shell;
    } else if (shell == "R" || shell == "r") {
        QString rHome = QString::fromLocal8Bit(qgetenv("R_HOME"));
        QString rterm;
        if (!rHome.isEmpty())
            rterm = rHome + "/bin/x64/Rterm.exe";
        if (rterm.isEmpty() || !QFileInfo::exists(rterm))
            rterm = QStandardPaths::findExecutable("Rterm");
        if (rterm.isEmpty() || !QFileInfo::exists(rterm)) {
            QProcess probe;
            probe.start("R", {"--slave", "-e", "cat(R.home('bin'))"});
            if (probe.waitForFinished(3000)) {
                QString binDir =
                    QString::fromLocal8Bit(probe.readAllStandardOutput()).trimmed();
                rterm = binDir + "/Rterm.exe";
            }
        }
        shellPath = QFileInfo::exists(rterm) ? rterm : "Rterm.exe";
    } else {
        QString pwsh = QStandardPaths::findExecutable("pwsh");
        shellPath = pwsh.isEmpty() ? QStandardPaths::findExecutable("powershell") : pwsh;
        if (shellPath.isEmpty()) shellPath = "powershell.exe";
    }
#else
    if (!shell.isEmpty()) {
        shellPath = shell;
        if (QFileInfo(shell).fileName().toLower() == "r")
            shellArgs << "--interactive" << "--no-save";
    } else {
        shellPath = QString::fromLocal8Bit(qgetenv("SHELL"));
        if (shellPath.isEmpty()) shellPath = "/bin/bash";
    }
#endif

    // ── Build child environment ─────────────────────────────────────────────
    QProcessEnvironment sysEnv = QProcessEnvironment::systemEnvironment();
    QString plotDir = QDir::tempPath() + "/rgui2_plots";
    QDir().mkpath(plotDir);

#ifdef Q_OS_WIN
    // On Windows the child inherits the parent's environment via CreateProcess.
    // We set needed variables directly in the parent before spawning.
    SetEnvironmentVariableW(L"RGUI2_PLOT_DIR", plotDir.toStdWString().c_str());
    SetEnvironmentVariableW(L"RGUI2_ENV_FILE",
        (QDir::tempPath() + "/rgui2_env.json").toStdWString().c_str());
    {
        QString base = QFileInfo(shellPath).baseName().toLower();
        if (base == "r" || base == "rterm") {
            QStringList dummy;
            QString initPath = setupREnv(dummy, sysEnv, plotDir);
            if (!initPath.isEmpty()) {
                QString orig = sysEnv.value("R_PROFILE_USER");
                if (!orig.isEmpty())
                    SetEnvironmentVariableW(L"RGUI2_ORIGINAL_R_PROFILE_USER",
                                            orig.toStdWString().c_str());
                SetEnvironmentVariableW(L"R_PROFILE_USER",
                                        initPath.toStdWString().c_str());
            }
        }
    }
#else
    // Build an explicit envp for execve.
    for (const QString &key : sysEnv.keys()) {
        if (key != "LANG" && key != "LC_ALL" && key != "TERM" && key != "R_PROFILE_USER")
            envList << key + "=" + sysEnv.value(key);
    }
    envList << "LANG=en_US.UTF-8"
            << "LC_ALL=en_US.UTF-8"
            << "TERM=xterm-256color"
            << "RGUI2_PLOT_DIR=" + plotDir;

    if (QFileInfo(shellPath).fileName().toLower() == "r")
        setupREnv(envList, sysEnv, plotDir);
#endif

    // ── Set up QWebChannel ──────────────────────────────────────────────────
    channel = new QWebChannel(this);
    bridge  = new TerminalBridge(this);
    bridge->tw = this;
    channel->registerObject(QStringLiteral("bridge"), bridge);
    page()->setWebChannel(channel);

    // ── Load the xterm.js terminal page ────────────────────────────────────
    // PTY is started when xterm.js calls bridge.ready() after QWebChannel init.
    page()->load(QUrl(QStringLiteral("qrc:///xterm/terminal.html")));

    // Re-fit after load so xterm.js measures the real (laid-out) widget size,
    // not the default viewport size the engine uses before layout settles.
    connect(page(), &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (!ok) return;
        pageLoaded = true;
        // Apply any font size requested before the page was ready.
        if (pendingFontSize > 0) {
            const int s = pendingFontSize;
            page()->runJavaScript(QString(
                "if (window.setFontSize) {"
                "  window.setFontSize(%1);"
                "} else if (window.resetFontSize && window.adjustFontSize) {"
                "  window.resetFontSize();"
                "  window.adjustFontSize(%1 - 11);"
                "}").arg(s));
        }
        QTimer::singleShot(100, this, [this]() {
            page()->runJavaScript("if(window.fitTerminal) window.fitTerminal();");
        });
    });
}

void TerminalWidget::setInitialFontSize(int pt)
{
    pendingFontSize = pt;
    if (pageLoaded) {
        page()->runJavaScript(QString(
            "if (window.setFontSize) {"
            "  window.setFontSize(%1);"
            "} else if (window.resetFontSize && window.adjustFontSize) {"
            "  window.resetFontSize();"
            "  window.adjustFontSize(%1 - 11);"
            "}").arg(pt));
    }
}

// ── Destructor ────────────────────────────────────────────────────────────────

TerminalWidget::~TerminalWidget()
{
#ifdef Q_OS_WIN
    // Terminate the shell process first - this causes ReadFile in the reader
    // thread to return, so the thread exits on its own.
    if (pty) {
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
        pty = nullptr;
    }
#else
    // 1. Kill the shell so the PTY master sees EOF.
    if (shellPid > 0) {
        ::kill(shellPid, SIGTERM);
    }
    // 2. Close the PTY fd - this unblocks ::read() in PtyReaderThread.
    if (ptyFd >= 0) {
        ::close(ptyFd);
        ptyFd = -1;
    }
#endif
    // 3. Wait for the reader thread to finish (fd/handle closure makes it exit).
    if (ptyReader) {
        ptyReader->requestStop();
        if (!ptyReader->wait(3000)) {
            ptyReader->terminate();
            ptyReader->wait(1000);
        }
        ptyReader = nullptr;
    }
#ifndef Q_OS_WIN
    // 4. Reap the child process.
    if (shellPid > 0) {
        int status;
        ::waitpid(shellPid, &status, WNOHANG);
        shellPid = -1;
    }
#endif
}

// ── PTY startup ───────────────────────────────────────────────────────────────

#ifdef Q_OS_WIN

void TerminalWidget::startPty()
{
    pty = new ConPtyImpl;

    // Initial size: compute from widget dimensions using the terminal font.
    QFont font;
    font.setFamily("Hack Nerd Font Mono");
    font.setPointSize(10);
    QFontMetrics fm(font);
    pty->cols = qMax(40, width()  / qMax(1, fm.horizontalAdvance('M')));
    pty->rows = qMax(10, height() / qMax(1, fm.height()));

    COORD sz = { static_cast<SHORT>(pty->cols), static_cast<SHORT>(pty->rows) };

    HANDLE hIn_r, hIn_w, hOut_r, hOut_w;
    if (!CreatePipe(&hIn_r, &hIn_w, nullptr, 0) ||
        !CreatePipe(&hOut_r, &hOut_w, nullptr, 0)) {
        return;
    }

    HRESULT hr = CreatePseudoConsole(sz, hIn_r, hOut_w, 0, &pty->hPCon);
    CloseHandle(hIn_r);
    CloseHandle(hOut_w);
    if (FAILED(hr)) {
        CloseHandle(hIn_w);
        CloseHandle(hOut_r);
        return;
    }

    pty->hPtyIn  = hIn_w;
    pty->hPtyOut = hOut_r;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    auto *attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attrSize));
    if (!attrList) return;
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              pty->hPCon, sizeof(HPCON), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb  = sizeof(si);
    si.lpAttributeList = attrList;

    QString cmd = shellPath.contains(' ') ? '"' + shellPath + '"' : shellPath;
    for (const QString &a : shellArgs)
        cmd += ' ' + (a.contains(' ') ? '"' + a + '"' : a);
    std::wstring wCmd = cmd.toStdWString();

    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, wCmd.data(), nullptr, nullptr, FALSE,
                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                   nullptr, nullptr, &si.StartupInfo, &pi);

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    pty->hProcess = pi.hProcess;
    pty->hThread  = pi.hThread;

    auto *reader = new PtyReaderThread(pty->hPtyOut, this);
    ptyReader = reader;
    connect(reader, &PtyReaderThread::dataReady,
            this,   &TerminalWidget::sendOutput,
            Qt::QueuedConnection);
    connect(reader, &PtyReaderThread::finished, reader, &QObject::deleteLater);
    connect(reader, &PtyReaderThread::finished, this,   &TerminalWidget::onPtyReaderFinished,
            Qt::QueuedConnection);
    reader->start();
}

#else  // ── POSIX forkpty ─────────────────────────────────────────────────────

void TerminalWidget::startPty()
{
    // Initial terminal size (corrected by doResize immediately after).
    struct winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;

    // Build argv for execve.
    QList<QByteArray> argBufs;
    QList<const char *> argv;
    argBufs << shellPath.toLocal8Bit();
    argv << argBufs.last().constData();
    for (const QString &a : shellArgs) {
        argBufs << a.toLocal8Bit();
        argv << argBufs.last().constData();
    }
    argv << nullptr;

    // Build envp for execve.
    QList<QByteArray> envBufs;
    QList<const char *> envp;
    for (const QString &e : envList) {
        envBufs << e.toLocal8Bit();
        envp << envBufs.last().constData();
    }
    envp << nullptr;

    // Resolve to absolute path so execve can find the binary (it doesn't search PATH).
    QString resolvedPath = shellPath;
    if (!QFileInfo(shellPath).isAbsolute()) {
        QString found = QStandardPaths::findExecutable(shellPath);
        if (!found.isEmpty()) resolvedPath = found;
    }
    QByteArray resolvedBuf = resolvedPath.toLocal8Bit();
    argv[0] = resolvedBuf.constData();

    shellPid = forkpty(&ptyFd, nullptr, nullptr, &ws);
    if (shellPid == 0) {
        // Child: change to the requested working directory (fallback to home).
        const QString startDir = m_workingDir.isEmpty() ? QDir::homePath() : m_workingDir;
        ::chdir(startDir.toLocal8Bit().constData());
        ::execve(argv[0],
                 const_cast<char *const *>(argv.data()),
                 const_cast<char *const *>(envp.data()));
        ::_exit(1);  // execve failed
    }
    if (shellPid < 0) return;  // forkpty failed

    auto *reader = new PtyReaderThread(ptyFd, this);
    ptyReader = reader;
    connect(reader, &PtyReaderThread::dataReady,
            this,   &TerminalWidget::sendOutput,
            Qt::QueuedConnection);
    connect(reader, &PtyReaderThread::finished, reader, &QObject::deleteLater);
    connect(reader, &PtyReaderThread::finished, this,   &TerminalWidget::onPtyReaderFinished,
            Qt::QueuedConnection);
    reader->start();
}

#endif  // Q_OS_WIN

// ── PTY process-exit / auto-restart ──────────────────────────────────────────

void TerminalWidget::onPtyReaderFinished()
{
    ptyReader = nullptr;  // already scheduled for deleteLater

#ifndef Q_OS_WIN
    if (shellPid > 0) {
        int status;
        ::waitpid(shellPid, &status, 0);   // child already exited; won't block
        shellPid = -1;
    }
    if (ptyFd >= 0) {
        ::close(ptyFd);
        ptyFd = -1;
    }
#else
    if (pty) {
        if (pty->hProcess != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(pty->hProcess, 2000);
            CloseHandle(pty->hProcess); pty->hProcess = INVALID_HANDLE_VALUE;
        }
        if (pty->hThread  != INVALID_HANDLE_VALUE) {
            CloseHandle(pty->hThread);  pty->hThread  = INVALID_HANDLE_VALUE;
        }
        if (pty->hPCon)   { ClosePseudoConsole(pty->hPCon); pty->hPCon = nullptr; }
        if (pty->hPtyIn   != INVALID_HANDLE_VALUE) {
            CloseHandle(pty->hPtyIn);  pty->hPtyIn  = INVALID_HANDLE_VALUE;
        }
        if (pty->hPtyOut  != INVALID_HANDLE_VALUE) {
            CloseHandle(pty->hPtyOut); pty->hPtyOut = INVALID_HANDLE_VALUE;
        }
        delete pty;
        pty = nullptr;
    }
#endif

    // Only auto-restart for R sessions.
    if (QFileInfo(shellPath).fileName().toLower() != "r")
        return;

    // Print a notice in the terminal before restarting.
    page()->runJavaScript(
        "if(window.term){"
        "  term.write('\\r\\n\\x1b[33m[R session ended \u2013 restarting\u2026]\\x1b[0m\\r\\n');"
        "}");

    QTimer::singleShot(600, this, [this]() {
        startPty();
        page()->runJavaScript("if(window.fitTerminal) window.fitTerminal();");
    });
}

// ── PTY I/O ───────────────────────────────────────────────────────────────────

void TerminalWidget::writeToPty(const QByteArray &data)
{
#ifdef Q_OS_WIN
    if (!pty || pty->hPtyIn == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(pty->hPtyIn, data.constData(),
              static_cast<DWORD>(data.size()), &written, nullptr);
#else
    if (ptyFd < 0) return;
    ::write(ptyFd, data.constData(), static_cast<size_t>(data.size()));
#endif
}

// Called on the main thread via Qt::QueuedConnection from PtyReaderThread.
void TerminalWidget::sendOutput(const QByteArray &data)
{
    // Base64-encode for safe transport through QWebChannel's JSON serialisation.
    emit bridge->outputReady(QString::fromLatin1(data.toBase64()));
}

void TerminalWidget::doResize(int cols, int rows)
{
#ifdef Q_OS_WIN
    if (!pty || !pty->hPCon) return;
    if (cols == pty->cols && rows == pty->rows) return;
    pty->cols = cols;
    pty->rows = rows;
    COORD sz = { static_cast<SHORT>(cols), static_cast<SHORT>(rows) };
    ResizePseudoConsole(pty->hPCon, sz);
#else
    if (ptyFd < 0) return;
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(ptyFd, TIOCSWINSZ, &ws);
#endif
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void TerminalWidget::setTheme(const EditorTheme &theme)
{
    currentTheme = theme;
    emit bridge->themeChanged(theme.background.name(),
                               theme.foreground.name(),
                               theme.foreground.name());
}

// ── Resize ────────────────────────────────────────────────────────────────────

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWebEngineView::resizeEvent(event);
    // Always re-fit: doResize() is a no-op until ptyFd is valid, and
    // fitTerminal() is guarded on the JS side, so this is always safe.
    page()->runJavaScript("if(window.fitTerminal) window.fitTerminal();");
}

// ── Public API ────────────────────────────────────────────────────────────────

void TerminalWidget::setArgs(const QStringList &args)
{
    shellArgs = args;
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
    // Write to a temp file and source() with echo=FALSE so the command
    // text itself is not echoed in the terminal (same trick on all platforms).
    // NOTE: R uses GNU readline which redisplays input; the source(...) line
    // will still be visible in the console. For truly invisible execution,
    // route the command through the rgui2::init_help_pane queue mechanism.
    QString pid     = QString::number(QCoreApplication::applicationPid());
    QString tmpPath = QDir::tempPath() + "/rgui2_cmd_" + pid + ".R";
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        executeCommand(command);
        return;
    }
    QTextStream out(&f);
    out << command << "\n";
    f.close();
    tmpPath.replace('\\', '/');
    writeToPty(QString("source('%1', echo=FALSE, print.eval=FALSE, local=FALSE)\r")
                   .arg(tmpPath).toUtf8());
}

void TerminalWidget::executeRCode(const QString &code)
{
    if (!code.contains('\n')) {
        writeToPty((code.trimmed() + "\r").toUtf8());
        return;
    }
    // Multi-line: write to a temp file and source() with echo=TRUE so R prints
    // each expression with indentation preserved.
    QString pid     = QString::number(QCoreApplication::applicationPid());
    QString tmpPath = QDir::tempPath() + "/rgui2_run_" + pid + ".R";
    QFile f(tmpPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << code;
        f.close();
        tmpPath.replace('\\', '/');
        writeToPty(QString("source('%1', echo=TRUE, max.deparse.length=Inf)\r")
                       .arg(tmpPath).toUtf8());
    } else {
        executeCommand(code);
    }
}

// ── Context menu ──────────────────────────────────────────────────────────────

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);

    QAction *copy = menu->addAction(tr("Copy"));
    copy->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(copy, &QAction::triggered, this,
            [this]() { page()->triggerAction(QWebEnginePage::Copy); });

    QAction *paste = menu->addAction(tr("Paste"));
    paste->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(paste, &QAction::triggered, this, [this]() {
        writeToPty(QApplication::clipboard()->text().toUtf8());
    });

    menu->addSeparator();

    QAction *clear = menu->addAction(tr("Clear"));
    connect(clear, &QAction::triggered, this, [this]() {
        if (QFileInfo(shellPath).fileName().toLower() == "r")
            executeCommand("clear()");
        else
            writeToPty("\x0c");  // Ctrl+L clears most shells
    });

    menu->exec(event->globalPos());
    delete menu;
}

// Required for Q_OBJECT classes (PtyReaderThread) defined in this .cpp file.
#include "terminalwidget.moc"
