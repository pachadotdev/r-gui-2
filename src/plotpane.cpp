#include "plotpane.h"

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QResizeEvent>
#include <QSizePolicy>

PlotPane::PlotPane(const QString &plotDir, QWidget *parent)
    : QWidget(parent)
    , m_plotDir(plotDir)
    , m_currentFile(QDir(plotDir).filePath("rgui2_current.png"))
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

    // ── File system watcher ──────────────────────────────────────────────────
    // Watch a single fixed-name file (rgui2_current.png). It doesn't exist
    // until the first plot is drawn, so also watch the containing directory
    // to catch its creation, then switch to watching the file itself.
    QDir().mkpath(plotDir);

    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(plotDir);
    if (QFile::exists(m_currentFile))
        m_watcher->addPath(m_currentFile);

    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);

    connect(m_watcher,     &QFileSystemWatcher::fileChanged,
            this, &PlotPane::onFileChanged);
    connect(m_watcher,     &QFileSystemWatcher::directoryChanged,
            this, &PlotPane::onFileChanged);
    connect(m_reloadTimer, &QTimer::timeout, this, &PlotPane::refresh);

    connect(m_zoomInBtn,  &QPushButton::clicked, this, &PlotPane::zoomIn);
    connect(m_zoomOutBtn, &QPushButton::clicked, this, &PlotPane::zoomOut);
    connect(m_zoomFitBtn, &QPushButton::clicked, this, &PlotPane::zoomFit);
}

PlotPane::~PlotPane() = default;

// ── Private helpers ──────────────────────────────────────────────────────────

void PlotPane::loadPlot(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    QPixmap px(filePath);
    if (px.isNull()) return;

    m_currentPixmap = px;
    // Once a real plot is showing, let the label keep its own explicit
    // (possibly zoomed) size instead of being force-fit to the viewport, so
    // scrollbars appear correctly when zoomed in.
    m_scrollArea->setWidgetResizable(false);

    if (!m_userZoomed)
        zoomFit();   // auto-fit new plots unless the user has manually zoomed
    else
        applyZoom();

    emit plotUpdated();
}

void PlotPane::applyZoom()
{
    if (m_currentPixmap.isNull()) return;

    QSize scaledSize = m_currentPixmap.size() * m_zoomFactor;
    m_imageLabel->setPixmap(
        m_currentPixmap.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_imageLabel->resize(scaledSize);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void PlotPane::onFileChanged(const QString & /*path*/)
{
    // grDevices::png() re-opening the file can drop it from the watch list
    // (some platforms report the change as a remove+create); re-add it.
    if (QFile::exists(m_currentFile) && !m_watcher->files().contains(m_currentFile))
        m_watcher->addPath(m_currentFile);
    m_reloadTimer->start();
}

void PlotPane::refresh()
{
    if (!QFile::exists(m_currentFile))
        return;

    loadPlot(m_currentFile);
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

    QSize available = m_scrollArea->viewport()->size();
    QSize img       = m_currentPixmap.size();

    double scaleW = static_cast<double>(available.width())  / img.width();
    double scaleH = static_cast<double>(available.height()) / img.height();
    m_zoomFactor  = qMin(scaleW, scaleH);
    applyZoom();
}

void PlotPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_userZoomed)
        zoomFit();
}
