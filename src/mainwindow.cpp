#include "mainwindow.h"
#include "codeeditor.h"
#include "filebrowser.h"
#include "terminalwidget.h"
#include "environmentpane.h"
#include "plotpane.h"
#include "helppane.h"
#include "thememanager.h"

#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QSettings>
#include <QTextStream>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QMenu>
#include <QFileInfo>
#include <QTimer>
#include <QStandardPaths>
#include <QApplication>
#include <QSplitter>
#include <QEvent>
#include <QResizeEvent>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QShortcut>
#include <QDesktopServices>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , console(nullptr)
{
    setWindowTitle("R GUI 2");
    resize(1200, 800);

    // ── Editor tabs (center-top) ────────────────────────────────────────────
    editorTabs = new QTabWidget(this);
    editorTabs->setTabsClosable(true);
    editorTabs->setMovable(true);
    addNewEditorTab();

    // ── Console tabs (center-bottom) ────────────────────────────────────────
    consoleTabs = new QTabWidget(this);
    consoleTabs->setTabsClosable(true);
    consoleTabs->setMovable(true);
    console = new TerminalWidget("R", this);
    consoleTabs->addTab(console, "R Console");
    connect(console, &TerminalWidget::fontSizeAdjustRequested,
            this,    &MainWindow::adjustAllTerminalFontSize);
    consoleTabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    consoleTabs->tabBar()->setTabButton(0, QTabBar::LeftSide,  nullptr);
    connect(consoleTabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        if (index > 0) {
            QWidget *w = consoleTabs->widget(index);
            consoleTabs->removeTab(index);
            delete w;
        }
    });

    // ── Center splitter (editor top / console bottom) ────────────────────────
    m_centerSplitter = new QSplitter(Qt::Vertical, this);
    m_centerSplitter->addWidget(editorTabs);
    m_centerSplitter->addWidget(consoleTabs);
    m_centerSplitter->setChildrenCollapsible(false);

    // ── Left: file browser ────────────────────────────────────────────────────
    fileBrowser = new FileBrowser(this);

    // ── Right: tabbed pane (Help / Environment / Plots) ──────────────────────
    QString plotDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                          .filePath("rgui2_plots");
    envPane  = new EnvironmentPane(console, this);
    plotPane = new PlotPane(plotDir, this);
    helpPane = new HelpPane(console, this);
    m_rightTabs = new QTabWidget(this);
    m_rightTabs->addTab(envPane,   tr("Environment"));
    m_rightTabs->addTab(plotPane,  tr("Plots"));
    m_rightTabs->addTab(helpPane,  tr("Help"));

    // ── Outer horizontal splitter (left | center | right) ────────────────────
    m_outerSplitter = new QSplitter(Qt::Horizontal, this);
    m_outerSplitter->addWidget(fileBrowser);
    m_outerSplitter->addWidget(m_centerSplitter);
    m_outerSplitter->addWidget(m_rightTabs);
    m_outerSplitter->setChildrenCollapsible(false);

    setCentralWidget(m_outerSplitter);

    createMenus();
    menuBar()->setNativeMenuBar(false);
    setupConnections();
    loadSettings();
}

MainWindow::~MainWindow()
{
    saveSettings();
}

