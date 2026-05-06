#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include <qtermwidget6/qtermwidget.h>
#include "thememanager.h"
#include <QMenu>
#include <QShortcut>
#include <QTimer>

class TerminalWidget : public QTermWidget
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

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QString shellPath;
    EditorTheme currentTheme;
    QShortcut *copyShortcut;
    QShortcut *pasteShortcut;
};

#endif // TERMINALWIDGET_H
