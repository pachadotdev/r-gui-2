#include "environmentpane.h"
#include "terminalwidget.h"
#include <QHeaderView>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

EnvironmentPane::EnvironmentPane(TerminalWidget *terminal, QWidget *parent)
    : QWidget(parent), terminal(terminal)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // Toolbar
    QHBoxLayout *toolLayout = new QHBoxLayout();
    refreshButton = new QPushButton("Refresh", this);
    deleteButton = new QPushButton("Delete Checked", this);
    clearButton = new QPushButton("Clear All", this);
    gcButton = new QPushButton("Free Memory", this);
    
    toolLayout->addWidget(refreshButton);
    toolLayout->addWidget(deleteButton);
    toolLayout->addWidget(clearButton);
    toolLayout->addWidget(gcButton);
    layout->addLayout(toolLayout);
    
    // Memory label
    memoryLabel = new QLabel("Total size: 0 B", this);
    memoryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(memoryLabel);

    // Tree Widget
    treeWidget = new QTreeWidget(this);
    treeWidget->setHeaderLabels(QStringList() << "Name" << "Type" << "Length/Dim" << "Size");
    treeWidget->setSelectionMode(QAbstractItemView::NoSelection); // We use checkboxes
    layout->addWidget(treeWidget);

    // Initial status
    new QTreeWidgetItem(treeWidget, QStringList() << "Status" << "Waiting for R..." << "" << "");

    connect(refreshButton, &QPushButton::clicked, this, &EnvironmentPane::refreshEnvironment);
    connect(deleteButton, &QPushButton::clicked, this, &EnvironmentPane::deleteCheckedItems);
    connect(clearButton, &QPushButton::clicked, this, &EnvironmentPane::clearAllItems);
    connect(gcButton, &QPushButton::clicked, this, &EnvironmentPane::runGC);
    
    // Setup file watcher
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    envFilePath = QDir(tempDir).filePath("q_env.json");
    
    // Ensure file exists so watcher can watch it
    QFile f(envFilePath);
    if (!f.exists()) {
        if (f.open(QIODevice::WriteOnly)) {
            f.write("{}");
            f.close();
        }
    }
    
    fileWatcher = new QFileSystemWatcher(this);
    fileWatcher->addPath(envFilePath);
    
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, &EnvironmentPane::onEnvironmentFileChanged);
}

EnvironmentPane::~EnvironmentPane()
{
}

void EnvironmentPane::refreshEnvironment()
{
    if (!terminal) return;
    // terminal->executeCommand("if (requireNamespace('qide', quietly=TRUE)) qide::update_env()");
    terminal->executeCommand("qide::update_env()");
}

void EnvironmentPane::deleteCheckedItems()
{
    if (!terminal) return;

    QStringList vars;
    for (int i = 0; i < treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = treeWidget->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked) {
            vars << item->text(0);
        }
    }

    if (vars.isEmpty()) return;

    terminal->executeCommand(QString("rm(%1)").arg(vars.join(", ")));
}

void EnvironmentPane::clearAllItems()
{
    if (!terminal) return;
    terminal->executeCommand("rm(list=ls())");
}

void EnvironmentPane::runGC()
{
    if (!terminal) return;
    terminal->executeCommand("gc()");
}

void EnvironmentPane::onEnvironmentFileChanged(const QString &path)
{
    if (path != envFilePath) return;
    
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        file.close();
        if (data.isEmpty()) {
             treeWidget->clear();
             new QTreeWidgetItem(treeWidget, QStringList() << "Status" << "Environment empty" << "");
        } else {
             parseEnvironmentData(data);
        }
    } else {
        treeWidget->clear();
        new QTreeWidgetItem(treeWidget, QStringList() << "Error" << "Cannot read file" << "");
    }
    
    // Re-add path if watcher lost it (some editors delete/recreate files)
    if (!fileWatcher->files().contains(path)) {
        fileWatcher->addPath(path);
    }
}

void EnvironmentPane::parseEnvironmentData(const QByteArray &jsonData)
{
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (doc.isNull()) {
        treeWidget->clear();
        new QTreeWidgetItem(treeWidget, QStringList() << "Error" << "Invalid JSON data" << "");
        return;
    }

    QJsonObject root = doc.object();
    
    if (root.contains("error")) {
        treeWidget->clear();
        QTreeWidgetItem *item = new QTreeWidgetItem(treeWidget);
        item->setText(0, "Error: " + root["error"].toString());
        return;
    }

    QJsonArray objects = root["objects"].toArray();
    QJsonObject types = root["types"].toObject();
    QJsonObject dims = root["dim"].toObject();
    QJsonObject lens = root["len"].toObject();
    QJsonObject sizes = root["size"].toObject();
    
    // Update total size label
    double totalSize = root["total_size"].toDouble();
    
    double totalRam = 0;
#ifdef Q_OS_UNIX
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    totalRam = (double)pages * (double)page_size;
#endif

    if (totalRam > 0) {
        double percent = (totalSize / totalRam) * 100.0;
        memoryLabel->setText(QString("Used memory: %1 out of %2 (%3%)")
            .arg(formatSize(totalSize))
            .arg(formatSize(totalRam))
            .arg(QString::number(percent, 'f', 1)));
    } else {
        memoryLabel->setText("Used memory: " + formatSize(totalSize));
    }

    treeWidget->clear();

    for (const auto &objVal : objects) {
        QString name = objVal.toString();
        
        // Get Type
        QString typeStr;
        QJsonValue typeVal = types[name];
        if (typeVal.isArray()) {
            QJsonArray typeArr = typeVal.toArray();
            if (!typeArr.isEmpty()) typeStr = typeArr[0].toString();
        } else if (typeVal.isString()) {
             typeStr = typeVal.toString();
        }
        
        // Get Details (Dim or Length)
        QString details;
        
        // Only show details for specific types
        bool showDetails = true;
        if (typeStr == "function" || typeStr == "environment") {
            showDetails = false;
        }
        
        if (showDetails) {
            QJsonValue dimVal = dims[name];
            if (dimVal.isArray() && !dimVal.toArray().isEmpty()) {
                QJsonArray dimArr = dimVal.toArray();
                QStringList dimStrs;
                for(const auto &d : dimArr) dimStrs << QString::number(d.toInt());
                details = dimStrs.join("x");
            } else {
                QJsonValue lenVal = lens[name];
                if (lenVal.isArray() && !lenVal.toArray().isEmpty()) {
                     details = QString::number(lenVal.toArray()[0].toInt());
                } else if (lenVal.isDouble()) {
                     details = QString::number(lenVal.toInt());
                }
            }
        }
        
        // Get Size
        double sizeBytes = 0;
        QJsonValue sizeVal = sizes[name];
        if (sizeVal.isArray() && !sizeVal.toArray().isEmpty()) {
             sizeBytes = sizeVal.toArray()[0].toDouble();
        } else if (sizeVal.isDouble()) {
             sizeBytes = sizeVal.toDouble();
        }
        QString sizeStr = formatSize(sizeBytes);

        QTreeWidgetItem *item = new QTreeWidgetItem(treeWidget);
        item->setText(0, name);
        item->setText(1, typeStr);
        item->setText(2, details);
        item->setText(3, sizeStr);
        item->setCheckState(0, Qt::Unchecked); 
    }
}

QString EnvironmentPane::formatSize(double bytes)
{
    if (bytes < 1024) {
        return QString::number(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        return QString::number(bytes / 1024.0, 'f', 2) + " KB";
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
    } else {
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
    }
}
