// terminalwidget.cpp  –  native (QPainter + VtScreen) terminal for all platforms.
// PTY backend: POSIX forkpty on Linux/macOS, ConPTY on Windows.

#include "terminalwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
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

static QString findWindowsRterm(QString *rHomeOut = nullptr)
{
    auto accept = [rHomeOut](const QString &rHome) {
        const QStringList candidates = {
            QDir(rHome).filePath("bin/x64/Rterm.exe"),
            QDir(rHome).filePath("bin/Rterm.exe")
        };
        for (const QString &candidate : candidates) {
            if (QFileInfo(candidate).isFile()) {
                if (rHomeOut) *rHomeOut = QDir::cleanPath(rHome);
                return QDir::cleanPath(candidate);
            }
        }
        return QString();
    };

    const QString envHome = QString::fromLocal8Bit(qgetenv("R_HOME"));
    if (!envHome.isEmpty()) {
        const QString found = accept(envHome);
        if (!found.isEmpty()) return found;
    }

    QString found = QStandardPaths::findExecutable("Rterm.exe");
    if (!found.isEmpty() && QFileInfo(found).isFile()) {
        QDir binDir = QFileInfo(found).dir();
        if (binDir.dirName().compare("x64", Qt::CaseInsensitive) == 0)
            binDir.cdUp();
        if (rHomeOut) *rHomeOut = binDir.filePath("..");
        return QDir::cleanPath(found);
    }

    auto readRegistryHome = [](HKEY root, REGSAM view) {
        HKEY key = nullptr;
        const LONG openResult = RegOpenKeyExW(
            root, L"SOFTWARE\\R-core\\R64", 0, KEY_READ | view, &key);
        if (openResult != ERROR_SUCCESS) return QString();

        wchar_t value[1024] = {};
        DWORD type = 0;
        DWORD size = sizeof(value);
        QString result;
        if (RegQueryValueExW(key, L"InstallPath", nullptr, &type,
                             reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            result = QString::fromWCharArray(value);
        }
        RegCloseKey(key);
        return result;
    };

    for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        for (const HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            const QString home = readRegistryHome(root, view);
            if (!home.isEmpty()) {
                found = accept(home);
                if (!found.isEmpty()) return found;
            }
        }
    }
    return {};
}
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
                         const QString &plotChannelFile, const QString &plotSizeFile)
{
    QString pid      = QString::number(QCoreApplication::applicationPid());
    QString initPath = QDir::tempPath() + "/simpler_init_" + pid + ".R";
    QFile f(initPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};

    QTextStream out(&f);
    out << "invisible(local({\n"
        << "  orig_prof <- Sys.getenv('SIMPLER_ORIGINAL_R_PROFILE_USER')\n"
        << "  if (nzchar(orig_prof) && file.exists(orig_prof)) {\n"
        << "    source(orig_prof)\n"
        << "  } else {\n"
        << "    if (file.exists('.Rprofile')) source('.Rprofile')\n"
        << "    else if (file.exists(file.path(Sys.getenv('HOME'), '.Rprofile')))\n"
        << "      source(file.path(Sys.getenv('HOME'), '.Rprofile'))\n"
        << "  }\n"
        << "  if (requireNamespace('simplerapi', quietly=TRUE)) {\n"
        << "    library(simplerapi)\n"
        << "    simplerapi::init_monitor(Sys.getenv('SIMPLER_ENV_FILE', unset='/tmp/simpler_env.json'))\n"
        << "    tryCatch(simplerapi::init_help_pane(\n"
        << "      port_file  = Sys.getenv('SIMPLER_HELP_PORT_FILE',  unset='/tmp/simpler_help_port'),\n"
        << "      queue_file = Sys.getenv('SIMPLER_HELP_QUEUE_FILE', unset='/tmp/simpler_help_queue'),\n"
        << "      url_file   = Sys.getenv('SIMPLER_HELP_URL_FILE',   unset='/tmp/simpler_help_url')\n"
        << "    ), error = function(e) NULL)\n"
        << "    tryCatch(simplerapi::init_plot_capture(\n"
        << "      Sys.getenv('SIMPLER_PLOT_PORT_FILE', unset=file.path(tempdir(), 'simpler_plot_port')),\n"
        << "      Sys.getenv('SIMPLER_PLOT_SIZE_FILE', unset=file.path(tempdir(), 'simpler_plot_size'))\n"
        << "    ), error = function(e) NULL)\n"
        << "  }\n"
        << "}))\n";
    f.close();

    QString origProf = sysEnv.value("R_PROFILE_USER");
    if (!origProf.isEmpty())
        env << "SIMPLER_ORIGINAL_R_PROFILE_USER=" + origProf;
    env << "R_PROFILE_USER=" + initPath;
    env << "SIMPLER_ENV_FILE=" + QDir::tempPath() + "/simpler_env.json";
    env << "SIMPLER_PLOT_PORT_FILE=" + plotChannelFile;
    env << "SIMPLER_PLOT_SIZE_FILE=" + plotSizeFile;
    return initPath;
}

