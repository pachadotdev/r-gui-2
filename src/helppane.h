#ifndef HELPPANE_H
#define HELPPANE_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QFileSystemWatcher>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <QHash>
#include <QSet>
#include <QImage>

class TerminalWidget;
struct EditorTheme;

// QTextBrowser that can pull inline images from the local R help server.
class HelpBrowser : public QTextBrowser
{
    Q_OBJECT
public:
    explicit HelpBrowser(QNetworkAccessManager *nam, QWidget *parent = nullptr)
        : QTextBrowser(parent), m_nam(nam) {}

protected:
    QVariant loadResource(int type, const QUrl &url) override;

private:
    QNetworkAccessManager *m_nam;
    QHash<QUrl, QImage> m_images;
    QSet<QUrl> m_pending;
};

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

private:
    void navigateTo(const QString &path);
    void loadUrl(const QUrl &url, bool addToHistory = true);
    void showHtml(const QString &html, const QUrl &url);
    void applyPort(int port);
    void resolveHelpTopic(const QString &topic);
    void applyThemeStyle();
    void goBack();

    TerminalWidget   *m_terminal;
    QLineEdit        *m_searchBar;
    QPushButton      *m_searchBtn;
    QPushButton      *m_homeBtn;
    QPushButton      *m_backBtn;
    HelpBrowser      *m_view;
    QNetworkAccessManager *m_nam;
    QFileSystemWatcher *m_watcher;
    QTimer           *m_pollTimer;

    QString m_portFilePath;
    QString m_helpUrlFilePath;
    QString m_helpQueueFilePath;
    int     m_helpPort = 0;

    QList<QUrl> m_history;
    QUrl    m_currentUrl;
    QString m_currentHtml;
};

#endif // HELPPANE_H
