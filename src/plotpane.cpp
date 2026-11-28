#include "plotpane.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QStandardPaths>
#include <QFile>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QScrollBar>
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

    m_saveBtn = new QPushButton(tr("Save…"), this);
    m_saveBtn->setToolTip(tr("Save the current plot as an image"));

    for (QPushButton *b : {m_zoomOutBtn, m_zoomFitBtn, m_zoomInBtn})
        b->setFixedWidth(32);

    tbLayout->addWidget(m_saveBtn);
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

    connect(m_saveBtn, &QPushButton::clicked, this, &PlotPane::savePlot);
    connect(m_zoomInBtn,  &QPushButton::clicked, this, &PlotPane::zoomIn);
    connect(m_zoomOutBtn, &QPushButton::clicked, this, &PlotPane::zoomOut);
    connect(m_zoomFitBtn, &QPushButton::clicked, this, &PlotPane::zoomFit);

    // The first size is written before the pane has completed its layout.
    // Refresh once the viewport has its real dimensions so the initial plot
    // uses the available space rather than the construction-time size.
    QTimer::singleShot(250, this, [this]() {
        writePlotSize();
    });
    QTimer::singleShot(750, this, [this]() {
        writePlotSize();
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
    if (fields.size() != 3 || fields[0] != "RGPN") {
        m_pendingData.clear();
        return;
    }
    // The second header field is the PNG byte length; image dimensions are
    // encoded in the PNG itself.
    const qsizetype payloadSize = fields[1].toLongLong();
    if (payloadSize <= 0 || m_pendingData.size() < headerEnd + 1 + payloadSize)
        return;
    const QByteArray payload = m_pendingData.mid(headerEnd + 1, payloadSize);
    m_pendingData.remove(0, headerEnd + 1 + payloadSize);
    QPixmap px;
    if (!px.loadFromData(payload, "PNG"))
        return;

    m_currentPixmap = px;
    // Let the label keep its own (possibly zoomed) size so scrollbars work.
    m_scrollArea->setWidgetResizable(false);
    if (!m_userZoomed) {
        zoomFit();
        scheduleFit();
    } else {
        applyZoom();
    }
    scheduleInitialRefresh();
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

void PlotPane::scheduleInitialRefresh()
{
    if (m_initialRefreshScheduled)
        return;

    m_initialRefreshScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        writePlotSize();
        emit plotResizeRequested();
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

void PlotPane::savePlot()
{
    if (m_currentPixmap.isNull()) {
        QMessageBox::information(this, tr("Save Plot"), tr("There is no plot to save."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Plot"));
    QFormLayout *form = new QFormLayout(&dlg);

    QComboBox *format = new QComboBox(&dlg);
    format->addItem("PNG", "png");
    format->addItem("JPEG", "jpg");
    format->addItem("SVG", "svg");

    const QSize orig = m_scrollArea->viewport()->size().expandedTo(QSize(320, 240));
    QSpinBox *width = new QSpinBox(&dlg);
    QSpinBox *height = new QSpinBox(&dlg);
    width->setRange(16, 20000);
    height->setRange(16, 20000);
    width->setSuffix(" px");
    height->setSuffix(" px");
    width->setValue(orig.width());
    height->setValue(orig.height());

    // Keep the aspect ratio while editing either dimension.
    const double ratio = orig.height() > 0 ? double(orig.width()) / orig.height() : 1.0;
    QCheckBox *keepRatio = new QCheckBox(tr("Maintain aspect ratio"), &dlg);
    connect(width, &QSpinBox::valueChanged, &dlg, [=](int w) {
        if (!keepRatio->isChecked()) return;
        QSignalBlocker b(height);
        height->setValue(qRound(w / ratio));
    });
    connect(height, &QSpinBox::valueChanged, &dlg, [=](int h) {
        if (!keepRatio->isChecked()) return;
        QSignalBlocker b(width);
        width->setValue(qRound(h * ratio));
    });

    QLineEdit *path = new QLineEdit(
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath("plot.png"), &dlg);
    QPushButton *browse = new QPushButton(tr("Browse…"), &dlg);
    QWidget *pathRow = new QWidget(&dlg);
    QHBoxLayout *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(path);
    pathLayout->addWidget(browse);

    auto syncExtension = [=]() {
        QFileInfo fi(path->text());
        if (fi.fileName().isEmpty()) return;
        path->setText(fi.path() + "/" + fi.completeBaseName() + "."
                      + format->currentData().toString());
    };
    connect(format, &QComboBox::currentIndexChanged, &dlg, syncExtension);
    connect(browse, &QPushButton::clicked, &dlg, [=]() {
        const QString f = QFileDialog::getSaveFileName(
            pathRow, tr("Save Plot"), path->text(),
            tr("PNG (*.png);;JPEG (*.jpg *.jpeg);;SVG (*.svg)"));
        if (f.isEmpty()) return;
        path->setText(f);
        const QString ext = QFileInfo(f).suffix().toLower();
        int idx = ext == "svg" ? 2 : (ext == "jpg" || ext == "jpeg") ? 1 : 0;
        format->setCurrentIndex(idx);
        path->setText(f);
    });

    form->addRow(tr("Format:"), format);
    form->addRow(tr("Save as:"), pathRow);
    form->addRow(tr("Width:"), width);
    form->addRow(tr("Height:"), height);
    form->addRow(keepRatio);
    QDialogButtonBox *box = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted || path->text().isEmpty())
        return;

    requestSave(format->currentData().toString(), path->text(), width->value(), height->value());
}

void PlotPane::requestSave(const QString &fmt, const QString &file, int w, int h)
{
    if (m_plotSizeFile.isEmpty()) {
        QMessageBox::warning(this, tr("Save Plot"), tr("R plot channel is unavailable."));
        return;
    }
    // R re-draws the plot natively at the requested size
    // (png()/jpeg()/svg() + replayPlot + dev.off()) and reports the result.
    const QString resultPath = m_plotSizeFile + ".saveresult";
    QFile::remove(resultPath);
    QFile req(m_plotSizeFile + ".save");
    if (!req.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save Plot"), tr("Could not send the request to R."));
        return;
    }
    req.write(QStringLiteral("%1|%2|%3|%4\n").arg(fmt).arg(w).arg(h).arg(file).toUtf8());
    req.close();

    QTimer *timer = new QTimer(this);
    auto *elapsed = new int(0);
    connect(timer, &QTimer::timeout, this, [=]() {
        QFile res(resultPath);
        QString msg;
        if (res.exists() && res.open(QIODevice::ReadOnly | QIODevice::Text)) {
            msg = QString::fromUtf8(res.readAll()).trimmed();
            res.close();
            QFile::remove(resultPath);
        } else if ((*elapsed += 100) >= 15000) {
            msg = tr("error: R did not respond (is it busy?)");
            QFile::remove(m_plotSizeFile + ".save");
        } else {
            return;
        }
        timer->stop();
        timer->deleteLater();
        delete elapsed;
        if (msg != "ok")
            QMessageBox::warning(this, tr("Save Plot"),
                tr("Could not save the plot to %1.\n%2").arg(file, msg));
    });
    timer->start(100);
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