// ── Constructor ───────────────────────────────────────────────────────────────

TerminalWidget::TerminalWidget(const QString &shell, QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_screen(80, 24)
{
    currentTheme = ThemeManager::instance().currentTheme();

    // ── Resolve shell path ──────────────────────────────────────────────────
#ifdef Q_OS_WIN
    if (!shell.isEmpty() && shell != "R" && shell != "r") {
        shellPath = shell;
    } else if (shell == "R" || shell == "r") {
        QString rHome;
        QString rterm = findWindowsRterm(&rHome);
        if (rterm.isEmpty() || !QFileInfo::exists(rterm)) {
            QProcess probe;
            probe.start("R", {"--slave", "-e", "cat(R.home('bin'))"});
            if (probe.waitForFinished(3000)) {
                QString binDir =
                    QString::fromLocal8Bit(probe.readAllStandardOutput()).trimmed();
                rterm = binDir + "/Rterm.exe";
            }
        }
        shellPath = QFileInfo::exists(rterm) ? rterm : QString();
        if (!rHome.isEmpty()) {
            SetEnvironmentVariableW(L"R_HOME", rHome.toStdWString().c_str());
            const QString rBin = QFileInfo(rterm).absolutePath();
            const QString path = QString::fromLocal8Bit(qgetenv("PATH"));
            if (!path.split(';', Qt::SkipEmptyParts).contains(rBin, Qt::CaseInsensitive))
                SetEnvironmentVariableW(L"PATH",
                    (rBin + ";" + path).toStdWString().c_str());
        }
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
    QString plotChannelFile = QDir::tempPath() + "/simpler_plot_port";
    QString plotSizeFile = QDir::tempPath() + "/simpler_plot_size";

#ifdef Q_OS_WIN
    // On Windows the child inherits the parent's environment via CreateProcess.
    // We set needed variables directly in the parent before spawning.
    {
        QString tmpDir = QDir::tempPath();
        SetEnvironmentVariableW(L"SIMPLER_ENV_FILE",
            (tmpDir + "/simpler_env.json").toStdWString().c_str());
        SetEnvironmentVariableW(L"SIMPLER_HELP_PORT_FILE",
            (tmpDir + "/simpler_help_port").toStdWString().c_str());
        SetEnvironmentVariableW(L"SIMPLER_HELP_QUEUE_FILE",
            (tmpDir + "/simpler_help_queue").toStdWString().c_str());
        SetEnvironmentVariableW(L"SIMPLER_HELP_URL_FILE",
            (tmpDir + "/simpler_help_url").toStdWString().c_str());
        SetEnvironmentVariableW(L"SIMPLER_PLOT_PORT_FILE",
            plotChannelFile.toStdWString().c_str());
        SetEnvironmentVariableW(L"SIMPLER_PLOT_SIZE_FILE",
            plotSizeFile.toStdWString().c_str());
    }
    {
        QString base = QFileInfo(shellPath).baseName().toLower();
        if (base == "r" || base == "rterm") {
            QStringList dummy;
            QString initPath = setupREnv(dummy, sysEnv, plotChannelFile, plotSizeFile);
            if (!initPath.isEmpty()) {
                QString orig = sysEnv.value("R_PROFILE_USER");
                if (!orig.isEmpty())
                    SetEnvironmentVariableW(L"SIMPLER_ORIGINAL_R_PROFILE_USER",
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
            << "TERM=xterm-256color";

    if (QFileInfo(shellPath).fileName().toLower() == "r")
        setupREnv(envList, sysEnv, plotChannelFile, plotSizeFile);
#endif

    // ── Widget setup ────────────────────────────────────────────────────────
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setCursor(Qt::IBeamCursor);
    verticalScrollBar()->setSingleStep(1);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        m_stickToBottom = verticalScrollBar()->value() == verticalScrollBar()->maximum();
        viewport()->update();
    });

    m_font = QFont(ThemeManager::instance().monospaceFont());
    m_font.setStyleHint(QFont::Monospace);
    m_font.setPointSize(12);
    updateFontMetrics();
    updateGrid();

    m_screen.setReplyHandler([this](const QByteArray &b) { writeToPty(b); });

    // Start the child once the widget has had a chance to receive its real size.
    QTimer::singleShot(0, this, [this]() {
        if (ptyStarted) return;
        ptyStarted = true;
        updateGrid();
        startPty();
        doResize(m_cols, m_rows);
    });
}

void TerminalWidget::setInitialFontSize(int pt)
{
    if (pt <= 0) return;
    m_font.setPointSize(pt);
    updateFontMetrics();
    updateGrid();
    viewport()->update();
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
    if (ptySlaveFd >= 0) {
        ::close(ptySlaveFd);
        ptySlaveFd = -1;
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

    // Initial size comes from the current terminal grid.
    pty->cols = m_cols;
    pty->rows = m_rows;

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
    const BOOL launched = CreateProcessW(
        nullptr, wCmd.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &si.StartupInfo, &pi);

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    if (!launched) {
        const DWORD error = GetLastError();
        pty->hProcess = INVALID_HANDLE_VALUE;
        pty->hThread = INVALID_HANDLE_VALUE;
        const QString message = QString("Unable to start R. Rterm.exe was not found or could not be "
                                        "launched (Windows error %1).").arg(error);
        qWarning().noquote() << message;
        showMessage(message, 1);
        CloseHandle(pty->hPtyIn);
        CloseHandle(pty->hPtyOut);
        ClosePseudoConsole(pty->hPCon);
        pty->hPtyIn = INVALID_HANDLE_VALUE;
        pty->hPtyOut = INVALID_HANDLE_VALUE;
        pty->hPCon = nullptr;
        return;
    }

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
    ws.ws_col = static_cast<unsigned short>(m_cols);
    ws.ws_row = static_cast<unsigned short>(m_rows);

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

    char slaveName[256] = {};
    shellPid = forkpty(&ptyFd, slaveName, nullptr, &ws);
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
    ptySlaveFd = ::open(slaveName, O_RDWR | O_NOCTTY);

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
    if (ptySlaveFd >= 0) {
        ::close(ptySlaveFd);
        ptySlaveFd = -1;
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
    showMessage(QStringLiteral("[R session ended \u2013 restarting\u2026]"), 3);

    QTimer::singleShot(600, this, [this]() {
        startPty();
        doResize(m_cols, m_rows);
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

// Writes data to the pty with local ECHO temporarily disabled, so the raw
// bytes we inject (e.g. an internal `source('...')` wrapper call) never
// appear in the terminal transcript. R's own prompt/echo (from `echo=TRUE`
// inside source(), or the next "> " prompt) is unaffected since those are
// printed by R itself, not echoed by the pty line discipline. Only used on
// POSIX; on Windows the ConPTY-hosted console owns its own echo and cannot
// be toggled from here, so this falls back to a normal (visible) write.
void TerminalWidget::writeToPtySilent(const QByteArray &data)
{
#ifdef Q_OS_WIN
    writeToPty(data);
#else
    if (ptyFd < 0) return;
    const int terminalFd = ptySlaveFd >= 0 ? ptySlaveFd : ptyFd;
    struct termios tio{};
    bool haveTio = (::tcgetattr(terminalFd, &tio) == 0);
    if (haveTio) {
        struct termios noecho = tio;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL);
        ::tcsetattr(terminalFd, TCSANOW, &noecho);
    }
    ::write(ptyFd, data.constData(), static_cast<size_t>(data.size()));
    if (haveTio) {
        // The line discipline may echo the bytes after write() returns.
        // Keep echo disabled long enough for R to consume the line, then
        // restore it so immediately following user input remains visible.
        QTimer::singleShot(250, this, [this, terminalFd, tio]() {
            if (ptySlaveFd >= 0)
                ::tcsetattr(terminalFd, TCSANOW, &tio);
        });
    }
#endif
}

// Called on the main thread via Qt::QueuedConnection from PtyReaderThread.
void TerminalWidget::sendOutput(const QByteArray &data)
{
    QByteArray visibleData = data;
    if (suppressInternalOutput) {
        internalOutputBuffer.append(data);
        const qsizetype markerIndex = internalOutputBuffer.indexOf(internalOutputMarker);
        if (markerIndex < 0)
            return;
        visibleData = internalOutputBuffer.mid(
            markerIndex + internalOutputMarker.size());
        suppressInternalOutput = false;
        suppressInternalPrompt = true;
        internalOutputBuffer.clear();
    }

    if (suppressInternalPrompt) {
        internalOutputBuffer.append(visibleData);
        const QString pending = QString::fromUtf8(internalOutputBuffer);
        static const QRegularExpression internalPrompt(
            QStringLiteral(R"(^(?:\r?\n)*\s*(?:>\s*)+)"));
        const QRegularExpressionMatch match = internalPrompt.match(pending);
        if (!match.hasMatch())
            return;
        const qsizetype consumed = match.capturedLength();
        visibleData = internalOutputBuffer.mid(consumed);
        internalOutputBuffer.clear();
        suppressInternalPrompt = false;
    }

    QString output = m_decoder(visibleData);
    static const QRegularExpression internalSource(
        QStringLiteral(R"(>?\s*source\('/tmp/simpler_cmd_[0-9]+\.R', echo=FALSE, print\.eval=FALSE, local=FALSE\))"));
    output.replace(internalSource, QString());
    if (output.isEmpty())
        return;

    m_screen.feed(output);
    updateScrollBar();
    viewport()->update();
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

// ── Theme / font ──────────────────────────────────────────────────────────────

void TerminalWidget::setTheme(const EditorTheme &theme)
{
    currentTheme = theme;
    viewport()->update();
}

void TerminalWidget::setFontFamily(const QString &family)
{
    m_font.setFamily(family);
    updateFontMetrics();
    updateGrid();
    viewport()->update();
}

void TerminalWidget::updateFontMetrics()
{
    m_font.setKerning(false);
    QFontMetrics fm(m_font);
    m_cellW = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qMax(1, fm.height());
    m_ascent = fm.ascent();
}

void TerminalWidget::updateGrid()
{
    const int cols = qMax(2, viewport()->width() / m_cellW);
    const int rows = qMax(1, viewport()->height() / m_cellH);
    if (cols != m_cols || rows != m_rows) {
        m_cols = cols;
        m_rows = rows;
        m_screen.resize(cols, rows);
        doResize(cols, rows);
    }
    updateScrollBar();
}

void TerminalWidget::updateScrollBar()
{
    QScrollBar *bar = verticalScrollBar();
    const int maxv = qMax(0, m_screen.lineCount() - m_rows);
    bar->setRange(0, maxv);
    bar->setPageStep(m_rows);
    if (m_stickToBottom)
        bar->setValue(maxv);
}

void TerminalWidget::showMessage(const QString &text, int ansiColor)
{
    m_screen.feed(QStringLiteral("\r\n\x1b[3%1m%2\x1b[0m\r\n").arg(ansiColor).arg(text));
    updateScrollBar();
    viewport()->update();
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateGrid();
}

// ── Painting ──────────────────────────────────────────────────────────────────

QColor TerminalWidget::paletteColor(qint32 code, bool isFg) const
{
    if (code < 0)
        return isFg ? currentTheme.foreground : currentTheme.background;
    if (code >= 0x01000000)
        return QColor((code >> 16) & 0xFF, (code >> 8) & 0xFF, code & 0xFF);

    const QColor *ansi[16] = {
        &currentTheme.color_01, &currentTheme.color_02, &currentTheme.color_03, &currentTheme.color_04,
        &currentTheme.color_05, &currentTheme.color_06, &currentTheme.color_07, &currentTheme.color_08,
        &currentTheme.color_09, &currentTheme.color_10, &currentTheme.color_11, &currentTheme.color_12,
        &currentTheme.color_13, &currentTheme.color_14, &currentTheme.color_15, &currentTheme.color_16};
    if (code < 16)
        return *ansi[code];
    if (code < 232) {
        const int i = code - 16;
        auto lvl = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(lvl(i / 36), lvl((i / 6) % 6), lvl(i % 6));
    }
    const int g = 8 + (code - 232) * 10;
    return QColor(g, g, g);
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), currentTheme.background);
    p.setFont(m_font);

    const int first = verticalScrollBar()->value();
    const int total = m_screen.lineCount();
    const int curLine = m_screen.cursorLine();
    const int curCol = m_screen.cursorCol();
    const bool atBottom = first >= total - m_rows;
    const qint64 dropped = m_screen.droppedLines();

    QPoint selA = m_selAnchor, selB = m_selHead;
    if (selB.y() < selA.y() || (selB.y() == selA.y() && selB.x() < selA.x()))
        std::swap(selA, selB);
    auto selected = [&](int line, int col) {
        if (!m_hasSelection) return false;
        const qint64 abs = line + dropped;
        if (abs < selA.y() || abs > selB.y()) return false;
        if (abs == selA.y() && col < selA.x()) return false;
        if (abs == selB.y() && col > selB.x()) return false;
        return true;
    };

    quint8 curFlags = 0xFF;
    const int lastVisible = qMin(total, first + m_rows + 1);
    for (int li = first; li < lastVisible; ++li) {
        const VtScreen::Line &line = m_screen.line(li);
        const int y = (li - first) * m_cellH;
        const int ncols = qMax<int>(line.size(), 0);
        for (int c = 0; c < ncols; ++c) {
            const VtCell &cell = line.at(c);
            const bool isCursor = atBottom && li == curLine && c == curCol && hasFocus();
            const bool sel = selected(li, c);

            QColor fg = paletteColor(
                (cell.flags & VtCell::Bold) && cell.fg >= 0 && cell.fg < 8 ? cell.fg + 8 : cell.fg, true);
            QColor bg = paletteColor(cell.bg, false);
            if (cell.flags & VtCell::Reverse)
                std::swap(fg, bg);
            if (sel) {
                bg = currentTheme.selection;
            }
            if (isCursor) {
                bg = currentTheme.cursor.isValid() ? currentTheme.cursor : currentTheme.foreground;
                fg = currentTheme.background;
            }

            const int x = c * m_cellW;
            const bool needBg = isCursor || sel || cell.bg >= 0 || (cell.flags & VtCell::Reverse);
            if (needBg)
                p.fillRect(x, y, m_cellW, m_cellH, bg);

            if (cell.ch != U' ') {
                if (cell.flags != curFlags) {
                    QFont f = m_font;
                    f.setBold(cell.flags & VtCell::Bold);
                    f.setItalic(cell.flags & VtCell::Italic);
                    f.setUnderline(cell.flags & VtCell::Underline);
                    p.setFont(f);
                    curFlags = cell.flags;
                }
                p.setPen(fg);
                p.drawText(x, y + m_ascent, QString::fromUcs4(&cell.ch, 1));
            }
        }
    }

    // Hollow cursor when unfocused.
    if (atBottom && !hasFocus() && curLine >= first && curLine < first + m_rows) {
        p.setPen(currentTheme.foreground);
        p.drawRect(curCol * m_cellW, (curLine - first) * m_cellH, m_cellW - 1, m_cellH - 1);
    }
}

void TerminalWidget::focusInEvent(QFocusEvent *e)  { QAbstractScrollArea::focusInEvent(e);  viewport()->update(); }
void TerminalWidget::focusOutEvent(QFocusEvent *e) { QAbstractScrollArea::focusOutEvent(e); viewport()->update(); }
bool TerminalWidget::focusNextPrevChild(bool) { return false; }  // Tab belongs to the shell

// ── Keyboard ──────────────────────────────────────────────────────────────────

void TerminalWidget::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;
    const int key = e->key();

    if (ctrl && shift && key == Qt::Key_C) {
        QApplication::clipboard()->setText(selectedText());
        return;
    }
    if ((ctrl && shift && key == Qt::Key_V) || (shift && key == Qt::Key_Insert)) {
        QString t = QApplication::clipboard()->text();
        t.replace("\r\n", "\r").replace('\n', '\r');
        writeToPty(t.toUtf8());
        return;
    }
    if (ctrl && !shift && !alt) {
        if (key == Qt::Key_Plus || key == Qt::Key_Equal) { emit fontSizeAdjustRequested(+1); return; }
        if (key == Qt::Key_Minus) { emit fontSizeAdjustRequested(-1); return; }
        if (key == Qt::Key_0)     { emit fontSizeAdjustRequested(0);  return; }
    }
    if (shift && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        QScrollBar *bar = verticalScrollBar();
        bar->setValue(bar->value() + (key == Qt::Key_PageUp ? -m_rows : m_rows));
        return;
    }

    QByteArray out;
    switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: out = "\r"; break;
    case Qt::Key_Backspace: out = "\x7f"; break;
    case Qt::Key_Tab: out = "\t"; break;
    case Qt::Key_Backtab: out = "\x1b[Z"; break;
    case Qt::Key_Escape: out = "\x1b"; break;
    case Qt::Key_Up: out = "\x1b[A"; break;
    case Qt::Key_Down: out = "\x1b[B"; break;
    case Qt::Key_Right: out = ctrl ? "\x1b[1;5C" : "\x1b[C"; break;
    case Qt::Key_Left: out = ctrl ? "\x1b[1;5D" : "\x1b[D"; break;
    case Qt::Key_Home: out = "\x1b[H"; break;
    case Qt::Key_End: out = "\x1b[F"; break;
    case Qt::Key_Delete: out = "\x1b[3~"; break;
    case Qt::Key_Insert: out = "\x1b[2~"; break;
    case Qt::Key_PageUp: out = "\x1b[5~"; break;
    case Qt::Key_PageDown: out = "\x1b[6~"; break;
    default: {
        const QString text = e->text();
        if (!text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7F) {
            out = text.toUtf8();
        } else if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z) {
            out = QByteArray(1, char(key - Qt::Key_A + 1));
        } else if (!text.isEmpty() && text.at(0).unicode() < 0x20) {
            out = text.toUtf8();
        } else {
            QAbstractScrollArea::keyPressEvent(e);
            return;
        }
        break;
    }
    }

    if (alt && !out.isEmpty())
        out.prepend('\x1b');
    clearSelection();
    m_stickToBottom = true;
    updateScrollBar();
    writeToPty(out);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty())
        writeToPty(e->commitString().toUtf8());
    e->accept();
}

// ── Mouse / selection ─────────────────────────────────────────────────────────

QPoint TerminalWidget::cellAt(const QPoint &pos) const
{
    const int col = qBound(0, pos.x() / m_cellW, m_cols - 1);
    const int row = qBound(0, pos.y() / m_cellH, m_rows - 1);
    return QPoint(col, int(verticalScrollBar()->value() + row + m_screen.droppedLines()));
}

void TerminalWidget::clearSelection()
{
    if (m_hasSelection) {
        m_hasSelection = false;
        viewport()->update();
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_selecting = true;
        m_hasSelection = false;
        m_selAnchor = m_selHead = cellAt(e->pos());
        viewport()->update();
    } else if (e->button() == Qt::MiddleButton && QApplication::clipboard()->supportsSelection()) {
        writeToPty(QApplication::clipboard()->text(QClipboard::Selection).toUtf8());
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_selecting) return;
    m_selHead = cellAt(e->pos());
    m_hasSelection = m_selHead != m_selAnchor;
    viewport()->update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    m_selecting = false;
    if (m_hasSelection && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection) return QString();
    QPoint a = m_selAnchor, b = m_selHead;
    if (b.y() < a.y() || (b.y() == a.y() && b.x() < a.x()))
        std::swap(a, b);

    const qint64 dropped = m_screen.droppedLines();
    QStringList lines;
    for (qint64 abs = a.y(); abs <= b.y(); ++abs) {
        const qint64 idx = abs - dropped;
        if (idx < 0 || idx >= m_screen.lineCount()) continue;
        const VtScreen::Line &l = m_screen.line(int(idx));
        const int from = abs == a.y() ? a.x() : 0;
        const int to = abs == b.y() ? b.x() : int(l.size()) - 1;
        QString s;
        for (int c = from; c <= to && c < l.size(); ++c)
            s += QString::fromUcs4(&l.at(c).ch, 1);
        while (s.endsWith(QLatin1Char(' ')))
            s.chop(1);
        lines << s;
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const int dy = e->angleDelta().y();
        if (dy != 0) emit fontSizeAdjustRequested(dy > 0 ? +1 : -1);
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
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

// R's readline treats a raw Tab as "complete", which swallows the indentation
// of submitted code. Prefix tabs with quoted-insert (Ctrl+V) so they reach R
// as literal tabs. The Windows console host has no readline, so it is skipped.
static QString preserveTabs(const QString &code)
{
#ifdef Q_OS_WIN
    return code;
#else
    QString out = code;
    out.replace(QLatin1Char('\t'), QStringLiteral("\x16\t"));
    return out;
#endif
}

void TerminalWidget::executeCommand(const QString &command)
{
    writeToPty((preserveTabs(command) + "\r").toUtf8());
}

void TerminalWidget::executeCommandSilent(const QString &command)
{
    if (command.isEmpty())
        return;
    QString pid     = QString::number(QCoreApplication::applicationPid());
    QString tmpPath = QDir::tempPath() + "/simpler_cmd_" + pid + ".R";
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        writeToPtySilent((command + "\r").toUtf8());
        return;
    }
    const QByteArray marker = QByteArray("__RGGUI_INTERNAL_DONE_")
        + QByteArray::number(QCoreApplication::applicationPid()) + "__";
    QTextStream out(&f);
    out << command << "\n"
        << "cat('" << QString::fromLatin1(marker) << "\\n')\n";
    f.close();
    tmpPath.replace('\\', '/');
    internalOutputMarker = marker;
    internalOutputBuffer.clear();
    suppressInternalOutput = true;
    QTimer::singleShot(2000, this, [this]() {
        suppressInternalOutput = false;
        suppressInternalPrompt = false;
        internalOutputBuffer.clear();
    });
    writeToPtySilent(QString("source('%1', echo=FALSE, print.eval=FALSE, local=FALSE)\r")
                   .arg(tmpPath).toUtf8());
}

void TerminalWidget::executeRCode(const QString &code)
{
    writeToPty((preserveTabs(code) + "\r").toUtf8());
}

// ── Context menu ──────────────────────────────────────────────────────────────

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);

    QAction *copy = menu->addAction(tr("Copy"));
    copy->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(copy, &QAction::triggered, this,
            [this]() { QApplication::clipboard()->setText(selectedText()); });
    copy->setEnabled(m_hasSelection);

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
