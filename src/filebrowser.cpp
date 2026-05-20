#include "filebrowser.h"
#include <QDir>
#include <QHeaderView>
#include <QSettings>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QClipboard>
#include <QApplication>

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    // Create layout
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Create filter input
    filterEdit = new QLineEdit(this);
    filterEdit->setPlaceholderText("Filter files...");
    layout->addWidget(filterEdit);
    
    // Create file system model
    model = new QFileSystemModel(this);
    model->setRootPath(QDir::homePath());
    model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    
    // Optionally hide hidden files to reduce KDE warnings
    // model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    
    // Create tree view
    treeView = new QTreeView(this);
    treeView->setModel(model);
    treeView->setRootIndex(model->index(QDir::homePath()));
    treeView->setAnimated(true);
    treeView->setIndentation(20);
    treeView->setSortingEnabled(true);
    treeView->sortByColumn(0, Qt::AscendingOrder);
    treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    
    // Restore column visibility (default: hide Size and Type)
    {
        QSettings s("Q", "Q");
        treeView->setColumnWidth(0, 200);
        treeView->setColumnHidden(1, s.value("filesBrowserCol1Hidden", true).toBool());
        treeView->setColumnHidden(2, s.value("filesBrowserCol2Hidden", true).toBool());
        // treeView->setColumnHidden(3, s.value("filesBrowserCol3Hidden", true).toBool());
    }
    
    // Set header
    treeView->header()->setStretchLastSection(true);
    // Allow resizing all columns
    treeView->header()->setSectionResizeMode(QHeaderView::Interactive);
    // Set initial width for name column
    treeView->setColumnWidth(0, 250);

    // Right-click on header to show/hide columns
    treeView->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(treeView->header(), &QHeaderView::customContextMenuRequested,
            this, &FileBrowser::showHeaderContextMenu);
    
    layout->addWidget(treeView);
    
    // Connect signals
    connect(treeView, &QTreeView::doubleClicked,
            this, &FileBrowser::onItemDoubleClicked);
    connect(filterEdit, &QLineEdit::textChanged,
            this, &FileBrowser::onFilterChanged);
    connect(treeView, &QTreeView::customContextMenuRequested,
            this, &FileBrowser::showContextMenu);
}

void FileBrowser::setRootPath(const QString &path)
{
    if (QDir(path).exists()) {
        treeView->setRootIndex(model->index(path));
    }
}

void FileBrowser::onItemDoubleClicked(const QModelIndex &index)
{
    QString filePath = model->filePath(index);
    
    if (QFileInfo(filePath).isFile()) {
        emit fileDoubleClicked(filePath);
    }
}

void FileBrowser::onFilterChanged(const QString &text)
{
    if (text.isEmpty()) {
        model->setNameFilters(QStringList());
        model->setNameFilterDisables(false);
    } else {
        QStringList filters;
        filters << QString("*%1*").arg(text);
        model->setNameFilters(filters);
        model->setNameFilterDisables(false);
    }
}

