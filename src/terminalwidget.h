#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include "thememanager.h"
#include "vtscreen.h"
#include <QAbstractScrollArea>
#include <QFont>
#include <QStringDecoder>
#include <QTimer>

// Native terminal: PTY backend (forkpty / ConPTY) + VtScreen emulator,
// painted with QPainter. No embedded browser.
class TerminalWidget : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit TerminalWidget(const QString &shell = QString(), QWidget *parent = nullptr);
    ~TerminalWidget();

    void setTheme(const EditorTheme &theme);
    void setFontFamily(const QString &family);
    QString getShell() const { return shellPath; }
    void setArgs(const QStringList &args);
    void writeToShell(const QString &text);
    void executeCommand(const QString &command);
    void executeCommandSilent(const QString &command);
    void executeRCode(const QString &code);
    void setInitialFontSize(int pt);
    // Set the working directory for new PTY sessions (must be called before the PTY starts).
    void setWorkingDirectory(const QString &dir) { m_workingDir = dir; }

signals:
    void fontSizeAdjustRequested(int delta);  // Ctrl+wheel / Ctrl+/- inside the terminal

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    QString     shellPath;
    QString     m_workingDir;
    QStringList shellArgs;
    QStringList envList;          // POSIX child environment (unused on Windows)
    EditorTheme currentTheme;
    bool  ptyStarted = false;
    bool  suppressInternalOutput = false;
    bool  suppressInternalPrompt = false;
    QByteArray internalOutputBuffer;
    QByteArray internalOutputMarker;

    VtScreen       m_screen;
    QStringDecoder m_decoder{QStringConverter::Utf8};
    QFont m_font;
    int   m_cellW = 8, m_cellH = 16, m_ascent = 12;
    int   m_cols = 80, m_rows = 24;
    bool  m_stickToBottom = true;

    // Selection in absolute (dropped-lines-adjusted) line coordinates.
    bool    m_selecting = false;
    bool    m_hasSelection = false;
    QPoint  m_selAnchor, m_selHead;   // x = column, y = absolute line

    void startPty();
    void writeToPty(const QByteArray &data);
    void writeToPtySilent(const QByteArray &data);  // POSIX: written without local echo
    void sendOutput(const QByteArray &data);        // queued-connected to reader thread
    void doResize(int cols, int rows);
    void onPtyReaderFinished();                     // auto-restart R when the process exits

    void updateFontMetrics();
    void updateGrid();
    void updateScrollBar();
    QColor paletteColor(qint32 code, bool isFg) const;
    QPoint cellAt(const QPoint &pos) const;
    QString selectedText() const;
    void clearSelection();
    void showMessage(const QString &text, int ansiColor);

    class PtyReaderThread *ptyReader = nullptr;

#ifdef Q_OS_WIN
    struct ConPtyImpl;
    ConPtyImpl *pty = nullptr;
#else
    int   ptyFd    = -1;
    int   ptySlaveFd = -1;
    pid_t shellPid = -1;
#endif
};

#endif // TERMINALWIDGET_H