void MainWindow::createMenus()
{
    // File menu
    fileMenu = menuBar()->addMenu(tr("&File"));
    
    QAction *newAct = new QAction(tr("&New Script"), this);
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, &MainWindow::newFile);
    fileMenu->addAction(newAct);
    
    QAction *openAct = new QAction(tr("&Open File..."), this);
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);
    fileMenu->addAction(openAct);
    
    QAction *openDirAct = new QAction(tr("Open &Directory..."), this);
    openDirAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
    connect(openDirAct, &QAction::triggered, this, &MainWindow::openDirectory);
    fileMenu->addAction(openDirAct);
    
    fileMenu->addSeparator();
    
    QAction *createProjAct = new QAction(tr("Create &Project..."), this);
    createProjAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    connect(createProjAct, &QAction::triggered, this, &MainWindow::createProject);
    fileMenu->addAction(createProjAct);
    
    fileMenu->addSeparator();
    
    QAction *saveAct = new QAction(tr("&Save"), this);
    saveAct->setShortcut(QKeySequence::Save);
    connect(saveAct, &QAction::triggered, this, &MainWindow::saveFile);
    fileMenu->addAction(saveAct);
    
    QAction *saveAsAct = new QAction(tr("Save &As..."), this);
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::saveFileAs);
    fileMenu->addAction(saveAsAct);
    
    fileMenu->addSeparator();
    
    QAction *quitAct = new QAction(tr("&Quit"), this);
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quitAct);
    
    // Code menu
    codeMenu = menuBar()->addMenu(tr("&Code"));
    
    QAction *runLineAct = new QAction(tr("Run Line/Selection"), this);
    runLineAct->setShortcut(Qt::CTRL | Qt::Key_Return);
    connect(runLineAct, &QAction::triggered, this, &MainWindow::runCurrentLine);
    codeMenu->addAction(runLineAct);
    
    QAction *runSelAct = new QAction(tr("Run Selection Only"), this);
    runSelAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_Return);
    connect(runSelAct, &QAction::triggered, this, &MainWindow::runSelection);
    codeMenu->addAction(runSelAct);
    
    QAction *runAllAct = new QAction(tr("Run All"), this);
    runAllAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A);
    connect(runAllAct, &QAction::triggered, this, &MainWindow::runAll);
    codeMenu->addAction(runAllAct);
    
    codeMenu->addSeparator();
    
    QAction *sourceAct = new QAction(tr("Source File"), this);
    sourceAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    connect(sourceAct, &QAction::triggered, this, &MainWindow::sourceFile);
    codeMenu->addAction(sourceAct);
    
    codeMenu->addSeparator();
    
    QAction *pipeAct = new QAction(tr("Insert Native Pipe |>"), this);
    pipeAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_M);
    connect(pipeAct, &QAction::triggered, this, [this]() {
        CodeEditor *editor = getCurrentEditor();
        if (editor) {
            editor->textCursor().insertText(" |> ");
        }
    });
    codeMenu->addAction(pipeAct);
    
    QAction *clearConsoleAct = new QAction(tr("Clear Console"), this);
    clearConsoleAct->setShortcut(Qt::CTRL | Qt::Key_L);
    connect(clearConsoleAct, &QAction::triggered, this, [this]() {
        // If the current console tab is an R session, call the R function clear()
        // (equivalent to the right-click -> Clear behavior in the R console).
        if (!consoleTabs) return;
        TerminalWidget *active = qobject_cast<TerminalWidget*>(consoleTabs->currentWidget());
        if (active) {
            QString shellName = QFileInfo(active->getShell()).fileName().toLower();
            if (shellName == "r") {
                active->executeCommand("clear()");
            } else {
                // Ctrl+L clears most shells; TerminalWidget routes this to the PTY
                active->writeToShell("\x0c");
            }
        }
    });
    codeMenu->addAction(clearConsoleAct);
    
    // View menu
    viewMenu = menuBar()->addMenu(tr("&View"));

    QAction *themeAct = new QAction(tr("Change &Theme..."), this);
    themeAct->setShortcut(Qt::CTRL | Qt::Key_T);
    connect(themeAct, &QAction::triggered, this, &MainWindow::changeTheme);
    viewMenu->addAction(themeAct);

    viewMenu->addSeparator();

    // Font size menu items (display only - shortcuts handled below by QShortcut)
    QAction *fontBiggerAct = new QAction(tr("Increase Terminal Font  Ctrl++"), this);
    connect(fontBiggerAct, &QAction::triggered, this, [this]() { adjustAllTerminalFontSize(+1); });
    viewMenu->addAction(fontBiggerAct);

    QAction *fontSmallerAct = new QAction(tr("Decrease Terminal Font  Ctrl+-"), this);
    connect(fontSmallerAct, &QAction::triggered, this, [this]() { adjustAllTerminalFontSize(-1); });
    viewMenu->addAction(fontSmallerAct);

    QAction *fontResetAct = new QAction(tr("Reset Terminal Font  Ctrl+0"), this);
    connect(fontResetAct, &QAction::triggered, this, [this]() { adjustAllTerminalFontSize(0); });
    viewMenu->addAction(fontResetAct);

    // Application-level shortcuts so they fire before QWebEngineView consumes them.
    // Ctrl++ needs both Key_Plus (Shift+=) and Key_Equal (plain =) covered.
    auto makeFontSC = [this](const QKeySequence &ks, int delta) {
        auto *sc = new QShortcut(ks, this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, [this, delta]() { adjustAllTerminalFontSize(delta); });
    };
    makeFontSC(QKeySequence(Qt::CTRL | Qt::Key_Plus),  +1); // Ctrl+Shift+=
    makeFontSC(QKeySequence(Qt::CTRL | Qt::Key_Equal), +1); // Ctrl+= (no shift)
    makeFontSC(QKeySequence(Qt::CTRL | Qt::Key_Minus),  -1);
    makeFontSC(QKeySequence(Qt::CTRL | Qt::Key_0),       0);
    QMenu *terminalMenuBar = menuBar()->addMenu(tr("&Terminal"));
    struct ShellEntry { QString path; QString label; };
    QList<ShellEntry> shellCandidates = {
        {"/bin/zsh",                                                  "zsh"},
        {"/usr/bin/zsh",                                              "zsh"},
        {"/bin/bash",                                                 "bash"},
        {"/usr/bin/bash",                                             "bash"},
        {"/bin/sh",                                                   "sh"},
        {"/usr/bin/sh",                                               "sh"},
        {"/usr/bin/fish",                                             "fish"},
        {"/bin/fish",                                                 "fish"},
        {"/opt/homebrew/bin/zsh",                                     "zsh"},
        {"/usr/local/bin/zsh",                                        "zsh"},
        {"/usr/bin/pwsh",                                             "PowerShell"},
        {"/usr/local/bin/pwsh",                                       "PowerShell"},
        {"C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "PowerShell"},
        {"C:/Program Files/PowerShell/7/pwsh.exe",                   "PowerShell"},
        {"C:/msys64/usr/bin/bash.exe",                                "MSYS2 bash"},
        {"C:/msys64/mingw64/bin/bash.exe",                            "MSYS2 bash"},
    };
    QStringList addedLabels;
    for (const ShellEntry &entry : shellCandidates) {
        if (QFile::exists(entry.path) && !addedLabels.contains(entry.label)) {
            QAction *shellAction = terminalMenuBar->addAction(tr("New %1 terminal").arg(entry.label));
            connect(shellAction, &QAction::triggered, this, [this, entry]() {
                TerminalWidget *terminal = new TerminalWidget(entry.path, this);
                if (!m_currentDir.isEmpty())
                    terminal->setWorkingDirectory(m_currentDir);
                connect(terminal, &TerminalWidget::fontSizeAdjustRequested,
                        this,     &MainWindow::adjustAllTerminalFontSize);
                // Start this terminal at the current global font size.
                terminal->setInitialFontSize(m_globalFontSize);
                int index = consoleTabs->addTab(terminal, entry.label);
                consoleTabs->setCurrentIndex(index);
            });
            addedLabels << entry.label;
        }
    }

    // Help menu
    helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAct = new QAction(tr("&About"), this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::about);
    helpMenu->addAction(aboutAct);
}

