#include "helppane.h"
#include "terminalwidget.h"
#include "thememanager.h"

#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextStream>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDebug>
#include <cmath>

// ── HelpBrowser ───────────────────────────────────────────────────────────────

QVariant HelpBrowser::loadResource(int type, const QUrl &url)
{
    if (type == QTextDocument::ImageResource &&
        (url.scheme() == "http" || url.scheme() == "https")) {
        // Never spin a nested event loop here: this runs during layout/paint.
        const auto it = m_images.constFind(url);
        if (it != m_images.constEnd())
            return it.value();
        if (!m_pending.contains(url)) {
            m_pending.insert(url);
            QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
            connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
                reply->deleteLater();
                m_pending.remove(url);
                QImage img;
                if (reply->error() == QNetworkReply::NoError && img.loadFromData(reply->readAll())) {
                    m_images.insert(url, img);
                    QTimer::singleShot(0, this, [this]() {
                        document()->markContentsDirty(0, document()->characterCount());
                        viewport()->update();
                    });
                }
            });
        }
        return QVariant();
    }
    return QTextBrowser::loadResource(type, url);
}

// ── HelpPane ──────────────────────────────────────────────────────────────────

HelpPane::HelpPane(TerminalWidget *terminal, QWidget *parent)
    : QWidget(parent)
    , m_terminal(terminal)
    , m_helpPort(0)
{
    // These paths must match RGUI2_HELP_PORT_FILE / RGUI2_HELP_QUEUE_FILE /
    // RGUI2_HELP_URL_FILE in terminalwidget.cpp's setupREnv().
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_portFilePath      = QDir(tempDir).filePath("rgui2_help_port");
    m_helpUrlFilePath   = QDir(tempDir).filePath("rgui2_help_url");
    m_helpQueueFilePath = QDir(tempDir).filePath("rgui2_help_queue");

    m_nam = new QNetworkAccessManager(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout *searchRow = new QHBoxLayout();
    searchRow->setSpacing(4);

    m_backBtn = new QPushButton(tr("Back"), this);
    m_backBtn->setFixedWidth(50);
    m_backBtn->setEnabled(false);
    searchRow->addWidget(m_backBtn);

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

    m_view = new HelpBrowser(m_nam, this);
    m_view->setOpenLinks(false);
    layout->addWidget(m_view, 1);
    applyThemeStyle();

    // File watcher: notified when R writes the port file or a help URL.
    m_watcher = new QFileSystemWatcher(this);
    for (const QString &fp : {m_portFilePath, m_helpUrlFilePath}) {
        QFile f(fp);
        if (!f.exists() && f.open(QIODevice::WriteOnly))
            f.close();
        m_watcher->addPath(fp);
    }

    // Fallback poll every 2 s in case the watcher misses the write.
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);

    connect(m_searchBtn, &QPushButton::clicked, this, &HelpPane::onSearchRequested);
    connect(m_searchBar, &QLineEdit::returnPressed, this, &HelpPane::onSearchRequested);
    connect(m_homeBtn,   &QPushButton::clicked, this, [this]() {
        navigateTo("/doc/html/index.html");
    });
    connect(m_backBtn,   &QPushButton::clicked, this, &HelpPane::goBack);
    connect(m_watcher,   &QFileSystemWatcher::fileChanged, this, &HelpPane::onPortFileChanged);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        onPortFileChanged(m_portFilePath);
    });
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &link) {
        const QUrl target = m_currentUrl.isValid() ? m_currentUrl.resolved(link) : link;
        if (target.scheme() == "http" && target.host() == "127.0.0.1") {
            // Same-page anchors just scroll.
            if (!target.fragment().isEmpty() &&
                target.adjusted(QUrl::RemoveFragment) == m_currentUrl.adjusted(QUrl::RemoveFragment)) {
                m_view->scrollToAnchor(target.fragment());
                return;
            }
            loadUrl(target);
        } else {
            QDesktopServices::openUrl(target);
        }
    });

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
        startHelpServer();
        QTimer::singleShot(3000, this, &HelpPane::onSearchRequested);
        return;
    }

    // "pkg::fn" jumps straight to the function help page.
    if (topic.contains("::")) {
        QStringList parts = topic.split("::");
        if (parts.size() == 2) {
            navigateTo(QString("/library/%1/html/%2.html")
                           .arg(parts[0].trimmed(), parts[1].trimmed()));
            return;
        }
    }

    // Let R resolve the topic to a URL and write it to m_helpUrlFilePath.
    resolveHelpTopic(topic);
}