void FileBrowser::showContextMenu(const QPoint &pos)
{
    QModelIndex index = treeView->indexAt(pos);
    
    QMenu contextMenu(this);
    
    // New submenu
    QMenu *newMenu = contextMenu.addMenu(tr("New"));
    QAction *newFileAct = newMenu->addAction(tr("File"));
    QAction *newFolderAct = newMenu->addAction(tr("Folder"));
    connect(newFileAct, &QAction::triggered, this, &FileBrowser::newFile);
    connect(newFolderAct, &QAction::triggered, this, &FileBrowser::newFolder);
    
    contextMenu.addSeparator();
    
    // File operations (only if an item is selected)
    QAction *renameAct = nullptr;
    QAction *deleteAct = nullptr;
    QAction *copyAct = nullptr;
    
    if (index.isValid()) {
        renameAct = contextMenu.addAction(tr("Rename"));
        deleteAct = contextMenu.addAction(tr("Delete"));
        copyAct = contextMenu.addAction(tr("Copy"));
        
        connect(renameAct, &QAction::triggered, this, &FileBrowser::renameFile);
        connect(deleteAct, &QAction::triggered, this, &FileBrowser::deleteFile);
        connect(copyAct, &QAction::triggered, this, &FileBrowser::copyFile);
    }
    
    // Paste (always available if something is copied)
    QAction *pasteAct = nullptr;
    if (!copiedFilePath.isEmpty()) {
        pasteAct = contextMenu.addAction(tr("Paste"));
        connect(pasteAct, &QAction::triggered, this, &FileBrowser::pasteFile);
    }
    
    contextMenu.addSeparator();
    
    // Sort submenu
    QMenu *sortMenu = contextMenu.addMenu(tr("Sort By"));
    QAction *sortNameAct = sortMenu->addAction(tr("Name"));
    QAction *sortDateAct = sortMenu->addAction(tr("Date Modified"));
    connect(sortNameAct, &QAction::triggered, this, &FileBrowser::sortByName);
    connect(sortDateAct, &QAction::triggered, this, &FileBrowser::sortByDate);
    
    contextMenu.exec(treeView->viewport()->mapToGlobal(pos));
}

void FileBrowser::showHeaderContextMenu(const QPoint &pos)
{
    struct ColInfo { int col; QString label; };
    const ColInfo cols[] = {
        {1, tr("Size")},
        {2, tr("Type")},
        {3, tr("Date Modified")},
    };

    QMenu menu(this);
    for (const auto &c : cols) {
        QAction *act = menu.addAction(c.label);
        act->setCheckable(true);
        act->setChecked(!treeView->isColumnHidden(c.col));
        int col = c.col;
        connect(act, &QAction::toggled, this, [this, col](bool checked) {
            treeView->setColumnHidden(col, !checked);
            QSettings s("Q", "Q");
            s.setValue(QString("filesBrowserCol%1Hidden").arg(col), !checked);
        });
    }
    menu.exec(treeView->header()->mapToGlobal(pos));
}

void FileBrowser::renameFile()
{
    QModelIndex index = treeView->currentIndex();
    if (!index.isValid()) return;
    
    QString oldPath = model->filePath(index);
    QString oldName = model->fileName(index);
    
    bool ok;
    QString newName = QInputDialog::getText(this, tr("Rename"),
        tr("New name:"), QLineEdit::Normal, oldName, &ok);
    
    if (ok && !newName.isEmpty() && newName != oldName) {
        QFileInfo fileInfo(oldPath);
        QString newPath = fileInfo.absolutePath() + "/" + newName;
        
        if (QFile::exists(newPath)) {
            QMessageBox::warning(this, tr("Rename Failed"),
                tr("A file with that name already exists."));
            return;
        }
        
        if (!QFile::rename(oldPath, newPath)) {
            QMessageBox::warning(this, tr("Rename Failed"),
                tr("Could not rename the file."));
        }
    }
}

void FileBrowser::deleteFile()
{
    QModelIndex index = treeView->currentIndex();
    if (!index.isValid()) return;
    
    QString filePath = model->filePath(index);
    QFileInfo fileInfo(filePath);
    
    QString message = fileInfo.isDir() 
        ? tr("Are you sure you want to delete the folder '%1' and all its contents?")
        : tr("Are you sure you want to delete '%1'?");
    
    QMessageBox::StandardButton reply = QMessageBox::question(this,
        tr("Delete"), message.arg(fileInfo.fileName()),
        QMessageBox::Yes | QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        bool success = false;
        if (fileInfo.isDir()) {
            QDir dir(filePath);
            success = dir.removeRecursively();
        } else {
            success = QFile::remove(filePath);
        }
        
        if (!success) {
            QMessageBox::warning(this, tr("Delete Failed"),
                tr("Could not delete the file or folder."));
        }
    }
}