void MainWindow::setupConnections()
{
    // Tab close button
    connect(editorTabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget *widget = editorTabs->widget(index);
        editorTabs->removeTab(index);
        delete widget;
        
        // Add new tab if all closed
        if (editorTabs->count() == 0) {
            addNewEditorTab();
        }
    });
    
    // File browser double-click to open
    connect(fileBrowser, &FileBrowser::fileDoubleClicked, this, [this](const QString &path) {
        QFileInfo fileInfo(path);
        QString suffix = fileInfo.suffix().toLower();

        // .rproject: switch project directory
        if (suffix == "rproject") {
            QString projectDir = fileInfo.absolutePath();
            fileBrowser->setRootPath(projectDir);
            m_currentDir = projectDir;
            if (console) {
                console->executeCommand(
                    QString("setwd('%1')").arg(QString(projectDir).replace('\\', '/')));
                statusBar()->showMessage(tr("Opened project: %1").arg(fileInfo.fileName()), 5000);
            }
            return;
        }

        // Binary / media formats: open with the OS default application.
        static const QStringList osTypes = {
            "png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "tiff", "tif",
            "pdf", "xlsx", "xls", "docx", "doc", "pptx", "ppt",
            "zip", "tar", "gz", "bz2", "xz",
            "mp4", "avi", "mov", "mkv", "mp3", "wav", "ogg"
        };
        if (osTypes.contains(suffix)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }

        // Everything else: open as plain text in the editor.
        // We try to read the file; if it looks binary we fall back to the OS.
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QByteArray sample = file.read(8192);
        // Heuristic: if the sample contains a NUL byte it is likely binary.
        if (sample.contains('\0')) {
            file.close();
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
        // Seek back and read the rest as text.
        file.seek(0);
        QTextStream in(&file);
        QString content = in.readAll();
        file.close();

        addNewEditorTab(fileInfo.fileName());
        CodeEditor *editor = getCurrentEditor();
        if (editor) {
            editor->setPlainText(content);
            editor->setProperty("filePath", path);
            editor->setLanguageFromFile(path);
            editor->document()->setModified(false);
        }
    });
}

