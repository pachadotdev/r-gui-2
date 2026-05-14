#ifndef PLOTPANE_H
#define PLOTPANE_H

#include <QWidget>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QFileSystemWatcher>
#include <QPixmap>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>

class PlotPane : public QWidget
{
    Q_OBJECT

public:
    explicit PlotPane(const QString &plotDir, QWidget *parent = nullptr);
    ~PlotPane();

    QString plotDir() const { return m_plotDir; }

public slots:
    void refresh();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onIndexFileChanged(const QString &path);
    void zoomIn();
    void zoomOut();
    void zoomFit();

private:
    void loadPlot(const QString &filePath);
    void applyZoom();

    QString m_plotDir;
    QString m_indexFile;
    QString m_currentFile;

    QScrollArea  *m_scrollArea;
    QLabel       *m_imageLabel;
    QPushButton  *m_zoomInBtn;
    QPushButton  *m_zoomOutBtn;
    QPushButton  *m_zoomFitBtn;

    QFileSystemWatcher *m_watcher;
    QTimer             *m_reloadTimer;

    double  m_zoomFactor  = 1.0;
    bool    m_userZoomed  = false;   // true once user manually zoomed
    QPixmap m_currentPixmap;
};

#endif // PLOTPANE_H
