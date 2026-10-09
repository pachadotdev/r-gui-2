#include "plotpane.h"

#include <QDir>
#include <QFile>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QScrollBar>
#include <QImage>
#include <QHostAddress>

PlotPane::PlotPane(const QString &plotChannelFile, const QString &plotSizeFile,
                   QWidget *parent)
    : QWidget(parent)
    , m_plotChannelFile(plotChannelFile)
    , m_plotSizeFile(plotSizeFile)
    , m_server(new QTcpServer(this))
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────────
    QWidget *toolbar = new QWidget(this);
    QHBoxLayout *tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(4, 2, 4, 2);
    tbLayout->setSpacing(4);

    m_zoomOutBtn = new QPushButton(tr("−"), this);
    m_zoomFitBtn = new QPushButton(tr("Fit"), this);
    m_zoomInBtn  = new QPushButton(tr("+"), this);

    for (QPushButton *b : {m_zoomOutBtn, m_zoomFitBtn, m_zoomInBtn})
        b->setFixedWidth(32);

    tbLayout->addStretch();
    tbLayout->addWidget(m_zoomOutBtn);
    tbLayout->addWidget(m_zoomFitBtn);
    tbLayout->addWidget(m_zoomInBtn);

    layout->addWidget(toolbar);

    // ── Image area ───────────────────────────────────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->setBackgroundRole(QPalette::Dark);
    m_scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_imageLabel->setScaledContents(false);
    m_imageLabel->setText(tr("Plots will appear here when R code produces graphics."));
    m_imageLabel->setWordWrap(true);

    m_scrollArea->setWidget(m_imageLabel);
    // Resizable while showing the placeholder text (so it fills/wraps nicely
    // across the viewport); switched to false once a real plot loads so the
    // label can be its own explicit zoomed size and the scroll area shows
    // scrollbars instead of always stretching the image to fit.
    m_scrollArea->setWidgetResizable(true);
    layout->addWidget(m_scrollArea);

    connect(m_server, &QTcpServer::newConnection,
            this, &PlotPane::onPlotConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        return;

    QFile channelFile(m_plotChannelFile);
    if (channelFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        channelFile.write(QByteArray::number(m_server->serverPort()));
        channelFile.close();
    }
    writePlotSize();
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(150);
    connect(m_resizeTimer, &QTimer::timeout,
            this, &PlotPane::plotResizeRequested);

    connect(m_zoomInBtn,  &QPushButton::clicked, this, &PlotPane::zoomIn);
    connect(m_zoomOutBtn, &QPushButton::clicked, this, &PlotPane::zoomOut);
    connect(m_zoomFitBtn, &QPushButton::clicked, this, &PlotPane::zoomFit);

    // The first size is written before the pane has completed its layout.
    // Refresh once the viewport has its real dimensions so the initial plot
    // uses the available space rather than the construction-time size.
    QTimer::singleShot(250, this, [this]() {
        writePlotSize();
        if (m_resizeTimer)
            m_resizeTimer->start();
    });
    QTimer::singleShot(750, this, [this]() {
        writePlotSize();
        if (m_resizeTimer)
            m_resizeTimer->start();
    });
}

PlotPane::~PlotPane() = default;

// ── Private helpers ──────────────────────────────────────────────────────────

