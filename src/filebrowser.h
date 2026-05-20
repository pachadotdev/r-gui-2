#ifndef FILEBROWSER_H
#define FILEBROWSER_H

#include <QWidget>
#include <QTreeView>
#include <QFileSystemModel>
#include <QVBoxLayout>
#include <QLineEdit>

class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit FileBrowser(QWidget *parent = nullptr);
    void setRootPath(const QString &path);

signals:
    void fileDoubleClicked(const QString &filePath);

private slots:
    void onItemDoubleClicked(const QModelIndex &index);
    void onFilterChanged(const QString &text);
    void showContextMenu(const QPoint &pos);
    void showHeaderContextMenu(const QPoint &pos);
    void renameFile();
    void deleteFile();
    void copyFile();
    void pasteFile();
    void newFile();
    void newFolder();
    void sortByName();
    void sortByDate();

private:
    QTreeView *treeView;
    QFileSystemModel *model;
    QLineEdit *filterEdit;
    QString copiedFilePath;
};

#endif // FILEBROWSER_H
