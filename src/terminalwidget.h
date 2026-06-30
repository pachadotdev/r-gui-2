#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include "thememanager.h"
#include <QWebEngineView>
#include <QWebChannel>

// Forward declaration so TerminalBridge can hold a pointer before TerminalWidget is defined.
class TerminalWidget;

// JS ↔ C++ bridge exposed to xterm.js via QWebChannel.
// JS calls ready() / sendInput(); C++ emits outputReady / themeChanged.
class TerminalBridge : public QObject
{
    Q_OBJECT
public:
    explicit TerminalBridge(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void ready();                    // JS signals it is initialised
    Q_INVOKABLE void sendInput(const QString &data); // key/paste data from xterm.js
    Q_INVOKABLE void resizePty(int cols, int rows);  // JS reports actual terminal size
    Q_INVOKABLE void fontSizeUp();    // Ctrl++ inside xterm.js
    Q_INVOKABLE void fontSizeDown();  // Ctrl+- inside xterm.js
    Q_INVOKABLE void fontSizeReset(); // Ctrl+0 inside xterm.js

signals:
    void outputReady(const QString &b64data);    // base64-encoded PTY output
    void themeChanged(const QString &bg, const QString &fg, const QString &cursor);

private:
    friend class TerminalWidget;
    TerminalWidget *tw = nullptr;
};

class TerminalWidget : public QWebEngineView
{
    Q_OBJECT

public:
    explicit TerminalWidget(const QString &shell = QString(), QWidget *parent = nullptr);
    ~TerminalWidget();

    void setTheme(const EditorTheme &theme);
    QString getShell() const { return shellPath; }
    void setArgs(const QStringList &args);
    void writeToShell(const QString &text);
    void executeCommand(const QString &command);
    void executeCommandSilent(const QString &command);
    void executeRCode(const QString &code);
    // Apply a font size as soon as the page is ready (deferred if still loading).
    void setInitialFontSize(int pt);
    // Set the working directory for new PTY sessions (must be called before the page loads).
    void setWorkingDirectory(const QString &dir) { m_workingDir = dir; }

signals:
    void fontSizeAdjustRequested(int delta);  // emitted when Ctrl+/- pressed inside terminal

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    friend class TerminalBridge;

    QString     shellPath;
    QString     m_workingDir;     // initial working directory for the PTY child
    QStringList shellArgs;
    QStringList envList;          // POSIX child environment (unused on Windows)
    EditorTheme currentTheme;
    TerminalBridge *bridge  = nullptr;
    QWebChannel    *channel = nullptr;
    bool ptyStarted = false;
    bool pageLoaded = false;
    int  pendingFontSize = 0;   // 0 = none pending

    void startPty();
    void writeToPty(const QByteArray &data);
    void sendOutput(const QByteArray &data);   // queued-connected to reader thread
    void doResize(int cols, int rows);
    void onPtyReaderFinished();                // auto-restart R when the process exits

    class PtyReaderThread *ptyReader = nullptr;  // tracked on all platforms

#ifdef Q_OS_WIN
    struct ConPtyImpl;
    ConPtyImpl *pty = nullptr;
#else
    int   ptyFd    = -1;
    pid_t shellPid = -1;
#endif
};

#endif // TERMINALWIDGET_H