void PlotPane::readPlotData()
{
    if (!m_socket)
        return;

    m_pendingData.append(m_socket->readAll());
    constexpr int headerSize = 32;
    const int headerEnd = m_pendingData.indexOf('\n');
    if (headerEnd < 0 || headerEnd > headerSize)
        return;

    const QList<QByteArray> fields = m_pendingData.left(headerEnd).split(' ');
    if (fields.size() != 3 || (fields[0] != "RGPL" && fields[0] != "RGPN")) {
        m_pendingData.clear();
        return;
    }
    const int width = fields[1].toInt();
    const int height = fields[2].toInt();
    if (fields[0] == "RGPN") {
        const qsizetype payloadSize = static_cast<qsizetype>(height);
        if (payloadSize <= 0 || m_pendingData.size() < headerEnd + 1 + payloadSize)
            return;
        const QByteArray payload = m_pendingData.mid(headerEnd + 1, payloadSize);
        m_pendingData.remove(0, headerEnd + 1 + payloadSize);
        QPixmap px;
        if (!px.loadFromData(payload, "PNG"))
            return;
        m_currentPixmap = px;
        m_scrollArea->setWidgetResizable(false);
        if (!m_userZoomed) {
            zoomFit();
            scheduleFit();
        } else {
            applyZoom();
        }
        emit plotUpdated();
        return;
    }
    const qsizetype payloadSize = static_cast<qsizetype>(width) * height * 3;
    if (width <= 0 || height <= 0 || payloadSize <= 0
        || m_pendingData.size() < headerEnd + 1 + payloadSize)
        return;

    const QByteArray payload = m_pendingData.mid(headerEnd + 1, payloadSize);
    m_pendingData.remove(0, headerEnd + 1 + payloadSize);
    // The native device sends tightly packed RGB rows.  Pass the row stride
    // explicitly: letting QImage infer it may add alignment padding, which
    // shears images whose width is not divisible by four.
    QImage image(reinterpret_cast<const uchar *>(payload.constData()), width, height,
                 width * 3, QImage::Format_RGB888);
    QPixmap px = QPixmap::fromImage(image.copy());
    if (px.isNull())
        return;

    m_currentPixmap = px;
    // Once a real plot is showing, let the label keep its own explicit
    // (possibly zoomed) size instead of being force-fit to the viewport, so
    // scrollbars appear correctly when zoomed in.
    m_scrollArea->setWidgetResizable(false);

    if (!m_userZoomed)
    {
        zoomFit();   // auto-fit new plots unless the user has manually zoomed
        scheduleFit();
    }
    else
        applyZoom();

    emit plotUpdated();
}

void PlotPane::writePlotSize()
{
    if (m_plotSizeFile.isEmpty())
        return;

    const QSize viewportSize = m_scrollArea->viewport()->size();
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0)
        return;
    const QSize size(qMax(320, viewportSize.width()), qMax(240, viewportSize.height()));

    QFile sizeFile(m_plotSizeFile);
    if (sizeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sizeFile.write(QByteArray::number(size.width()));
        sizeFile.write(" ");
        sizeFile.write(QByteArray::number(size.height()));
        sizeFile.write("\n");
    }
}

void PlotPane::applyZoom()
{
    if (m_currentPixmap.isNull()) return;

    QSize scaledSize = m_currentPixmap.size() * m_zoomFactor;
    m_imageLabel->setPixmap(scaledSize == m_currentPixmap.size()
        ? m_currentPixmap
        : m_currentPixmap.scaled(scaledSize, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation));
    m_imageLabel->resize(scaledSize);
}

void PlotPane::scheduleFit()
{
    if (m_userZoomed || m_fitPending)
        return;

    m_fitPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_fitPending = false;
        if (!m_userZoomed)
            zoomFit();
    });
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void PlotPane::onPlotConnection()
{
    if (m_socket)
        m_socket->deleteLater();
    m_socket = m_server->nextPendingConnection();
    m_pendingData.clear();
    connect(m_socket, &QTcpSocket::readyRead, this, &PlotPane::readPlotData);
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        m_socket->deleteLater();
        m_socket = nullptr;
    });
}

void PlotPane::refresh()
{
    // Plot updates arrive through the local channel.
}

void PlotPane::zoomIn()
{
    m_userZoomed = true;
    m_zoomFactor = qMin(m_zoomFactor * 1.25, 5.0);
    applyZoom();
}

void PlotPane::zoomOut()
{
    m_userZoomed = true;
    m_zoomFactor = qMax(m_zoomFactor / 1.25, 0.1);
    applyZoom();
}

void PlotPane::zoomFit()
{
    if (m_currentPixmap.isNull()) return;

    // Fit to the pane's available width (not height), so a freshly-drawn
    // plot always starts as large as horizontal space allows; vertical
    // scrolling (already supported) takes over if that makes it taller than
    // the viewport.
    int available = m_scrollArea->viewport()->size().width();
    int imgWidth  = m_currentPixmap.size().width();
    if (imgWidth <= 0) return;

    // A tall image can make the vertical scrollbar appear after the first
    // fit, reducing the usable width. Account for that scrollbar up front;
    // scheduleFit() handles the final layout pass when Qt updates visibility.
    int viewportHeight = m_scrollArea->viewport()->size().height();
    if (viewportHeight > 0
        && !m_scrollArea->verticalScrollBar()->isVisible()
        && m_currentPixmap.height() * available / imgWidth > viewportHeight) {
        available -= m_scrollArea->verticalScrollBar()->sizeHint().width();
    }
    available = qMax(1, available);

    m_zoomFactor = static_cast<double>(available) / imgWidth;
    applyZoom();
}

void PlotPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    writePlotSize();
    if (m_resizeTimer)
        m_resizeTimer->start();
    if (!m_userZoomed)
        scheduleFit();
}