void HelpPane::onPortFileChanged(const QString &path)
{
    // R resolved a topic → navigate there.
    if (path == m_helpUrlFilePath) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream in(&f);
        QString url = in.readLine().trimmed();
        f.close();
        if (!url.isEmpty())
            loadUrl(QUrl(url));
        return;
    }

    // R wrote the help-server port.
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

// ── Loading / rendering ───────────────────────────────────────────────────────

void HelpPane::loadUrl(const QUrl &url, bool addToHistory)
{
    QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, addToHistory]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            showHtml(QString("<p>%1</p><p><code>%2</code></p>")
                         .arg(tr("Could not load help page."), reply->errorString().toHtmlEscaped()),
                     url);
            return;
        }
        if (addToHistory && m_currentUrl.isValid())
            m_history.append(m_currentUrl);
        m_backBtn->setEnabled(!m_history.isEmpty());
        showHtml(QString::fromUtf8(reply->readAll()), url);

        // Keep the search bar in sync with the resolved topic.
        static const QRegularExpression re(R"(/library/([^/]+)/html/([^/]+)\.html$)");
        const QRegularExpressionMatch m = re.match(url.path());
        if (m.hasMatch())
            m_searchBar->setText(m.captured(1) + "::" + m.captured(2));
    });
}

void HelpPane::showHtml(const QString &html, const QUrl &url)
{
    m_currentUrl = url;
    m_currentHtml = html;
    m_view->document()->setBaseUrl(url);
    applyThemeStyle();
    m_view->setHtml(html);
    if (!url.fragment().isEmpty())
        m_view->scrollToAnchor(url.fragment());
}

void HelpPane::goBack()
{
    if (m_history.isEmpty()) return;
    const QUrl prev = m_history.takeLast();
    m_backBtn->setEnabled(!m_history.isEmpty());
    loadUrl(prev, false);
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void HelpPane::setTheme(const EditorTheme & /*theme*/)
{
    applyThemeStyle();
    if (!m_currentHtml.isEmpty())
        m_view->setHtml(m_currentHtml);
}

void HelpPane::applyThemeStyle()
{
    const EditorTheme theme = ThemeManager::instance().currentTheme();
    const QColor bg = theme.background;
    const QColor fg = theme.foreground;
    const QColor codeBg = theme.lineNumberBg.isValid() ? theme.lineNumberBg : bg.lighter(115);
    const QColor border = fg.darker(150);

    // Link colour with WCAG AA contrast (≥ 4.5:1) against the background.
    auto toLinear = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto luminance = [&](const QColor &c) {
        return 0.2126 * toLinear(c.redF()) + 0.7152 * toLinear(c.greenF()) + 0.0722 * toLinear(c.blueF());
    };
    auto contrast = [&](const QColor &a, const QColor &b) {
        qreal la = luminance(a), lb = luminance(b);
        return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
    };
    QColor link = theme.function.isValid() ? theme.function : theme.keyword;
    for (int i = 0; i < 20 && contrast(link, bg) < 4.5; ++i)
        link = luminance(bg) < 0.18 ? link.lighter(115) : link.darker(115);

    m_view->setStyleSheet(QString("QTextBrowser { background-color: %1; color: %2; border: none; }")
                              .arg(bg.name(), fg.name()));

    // QTextDocument understands a small CSS subset; this is applied at setHtml().
    m_view->document()->setDefaultStyleSheet(QString(
        "body { color: %1; background-color: %2; }"
        "a { color: %3; text-decoration: none; }"
        "h1, h2, h3, h4, h5, h6 { color: %1; }"
        "pre, code, tt { background-color: %4; color: %1; }"
        "table, th, td { border-color: %5; }"
    ).arg(fg.name(), bg.name(), link.name(), codeBg.name(), border.name()));
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
    loadUrl(QUrl(QString("http://127.0.0.1:%1%2").arg(m_helpPort).arg(path)));
}

void HelpPane::applyPort(int port)
{
    m_helpPort = port;
    m_pollTimer->stop();
    qDebug() << "R help server running on port" << port;
    navigateTo("/doc/html/index.html");
}
