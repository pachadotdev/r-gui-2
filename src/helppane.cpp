#include "helppane.h"
#include "terminalwidget.h"
#include "thememanager.h"

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
    // ── Temp files R reads/writes ─────────────────────────────────────────
    // These paths must match RGUI2_HELP_PORT_FILE / RGUI2_HELP_QUEUE_FILE /
    // RGUI2_HELP_URL_FILE in terminalwidget.cpp's setupREnv().
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_portFilePath      = QDir(tempDir).filePath("rgui2_help_port");
    m_helpUrlFilePath   = QDir(tempDir).filePath("rgui2_help_url");
    m_helpQueueFilePath = QDir(tempDir).filePath("rgui2_help_queue");

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

    // ── File watcher (notified when R writes the port file or a help URL) ─
    m_watcher = new QFileSystemWatcher(this);

    // Create both watched files so the watcher can track them before R writes.
    for (const QString &fp : {m_portFilePath, m_helpUrlFilePath}) {
        QFile f(fp);
        if (!f.exists() && f.open(QIODevice::WriteOnly))
            f.close();
        m_watcher->addPath(fp);
    }

    // Fallback poll every 2 s in case the watcher misses the write
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);

    // ── Connections ───────────────────────────────────────────────────────
    connect(m_searchBtn, &QPushButton::clicked, this, &HelpPane::onSearchRequested);
    connect(m_searchBar, &QLineEdit::returnPressed, this, &HelpPane::onSearchRequested);
    connect(m_homeBtn,   &QPushButton::clicked,     this, [this]() {
        navigateTo("/doc/html/index.html");
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
    // rgui2::init_help_pane() (called from the R startup script) starts the
    // dynamic help server and writes the port to m_portFilePath as soon as
    // R has finished loading. We just poll the port file until it appears.
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
        navigateTo("/doc/html/index.html");
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

    // Let R resolve the topic to a URL and write it to m_helpUrlFilePath.
    resolveHelpTopic(topic);
}

void HelpPane::onPortFileChanged(const QString &path)
{
    // ── Help-URL file: R resolved a topic → navigate there ───────────────
    if (path == m_helpUrlFilePath) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream in(&f);
        QString url = in.readLine().trimmed();
        f.close();
        if (!url.isEmpty())
            m_webView->setUrl(QUrl(url));
        return;
    }

    // ── Port file: R wrote the help-server port ───────────────────────────
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

    // Restyle the loaded R help page to match the current IDE theme.
    injectThemeCss();

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

// ── Theme ─────────────────────────────────────────────────────────────────────

void HelpPane::setTheme(const EditorTheme & /*theme*/)
{
    // Re-inject CSS using the current ThemeManager state. Cheap and avoids
    // having to cache the theme inside HelpPane.
    injectThemeCss();
}

void HelpPane::injectThemeCss()
{
    if (!m_webView || !m_webView->page()) return;

    const EditorTheme theme = ThemeManager::instance().currentTheme();
    const QColor bg   = theme.background;
    const QColor fg   = theme.foreground;
    const QColor codeBg = theme.lineNumberBg.isValid() ? theme.lineNumberBg
                                                       : bg.lighter(115);
    const QColor border = fg.darker(150);

    // Pick a link colour with guaranteed contrast against bg.
    // Start from theme.function (or theme.keyword), then lighten/darken until
    // the relative-luminance contrast is ≥ 4.5:1 (WCAG AA).
    auto toLinear = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto luminance = [&](const QColor &c) {
        return 0.2126 * toLinear(c.redF())
             + 0.7152 * toLinear(c.greenF())
             + 0.0722 * toLinear(c.blueF());
    };
    auto contrast = [&](const QColor &a, const QColor &b) {
        qreal la = luminance(a), lb = luminance(b);
        return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
    };

    QColor link = theme.function.isValid() ? theme.function : theme.keyword;
    // Nudge the link colour until we reach ≥ 4.5:1 contrast with bg.
    for (int i = 0; i < 20 && contrast(link, bg) < 4.5; ++i) {
        // If bg is dark, lighten; if light, darken.
        link = luminance(bg) < 0.18 ? link.lighter(115) : link.darker(115);
    }

    auto rgba = [](const QColor &c, qreal a = 1.0) {
        return QString("rgba(%1,%2,%3,%4)")
            .arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
    };

    const QString css = QString(R"CSS(
        html, body { background: %1 !important; color: %2 !important; }
        body { font-family: sans-serif; }
        a, a:link, a:visited {
            color: %3 !important;
            background: %1 !important;
            text-decoration: none;
        }
        a:hover { text-decoration: underline; }
        h1, h2, h3, h4, h5, h6, table, tr, td, th, dt, dd, p, li, span, div {
            color: %2 !important;
            background: transparent !important;
            border-color: %4 !important;
        }
        pre, code, kbd, samp, tt {
            background: %5 !important;
            color: %2 !important;
            border: 1px solid %4 !important;
            padding: 2px 4px;
            border-radius: 3px;
        }
        pre { padding: 8px; overflow: auto; }
        table { border-collapse: collapse; }
        table, th, td { border: 1px solid %4 !important; }
        hr { border-color: %4 !important; }
    )CSS").arg(rgba(bg), rgba(fg), rgba(link), rgba(border), rgba(codeBg));

    // Strip newlines so we can embed the CSS literal in a JS string safely.
    QString cssOneLine = css;
    cssOneLine.replace('\n', ' ').replace('\r', ' ').replace('\\', "\\\\").replace('\'', "\\'");

    const QString js = QString(
        "(function(){"
        "  var id = 'q-ide-theme-style';"
        "  var s = document.getElementById(id);"
        "  if (!s) { s = document.createElement('style'); s.id = id;"
        "            document.head && document.head.appendChild(s); }"
        "  s.textContent = '%1';"
        "})();"
    ).arg(cssOneLine);

    m_webView->page()->runJavaScript(js);
}

// ── Private helpers ───────────────────────────────────────────────────────────
void HelpPane::resolveHelpTopic(const QString &topic)
{
    // Silent channel: write the topic to the queue file. The rgui2 R package
    // (init_help_pane) polls this file via later::later() and writes the
    // resolved URL to m_helpUrlFilePath without ever touching stdin/stdout.
    QFile f(m_helpQueueFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "helppane: cannot write queue file" << m_helpQueueFilePath;
        return;
    }
    QTextStream out(&f);
    out << topic << "\n";
    f.close();
}
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
    navigateTo("/doc/html/index.html");
}