void MainWindow::loadSettings()
{
    QSettings settings("RGUI2", "RGUI2");
    restoreGeometry(settings.value("geometry").toByteArray());

    m_globalFontSize = settings.value("globalFontSize", 11).toInt();
    m_globalFontSize = qBound(6, m_globalFontSize, 32);
    for (int i = 0; i < consoleTabs->count(); ++i) {
        if (auto *tw = qobject_cast<TerminalWidget*>(consoleTabs->widget(i)))
            tw->setInitialFontSize(m_globalFontSize);
    }
    for (int i = 0; i < editorTabs->count(); ++i) {
        if (auto *ed = qobject_cast<CodeEditor*>(editorTabs->widget(i)))
            ed->setFontSize(m_globalFontSize);
    }

    // Restore splitter sizes, or fall back to defaults (15 % | 60 % | 25 %).
    QByteArray outerState  = settings.value("outerSplitter").toByteArray();
    QByteArray centerState = settings.value("centerSplitter").toByteArray();

    if (!outerState.isEmpty()) {
        m_outerSplitter->restoreState(outerState);
    } else {
        QTimer::singleShot(0, this, [this]() {
            int total = m_outerSplitter->width();
            m_outerSplitter->setSizes({total * 15 / 100,
                                       total * 60 / 100,
                                       total * 25 / 100});
        });
    }

    if (!centerState.isEmpty()) {
        m_centerSplitter->restoreState(centerState);
    } else {
        QTimer::singleShot(0, this, [this]() {
            int total = m_centerSplitter->height();
            m_centerSplitter->setSizes({total * 60 / 100, total * 40 / 100});
        });
    }
}

void MainWindow::saveSettings()
{
    QSettings settings("RGUI2", "RGUI2");
    settings.setValue("geometry",       saveGeometry());
    settings.setValue("outerSplitter",  m_outerSplitter->saveState());
    settings.setValue("centerSplitter", m_centerSplitter->saveState());
    settings.setValue("globalFontSize", m_globalFontSize);
}

CodeEditor* MainWindow::getCurrentEditor()
{
    if (!editorTabs || editorTabs->count() == 0) {
        return nullptr;
    }
    return qobject_cast<CodeEditor*>(editorTabs->currentWidget());
}

void MainWindow::about()
{
    QMessageBox::about(this, "About",
                      "R GUI 2\n\n"
                      "A Qt-based software to ease using R.\n\n"
                      "Released under the Apache 2.0 License.");
}