void FileBrowser::copyFile()
{
    QModelIndex index = treeView->currentIndex();
    if (!index.isValid()) return;
    
    copiedFilePath = model->filePath(index);
}

void FileBrowser::pasteFile()
{
    if (copiedFilePath.isEmpty()) return;
    
    QModelIndex index = treeView->currentIndex();
    QString targetDir;
    
    if (index.isValid()) {
        QString path = model->filePath(index);
        QFileInfo info(path);
        targetDir = info.isDir() ? path : info.absolutePath();
    } else {
        targetDir = model->rootPath();
    }
    
    QFileInfo sourceInfo(copiedFilePath);
    QString targetPath = targetDir + "/" + sourceInfo.fileName();
    
    // Handle name conflicts
    if (QFile::exists(targetPath)) {
        int counter = 1;
        QString baseName = sourceInfo.completeBaseName();
        QString suffix = sourceInfo.suffix();
        
        do {
            QString newName = suffix.isEmpty() 
                ? QString("%1_%2").arg(baseName).arg(counter)
                : QString("%1_%2.%3").arg(baseName).arg(counter).arg(suffix);
            targetPath = targetDir + "/" + newName;
            counter++;
        } while (QFile::exists(targetPath));
    }
    
    if (sourceInfo.isDir()) {
        // Copy directory recursively
        QDir sourceDir(copiedFilePath);
        QDir().mkpath(targetPath);
        
        QMessageBox::information(this, tr("Copy"),
            tr("Directory copying not fully implemented yet. Only single files are supported."));
    } else {
        if (!QFile::copy(copiedFilePath, targetPath)) {
            QMessageBox::warning(this, tr("Copy Failed"),
                tr("Could not copy the file."));
        }
    }
}

void FileBrowser::newFile()
{
    QModelIndex index = treeView->currentIndex();
    QString targetDir;
    
    if (index.isValid()) {
        QString path = model->filePath(index);
        QFileInfo info(path);
        targetDir = info.isDir() ? path : info.absolutePath();
    } else {
        targetDir = model->rootPath();
    }
    
    bool ok;
    QString fileName = QInputDialog::getText(this, tr("New File"),
        tr("File name:"), QLineEdit::Normal, "untitled.r", &ok);
    
    if (ok && !fileName.isEmpty()) {
        QString filePath = targetDir + "/" + fileName;
        
        if (QFile::exists(filePath)) {
            QMessageBox::warning(this, tr("Create Failed"),
                tr("A file with that name already exists."));
            return;
        }
        
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, tr("Create Failed"),
                tr("Could not create the file."));
        } else {
            file.close();
        }
    }
}

void FileBrowser::newFolder()
{
    QModelIndex index = treeView->currentIndex();
    QString targetDir;
    
    if (index.isValid()) {
        QString path = model->filePath(index);
        QFileInfo info(path);
        targetDir = info.isDir() ? path : info.absolutePath();
    } else {
        targetDir = model->rootPath();
    }
    
    bool ok;
    QString folderName = QInputDialog::getText(this, tr("New Folder"),
        tr("Folder name:"), QLineEdit::Normal, "New Folder", &ok);
    
    if (ok && !folderName.isEmpty()) {
        QString folderPath = targetDir + "/" + folderName;
        
        if (QDir(folderPath).exists()) {
            QMessageBox::warning(this, tr("Create Failed"),
                tr("A folder with that name already exists."));
            return;
        }
        
        if (!QDir().mkdir(folderPath)) {
            QMessageBox::warning(this, tr("Create Failed"),
                tr("Could not create the folder."));
        }
    }
}

void FileBrowser::sortByName()
{
    treeView->sortByColumn(0, Qt::AscendingOrder);
}

void FileBrowser::sortByDate()
{
    treeView->sortByColumn(3, Qt::DescendingOrder);
}
