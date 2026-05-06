#include "plotpane.h"

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSizePolicy>

PlotPane::PlotPane(const QString &plotDir, QWidget *parent)
    : QWidget(parent)
    , m_plotDir(plotDir)
    , m_indexFile(QDir(plotDir).filePath("q_plot_index.txt"))
    , m_currentIndex(-1)
    , m_zoomFactor(1.0)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────────
    QWidget *toolbar = new QWidget(this);
    QHBoxLayout *tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(4, 2, 4, 2);
    tbLayout->setSpacing(4);

    m_prevBtn    = new QPushButton(tr("◀"), this);
    m_nextBtn    = new QPushButton(tr("▶"), this);
    m_zoomOutBtn = new QPushButton(tr("−"), this);
    m_zoomFitBtn = new QPushButton(tr("Fit"), this);
    m_zoomInBtn  = new QPushButton(tr("+"), this);

    for (QPushButton *b : {m_prevBtn, m_nextBtn, m_zoomOutBtn, m_zoomFitBtn,
                           m_zoomInBtn}) {
        b->setFixedWidth(32);
    }

    m_plotList = new QComboBox(this);
    m_plotList->hide();

    m_statusLabel = new QLabel(tr("No plots yet"), this);
    m_statusLabel->hide();

    tbLayout->addWidget(m_prevBtn);
    tbLayout->addWidget(m_nextBtn);
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

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_imageLabel->setScaledContents(false);
    m_imageLabel->setText(tr("Plots will appear here when R code produces graphics."));
    m_imageLabel->setWordWrap(true);

    m_scrollArea->setWidget(m_imageLabel);
    m_scrollArea->setWidgetResizable(true);
    layout->addWidget(m_scrollArea);

    // ── File system watcher ──────────────────────────────────────────────────
    QDir().mkpath(plotDir);

    // Ensure the index file exists so the watcher can track it
    {
        QFile f(m_indexFile);
        if (!f.exists()) {
            if (f.open(QIODevice::WriteOnly))
                f.close();
        }
    }

    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(m_indexFile);
    // Also watch the directory for new files
    m_watcher->addPath(plotDir);

    // Debounce rapid file-change events with a short timer
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);

    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &PlotPane::onIndexFileChanged);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &PlotPane::onIndexFileChanged);
    connect(m_reloadTimer, &QTimer::timeout, this, &PlotPane::refresh);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(m_prevBtn,    &QPushButton::clicked, this, &PlotPane::showPrevious);
    connect(m_nextBtn,    &QPushButton::clicked, this, &PlotPane::showNext);
    connect(m_zoomInBtn,  &QPushButton::clicked, this, &PlotPane::zoomIn);
    connect(m_zoomOutBtn, &QPushButton::clicked, this, &PlotPane::zoomOut);
    connect(m_zoomFitBtn, &QPushButton::clicked, this, &PlotPane::zoomFit);
    connect(m_plotList, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PlotPane::onPlotListChanged);

    updateNavButtons();
}

PlotPane::~PlotPane() = default;

// ── Private helpers ──────────────────────────────────────────────────────────

void PlotPane::loadPlot(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    QPixmap px(filePath);
    if (px.isNull()) return;

    m_currentPixmap = px;
    applyZoom();

    QFileInfo fi(filePath);
    m_statusLabel->setText(fi.fileName());
}

void PlotPane::applyZoom()
{
    if (m_currentPixmap.isNull()) return;

    QSize scaledSize = m_currentPixmap.size() * m_zoomFactor;
    m_imageLabel->setPixmap(
        m_currentPixmap.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_imageLabel->resize(scaledSize);
}

void PlotPane::updateNavButtons()
{
    m_prevBtn->setEnabled(m_currentIndex > 0);
    m_nextBtn->setEnabled(m_currentIndex >= 0 && m_currentIndex < m_plotFiles.size() - 1);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void PlotPane::onIndexFileChanged(const QString & /*path*/)
{
    // Re-add path to watcher: some editors/writers replace the file, which
    // removes it from the watcher.
    if (!m_watcher->files().contains(m_indexFile))
        m_watcher->addPath(m_indexFile);

    // Debounce
    m_reloadTimer->start();
}

void PlotPane::refresh()
{
    // Read the latest plot path from the index file
    QFile f(m_indexFile);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream in(&f);
    QString latestPlot = in.readLine().trimmed();
    f.close();

    if (latestPlot.isEmpty() || !QFile::exists(latestPlot))
        return;

    // Add to history if new
    if (!m_plotFiles.contains(latestPlot)) {
        m_plotFiles.append(latestPlot);

        // Keep combo box in sync (block signal to avoid re-entrancy)
        {
            QSignalBlocker blocker(m_plotList);
            m_plotList->addItem(QFileInfo(latestPlot).fileName());
        }
    }

    // Navigate to this plot
    int idx = m_plotFiles.indexOf(latestPlot);
    if (idx != m_currentIndex) {
        m_currentIndex = idx;
        {
            QSignalBlocker blocker(m_plotList);
            m_plotList->setCurrentIndex(idx);
        }
        loadPlot(latestPlot);
        updateNavButtons();
    } else {
        // Same file but may have been updated (re-rendered)
        loadPlot(latestPlot);
    }
}

void PlotPane::showPrevious()
{
    if (m_currentIndex <= 0) return;
    --m_currentIndex;
    QSignalBlocker blocker(m_plotList);
    m_plotList->setCurrentIndex(m_currentIndex);
    loadPlot(m_plotFiles.at(m_currentIndex));
    updateNavButtons();
}

void PlotPane::showNext()
{
    if (m_currentIndex >= m_plotFiles.size() - 1) return;
    ++m_currentIndex;
    QSignalBlocker blocker(m_plotList);
    m_plotList->setCurrentIndex(m_currentIndex);
    loadPlot(m_plotFiles.at(m_currentIndex));
    updateNavButtons();
}

void PlotPane::zoomIn()
{
    m_zoomFactor = qMin(m_zoomFactor * 1.25, 5.0);
    applyZoom();
}

void PlotPane::zoomOut()
{
    m_zoomFactor = qMax(m_zoomFactor / 1.25, 0.1);
    applyZoom();
}

void PlotPane::zoomFit()
{
    if (m_currentPixmap.isNull()) return;

    QSize available = m_scrollArea->viewport()->size();
    QSize img = m_currentPixmap.size();

    double scaleW = static_cast<double>(available.width())  / img.width();
    double scaleH = static_cast<double>(available.height()) / img.height();
    m_zoomFactor = qMin(scaleW, scaleH);
    applyZoom();
}

void PlotPane::onPlotListChanged(int index)
{
    if (index < 0 || index >= m_plotFiles.size()) return;
    m_currentIndex = index;
    loadPlot(m_plotFiles.at(index));
    updateNavButtons();
}