void MainWindow::addNewEditorTab(const QString &title)
{
    CodeEditor *editor = new CodeEditor(this);
    if (m_globalFontSize != 11)
        editor->setFontSize(m_globalFontSize);
    int index = editorTabs->addTab(editor, title);
    editorTabs->setCurrentIndex(index);
    
    // Track document modifications to update tab title
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor](bool changed) {
        // Find which tab this editor is in
        for (int i = 0; i < editorTabs->count(); ++i) {
            if (editorTabs->widget(i) == editor) {
                updateTabTitle(i, changed);
                break;
            }
        }
    });
}

void MainWindow::updateTabTitle(int index, bool modified)
{
    if (index < 0 || index >= editorTabs->count()) return;
    
    CodeEditor *editor = qobject_cast<CodeEditor*>(editorTabs->widget(index));
    if (!editor) return;
    
    // Get the base title (file name or "Untitled")
    QString baseTitle = editorTabs->tabText(index);
    // Remove existing asterisk if present
    baseTitle = baseTitle.replace(" *", "").trimmed();
    
    // Update title with asterisk if modified
    QString newTitle = modified ? baseTitle + " *" : baseTitle;
    editorTabs->setTabText(index, newTitle);
    
    // Apply italic font style using QTabBar API
    QFont tabFont = editorTabs->tabBar()->font();
    tabFont.setItalic(modified);
    
    // Unfortunately QTabBar doesn't have per-tab font setting directly
    // We need to use a custom approach with QProxyStyle or update all tabs
    // For now, we'll use a stylesheet that applies to tabs with asterisk
    QString stylesheet = QString(
        "QTabBar::tab { font-style: normal; }"
        "QTabBar::tab:selected { font-style: %1; }"
    ).arg(modified ? "italic" : "normal");
    
    // Store modification state as a property for potential future use
    editorTabs->widget(index)->setProperty("modified", modified);
    
    // Set tooltip
    editorTabs->setTabToolTip(index, modified ? tr("Modified - %1").arg(baseTitle) : baseTitle);
    
    // Apply stylesheet to make current tab italic if modified
    if (index == editorTabs->currentIndex()) {
        editorTabs->setStyleSheet(stylesheet);
    }
}

void MainWindow::newFile()
{
    addNewEditorTab("Untitled");
}

void MainWindow::openFile()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Open File"), "", 
        tr("All Supported Files (*.r *.R *.rmd *.Rmd *.qmd *.Qmd *.rproject *.h *.c *.cpp *.hpp);;"
           "R Files (*.R *.r *.Rmd *.rmd *.Qmd *.qmd);;"
           "C++ Files (*.cpp *.hpp *.h *.c);;"
           "R Projects (*.rproject);;"
           "All Files (*)"));
    
    if (!fileName.isEmpty()) {
        QFileInfo fileInfo(fileName);
        QString suffix = fileInfo.suffix().toLower();
        
        // Special handling for .rproject files
        if (suffix == "rproject") {
            QString projectDir = fileInfo.absolutePath();
            
            // Set file browser to project directory
            fileBrowser->setRootPath(projectDir);
            
            // Set R working directory
            if (console) {
                QString rCommand = QString("setwd('%1')").arg(projectDir.replace('\\', '/'));
                console->executeCommand(rCommand);
                statusBar()->showMessage(tr("Opened project: %1").arg(fileInfo.fileName()), 5000);
            }
        }
        
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            QString content = in.readAll();
            file.close();
            
            addNewEditorTab(fileInfo.fileName());
            CodeEditor *editor = getCurrentEditor();
            if (editor) {
                editor->setPlainText(content);
                editor->setProperty("filePath", fileName);
                editor->setLanguageFromFile(fileName);
                editor->document()->setModified(false);
            }
        }
    }
}

