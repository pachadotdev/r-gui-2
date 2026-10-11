#ifndef PLOTPANE_H
#define PLOTPANE_H

#include <QWidget>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QPixmap>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

class PlotPane : public QWidget
{
    Q_OBJECT

public:
    explicit PlotPane(const QString &plotChannelFile, const QString &plotSizeFile,
                      QWidget *parent = nullptr);
    ~PlotPane();

    QString plotChannelFile() const { return m_plotChannelFile; }
    bool hasPlot() const { return !m_currentPixmap.isNull(); }

public slots:
    void refresh();

signals:
    void plotUpdated();
    void plotResizeRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onPlotConnection();
    void zoomIn();
    void zoomOut();
    void zoomFit();

private:
    void readPlotData();
    void writePlotSize();
    void applyZoom();
    void scheduleFit();
    void scheduleInitialRefresh();

    QString m_plotChannelFile;
    QString m_plotSizeFile;
    QTcpServer *m_server;
    QTcpSocket *m_socket = nullptr;
    QByteArray m_pendingData;
    QTimer *m_resizeTimer = nullptr;

    QScrollArea  *m_scrollArea;
    QLabel       *m_imageLabel;
    QPushButton  *m_zoomInBtn;
    QPushButton  *m_zoomOutBtn;
    QPushButton  *m_zoomFitBtn;

    double  m_zoomFactor  = 1.0;
    bool    m_userZoomed  = false;   // true once user manually zoomed
    bool    m_fitPending  = false;
    bool    m_initialRefreshScheduled = false;
    QPixmap m_currentPixmap;
};

#endif // PLOTPANE_H
