#include "helppane.h"
#include "terminalwidget.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDebug>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QLabel>
#include <QProgressBar>

HelpPane::HelpPane(TerminalWidget *terminal, QWidget *parent)
    : QWidget(parent)
    , m_terminal(terminal)
    , m_helpPort(0)
{
    // ── Temp file that R writes the help-server port into ─────────────────
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_portFilePath  = QDir(tempDir).filePath("q_help_port");

    // ── Layout ────────────────────────────────────────────────────────────
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Search row
    QHBoxLayout *searchRow = new QHBoxLayout();
    searchRow->setSpacing(4);

    m_homeBtn = new QPushButton(tr("Home"), this);
    m_homeBtn->setFixedWidth(50);
    m_homeBtn->setToolTip(tr("Go to R help home"));
    searchRow->addWidget(m_homeBtn);

    m_searchBar = new QLineEdit(this);
    m_searchBar->setPlaceholderText(tr("Search R help (e.g. mean, dplyr::filter)…"));
    m_searchBar->setClearButtonEnabled(true);
    searchRow->addWidget(m_searchBar, 1);

    m_searchBtn = new QPushButton(tr("Search"), this);
    m_searchBtn->setFixedWidth(60);
    searchRow->addWidget(m_searchBtn);

    layout->addLayout(searchRow);

    // Web view
    m_webView = new QWebEngineView(this);
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_webView->setUrl(QUrl("about:blank"));
    layout->addWidget(m_webView, 1);

    // ── File watcher (notified when R writes the port file) ───────────────
    m_watcher = new QFileSystemWatcher(this);

    // Create the port file so the watcher can track it before R writes to it.
    {
        QFile f(m_portFilePath);
        if (!f.exists() && f.open(QIODevice::WriteOnly))
            f.close();
    }
    m_watcher->addPath(m_portFilePath);

    // Fallback poll every 2 s in case the watcher misses the write
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);

    // ── Connections ───────────────────────────────────────────────────────
    connect(m_searchBtn, &QPushButton::clicked, this, &HelpPane::onSearchRequested);
    connect(m_searchBar, &QLineEdit::returnPressed, this, &HelpPane::onSearchRequested);
    connect(m_homeBtn,   &QPushButton::clicked,     this, [this]() {
        navigateTo("/");
    });
    connect(m_watcher,   &QFileSystemWatcher::fileChanged, this, &HelpPane::onPortFileChanged);
    connect(m_pollTimer, &QTimer::timeout,                 this, [this]() {
        onPortFileChanged(m_portFilePath);
    });
    connect(m_webView,   &QWebEngineView::loadFinished, this, &HelpPane::onLoadFinished);

    // Start the R help server as soon as the widget is created.
    // We defer slightly so the R console is ready.
    QTimer::singleShot(2000, this, &HelpPane::startHelpServer);
}

// ── Public API ────────────────────────────────────────────────────────────────

void HelpPane::startHelpServer()
{
    if (!m_terminal) return;

    // Write the listening port to m_portFilePath.
    // tools::startDynamicHelp() returns the port (or 0 on failure).
    // We force-restart so calling this again after a crash still works.
    QString portFile = QString(m_portFilePath).replace('\\', '/');
    QString cmd = QString(
        "local({"
        "  p <- tools::startDynamicHelp(start = TRUE);"
        "  writeLines(as.character(p), '%1');"
        "})"
    ).arg(portFile);

    m_terminal->executeCommandSilent(cmd);

    // Start the fallback poll until the port is known.
    if (m_helpPort == 0)
        m_pollTimer->start();
}

void HelpPane::lookupTopic(const QString &topic)
{
    m_searchBar->setText(topic);
    onSearchRequested();
}

// ── Private slots ─────────────────────────────────────────────────────────────

void HelpPane::onSearchRequested()
{
    QString topic = m_searchBar->text().trimmed();
    if (topic.isEmpty()) {
        navigateTo("/");
        return;
    }

    if (m_helpPort == 0) {
        // Server not ready yet; queue the search after the server starts.
        startHelpServer();
        QTimer::singleShot(3000, this, &HelpPane::onSearchRequested);
        return;
    }

    // Support "pkg::fn" notation: jump directly to the function help page.
    if (topic.contains("::")) {
        QStringList parts = topic.split("::");
        if (parts.size() == 2) {
            QString pkg = parts[0].trimmed();
            QString fn  = parts[1].trimmed();
            navigateTo(QString("/library/%1/html/%2.html").arg(pkg, fn));
            return;
        }
    }

    // Otherwise use R's built-in search page.
    QString encoded = QUrl::toPercentEncoding(topic);
    navigateTo(QString("/doc/html/Search?q=%1&options=3").arg(QString(encoded)));
}

void HelpPane::onPortFileChanged(const QString &path)
{
    if (m_helpPort != 0) {
        m_pollTimer->stop();
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream in(&f);
    QString line = in.readLine().trimmed();
    f.close();

    bool ok = false;
    int port = line.toInt(&ok);
    if (ok && port > 0)
        applyPort(port);
}

void HelpPane::onLoadFinished(bool ok)
{
    Q_UNUSED(ok)
    // Keep the search bar in sync with the current URL so the user can see
    // what topic was resolved.
    QUrl url = m_webView->url();
    if (!url.isValid() || url.scheme() == "about")
        return;

    // If it's a search page, leave the search bar as-is.
    // If it's a direct topic page, show "pkg::fn" in the search bar.
    QString path = url.path();
    static QRegularExpression re(R"(/library/([^/]+)/html/([^/]+)\.html$)");
    QRegularExpressionMatch m = re.match(path);
    if (m.hasMatch())
        m_searchBar->setText(m.captured(1) + "::" + m.captured(2));
}

// ── Private helpers ───────────────────────────────────────────────────────────

void HelpPane::navigateTo(const QString &path)
{
    if (m_helpPort == 0) return;
    QUrl url(QString("http://127.0.0.1:%1%2").arg(m_helpPort).arg(path));
    m_webView->setUrl(url);
}

void HelpPane::applyPort(int port)
{
    m_helpPort = port;
    m_pollTimer->stop();
    qDebug() << "R help server running on port" << port;

    // Show the help home page.
    navigateTo("/");
}