void MainWindow::openDirectory()
{
    QString dirPath = QFileDialog::getExistingDirectory(this,
        tr("Open Directory"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    
    if (!dirPath.isEmpty()) {
        // Remember for future terminal sessions.
        m_currentDir = dirPath;

        // Set file browser to this directory
        fileBrowser->setRootPath(dirPath);
        
        // Set R working directory in the R console.
        if (console) {
            QString rCommand = QString("setwd('%1')").arg(QString(dirPath).replace('\\', '/'));
            console->executeCommand(rCommand);
        }

        // cd in every non-R terminal tab.
        const QString cdCmd = QString("cd '%1'\n").arg(dirPath);
        for (int i = 1; i < consoleTabs->count(); ++i) {
            if (auto *tw = qobject_cast<TerminalWidget*>(consoleTabs->widget(i))) {
                QString shellName = QFileInfo(tw->getShell()).fileName().toLower();
                if (shellName != "r")
                    tw->writeToShell(cdCmd);
            }
        }

        statusBar()->showMessage(tr("Working directory: %1").arg(dirPath), 5000);
    }
}

void MainWindow::createProject()
{
    QString dirPath = QFileDialog::getExistingDirectory(this,
        tr("Select Project Directory"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    
    if (dirPath.isEmpty()) {
        return;
    }
    
    // Ask for project name
    bool ok;
    QString projectName = QInputDialog::getText(this, tr("Create Project"),
        tr("Project name:"), QLineEdit::Normal,
        QFileInfo(dirPath).fileName(), &ok);
    
    if (!ok || projectName.isEmpty()) {
        return;
    }
    
    // Create .rproject file
    QString rProjPath = QDir(dirPath).filePath(projectName + ".rproject");
    
    if (QFile::exists(rProjPath)) {
        QMessageBox::StandardButton reply = QMessageBox::question(this,
            tr("Project Exists"),
            tr("A project file already exists. Overwrite?"),
            QMessageBox::Yes | QMessageBox::No);
        
        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    
    // Write .Rproj file with default settings
    QFile file(rProjPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "Version: 1.0\n\n";
        out << "RestoreWorkspace: No\n";
        out << "SaveWorkspace: No\n";
        out << "AlwaysSaveHistory: Yes\n\n";
        out << "EnableCodeIndexing: Yes\n";
        out << "UseSpacesForTab: Yes\n";
        out << "NumSpacesForTab: 2\n";
        out << "Encoding: UTF-8\n\n";
        out << "RnwWeave: Sweave\n";
        out << "LaTeX: pdfLaTeX\n";
        file.close();
        
        // Open the directory and set working directory
        fileBrowser->setRootPath(dirPath);
        
        if (console) {
            QString rCommand = QString("setwd('%1')").arg(dirPath.replace('\\', '/'));
            console->executeCommand(rCommand);
        }
        
        QMessageBox::information(this, tr("Project Created"),
            tr("Project created successfully:\n%1").arg(rProjPath));
        
        statusBar()->showMessage(tr("Project created: %1").arg(projectName), 5000);
    } else {
        QMessageBox::warning(this, tr("Error"),
            tr("Could not create project file:\n%1").arg(rProjPath));
    }
}

void MainWindow::saveFile()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor) return;
    
    QString filePath = editor->property("filePath").toString();
    if (filePath.isEmpty()) {
        saveFileAs();
        return;
    }
    
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << editor->toPlainText();
        file.close();
        editor->document()->setModified(false);
        statusBar()->showMessage(tr("File saved: %1").arg(filePath), 3000);
    }
}

void MainWindow::saveFileAs()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor) return;
    
    // Get current file path to determine default extension
    QString currentPath = editor->property("filePath").toString();
    QString defaultFilter = "R Scripts (*.r *.R)";
    QString selectedFilter;
    
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Save File"), currentPath.isEmpty() ? "untitled.r" : currentPath,
        tr("R Scripts (*.r *.R);;"
           "R Markdown (*.Rmd *.rmd);;"
           "Quarto (*.Qmd *.qmd);;"
           "C++ Files (*.cpp *.hpp *.h *.c);;"
           "All Files (*)"),
        &selectedFilter);
    
    if (!fileName.isEmpty()) {
        // Auto-append extension if no extension provided
        QFileInfo fileInfo(fileName);
        if (fileInfo.suffix().isEmpty()) {
            // Determine extension based on selected filter
            if (selectedFilter.contains("R Markdown")) {
                fileName += ".Rmd";
            } else if (selectedFilter.contains("Quarto")) {
                fileName += ".Qmd";
            } else if (selectedFilter.contains("C++")) {
                fileName += ".cpp";
            } else {
                // Default to .r for R scripts
                fileName += ".r";
            }
        }
        // Don't add extension if file already has proper one
        else if (fileInfo.suffix().toLower() == "r" || 
                 fileInfo.suffix().toLower() == "rmd" ||
                 fileInfo.suffix().toLower() == "qmd" ||
                 fileInfo.suffix() == "cpp" ||
                 fileInfo.suffix() == "hpp" ||
                 fileInfo.suffix() == "h" ||
                 fileInfo.suffix() == "c") {
            // Keep the existing extension as-is
        }
        
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << editor->toPlainText();
            file.close();
            
            editor->setProperty("filePath", fileName);
            editor->document()->setModified(false);
            editorTabs->setTabText(editorTabs->currentIndex(), QFileInfo(fileName).fileName());
            statusBar()->showMessage(tr("File saved: %1").arg(fileName), 3000);
        }
    }
}

