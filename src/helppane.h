#ifndef HELPPANE_H
#define HELPPANE_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWebEngineView>
#include <QFileSystemWatcher>
#include <QTimer>

class TerminalWidget;
struct EditorTheme;

class HelpPane : public QWidget
{
    Q_OBJECT

public:
    explicit HelpPane(TerminalWidget *terminal, QWidget *parent = nullptr);

    // Trigger a help lookup from outside (e.g. right-click → Help on selected word)
    void lookupTopic(const QString &topic);

    // Apply the IDE theme to the rendered R help pages.
    void setTheme(const EditorTheme &theme);

public slots:
    void startHelpServer();

private slots:
    void onSearchRequested();
    void onPortFileChanged(const QString &path);
    void onLoadFinished(bool ok);

private:
    void navigateTo(const QString &path);
    void applyPort(int port);
    void resolveHelpTopic(const QString &topic);
    void injectThemeCss();

    TerminalWidget   *m_terminal;
    QLineEdit        *m_searchBar;
    QPushButton      *m_searchBtn;
    QPushButton      *m_homeBtn;
    QWebEngineView   *m_webView;
    QFileSystemWatcher *m_watcher;
    QTimer           *m_pollTimer;

    QString m_portFilePath;
    QString m_helpUrlFilePath;
    QString m_helpQueueFilePath;
    int     m_helpPort = 0;
};

#endif // HELPPANE_H
