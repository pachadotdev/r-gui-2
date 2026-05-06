#ifndef PLOTPANE_H
#define PLOTPANE_H

#include <QWidget>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QFileSystemWatcher>
#include <QPixmap>
#include <QStringList>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QComboBox>
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

private slots:
    void onIndexFileChanged(const QString &path);
    void showPrevious();
    void showNext();
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void onPlotListChanged(int index);

private:
    void loadPlot(const QString &filePath);
    void updateNavButtons();
    void applyZoom();

    QString m_plotDir;
    QString m_indexFile;

    QScrollArea *m_scrollArea;
    QLabel *m_imageLabel;
    QPushButton *m_prevBtn;
    QPushButton *m_nextBtn;
    QPushButton *m_zoomInBtn;
    QPushButton *m_zoomOutBtn;
    QPushButton *m_zoomFitBtn;
    QComboBox *m_plotList;
    QLabel *m_statusLabel;

    QFileSystemWatcher *m_watcher;
    QTimer *m_reloadTimer;

    QStringList m_plotFiles;   // ordered list of captured plot file paths
    int m_currentIndex;        // index into m_plotFiles
    double m_zoomFactor;
    QPixmap m_currentPixmap;   // unscaled pixmap
};

#endif // PLOTPANE_H