void MainWindow::runCurrentLine()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor || !console) return;
    
    // Check if there's a selection first
    QString selection = editor->textCursor().selectedText();
    if (!selection.isEmpty()) {
        // If there's a selection, run it
        selection.replace(QChar(0x2029), '\n');
        console->executeRCode(selection);
        return;
    }
    
    // Otherwise, run the current line
    QTextCursor cursor = editor->textCursor();
    cursor.select(QTextCursor::LineUnderCursor);
    QString line = cursor.selectedText();
    
    if (!line.trimmed().isEmpty()) {
        console->executeCommand(line);
    }
}

void MainWindow::runSelection()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor || !console) return;
    
    QString selection = editor->textCursor().selectedText();
    if (!selection.isEmpty()) {
        // Qt uses Unicode paragraph separator, replace with newline
        selection.replace(QChar(0x2029), '\n');
        console->executeRCode(selection);
    }
}

void MainWindow::runAll()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor || !console) return;
    
    QString code = editor->toPlainText();
    if (!code.isEmpty()) {
        console->executeRCode(code);
    }
}

void MainWindow::sourceFile()
{
    CodeEditor *editor = getCurrentEditor();
    if (!editor || !console) return;
    
    QString filePath = editor->property("filePath").toString();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, tr("Source File"),
            tr("Please save the file before sourcing."));
        return;
    }
    
    // Use forward slashes for R
    filePath.replace('\\', '/');
    QString command = QString("source('%1')").arg(filePath);
    console->executeCommand(command);
}

