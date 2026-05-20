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

class HelpPane : public QWidget
{
    Q_OBJECT

public:
    explicit HelpPane(TerminalWidget *terminal, QWidget *parent = nullptr);

    // Trigger a help lookup from outside (e.g. right-click → Help on selected word)
    void lookupTopic(const QString &topic);

public slots:
    void startHelpServer();

private slots:
    void onSearchRequested();
    void onPortFileChanged(const QString &path);
    void onLoadFinished(bool ok);

private:
    void navigateTo(const QString &path);
    void applyPort(int port);

    TerminalWidget   *m_terminal;
    QLineEdit        *m_searchBar;
    QPushButton      *m_searchBtn;
    QPushButton      *m_homeBtn;
    QWebEngineView   *m_webView;
    QFileSystemWatcher *m_watcher;
    QTimer           *m_pollTimer;

    QString m_portFilePath;
    int     m_helpPort = 0;
};

#endif // HELPPANE_H