void MainWindow::changeTheme()
{
    ThemeManager &themeMgr = ThemeManager::instance();
    QStringList themes = themeMgr.availableThemes();
    
    // Create a dialog with theme selection, search bar, and attribution
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Select Theme"));
    dialog.setMinimumWidth(500);
    dialog.setMinimumHeight(600);
    
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    
    // Add info label with attribution
    QLabel *infoLabel = new QLabel(&dialog);
    infoLabel->setOpenExternalLinks(true);
    infoLabel->setTextFormat(Qt::RichText);
    infoLabel->setText(
        tr("Choose from <b>%1 themes</b><br>"
           "<small>Themes from: <a href='https://github.com/Gogh-Co/Gogh'>Gogh Project</a></small>")
        .arg(themes.count()));
    layout->addWidget(infoLabel);
    
    // Add search bar
    QLineEdit *searchBox = new QLineEdit(&dialog);
    searchBox->setPlaceholderText(tr("Search themes..."));
    searchBox->setClearButtonEnabled(true);
    layout->addWidget(searchBox);
    
    // Theme list
    QListWidget *themeList = new QListWidget(&dialog);
    themeList->addItems(themes);
    
    // Select current theme
    int currentIndex = themes.indexOf(themeMgr.currentTheme().name);
    if (currentIndex >= 0) {
        themeList->setCurrentRow(currentIndex);
        themeList->scrollToItem(themeList->item(currentIndex));
    }
    
    layout->addWidget(themeList);
    
    // Connect search box to filter themes
    connect(searchBox, &QLineEdit::textChanged, [themeList, themes](const QString &text) {
        QString searchText = text.toLower();
        
        for (int i = 0; i < themeList->count(); ++i) {
            QListWidgetItem *item = themeList->item(i);
            if (item) {
                QString themeName = item->text().toLower();
                // Show items that contain the search text
                bool matches = themeName.contains(searchText);
                item->setHidden(!matches);
            }
        }
        
        // Select first visible item if current is hidden
        if (themeList->currentItem() && themeList->currentItem()->isHidden()) {
            for (int i = 0; i < themeList->count(); ++i) {
                QListWidgetItem *item = themeList->item(i);
                if (item && !item->isHidden()) {
                    themeList->setCurrentItem(item);
                    break;
                }
            }
        }
    });
    
    // Buttons
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);
    
    // Handle double-click to apply immediately
    connect(themeList, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    
    // Focus search box initially
    searchBox->setFocus();
    
    if (dialog.exec() == QDialog::Accepted && themeList->currentItem()) {
        QString selectedTheme = themeList->currentItem()->text();
        
        qDebug() << "Applying theme:" << selectedTheme;
        
        themeMgr.setCurrentTheme(selectedTheme);
        EditorTheme theme = themeMgr.currentTheme();
        
        // Validate theme before applying
        if (theme.name.isEmpty() || !theme.background.isValid() || !theme.foreground.isValid()) {
            qWarning() << "Invalid theme loaded, skipping application";
            QMessageBox::warning(this, tr("Theme Error"),
                tr("Failed to load theme: %1\nPlease try another theme.").arg(selectedTheme));
            return;
        }
        
        // Apply global stylesheet to the entire application
        QString stylesheet = themeMgr.toStyleSheet(theme);
        qApp->setStyleSheet(stylesheet);
        
        // Apply to all editor tabs
        for (int i = 0; i < editorTabs->count(); ++i) {
            CodeEditor *editor = qobject_cast<CodeEditor*>(editorTabs->widget(i));
            if (editor) {
                editor->setTheme(theme);
            }
        }
        
        // Apply to console (if it exists)
        if (console) {
            console->setTheme(theme);
        }
        
        // Apply to all terminal tabs
        for (int i = 0; i < consoleTabs->count(); ++i) {
            TerminalWidget *terminal = qobject_cast<TerminalWidget*>(consoleTabs->widget(i));
            if (terminal) {
                terminal->setTheme(theme);
            }
        }

        // Apply to the R help pane (restyle rendered docs).
        if (helpPane) {
            helpPane->setTheme(theme);
        }

        qDebug() << "Theme applied successfully:" << selectedTheme;
    }
}

void MainWindow::adjustAllTerminalFontSize(int delta)
{
    // delta = +1 (bigger), -1 (smaller), 0 (reset to default 11pt)
    if (delta == 0)
        m_globalFontSize = 11;
    else
        m_globalFontSize = qBound(6, m_globalFontSize + delta, 32);

    // ── Terminals (xterm.js) ──────────────────────────────────────────────
    // Use the absolute setter so every terminal lands on the *same* size.
    // Fall back to reset + delta for older terminal.html that lacks setFontSize.
    const int target = m_globalFontSize;
    const QString js = QString(
        "if (window.setFontSize) {"
        "  window.setFontSize(%1);"
        "} else if (window.resetFontSize && window.adjustFontSize) {"
        "  window.resetFontSize();"
        "  window.adjustFontSize(%1 - 11);"
        "}").arg(target);
    for (int i = 0; i < consoleTabs->count(); ++i) {
        if (auto *tw = qobject_cast<TerminalWidget*>(consoleTabs->widget(i)))
            tw->page()->runJavaScript(js);
    }

    // ── Script editors ────────────────────────────────────────────────────
    for (int i = 0; i < editorTabs->count(); ++i) {
        if (auto *ed = qobject_cast<CodeEditor*>(editorTabs->widget(i)))
            ed->setFontSize(m_globalFontSize);
    }

    // Persist immediately so the size survives across sessions.
    QSettings settings("RGUI2", "RGUI2");
    settings.setValue("globalFontSize", m_globalFontSize);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    event->accept();
}
