#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDockWidget>
#include <QTabWidget>
#include <QEvent>
#include <QResizeEvent>
#include <QMenuBar>
#include <QStatusBar>
#include <QPushButton>

class QSplitter;

class CodeEditor;
class FileBrowser;
class TerminalWidget;
class EnvironmentPane;
class PlotPane;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void newFile();
    void openFile();
    void openDirectory();
    void createProject();
    void saveFile();
    void saveFileAs();
    void runCurrentLine();
    void runSelection();
    void runAll();
    void sourceFile();
    void changeTheme();
    void about();

private:
    void createMenus();
    void createDockWidgets();
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void setDefaultLayoutSizes();
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    // Central widget
    QTabWidget *editorTabs;
    QDockWidget *scriptDock;
    
    // Dock widgets
    QDockWidget *consoleDock;
    QDockWidget *filesDock;
    QDockWidget *envDock;
    QDockWidget *plotDock;
    
    // Console tabs
    QTabWidget *consoleTabs;
    
    // Components
    TerminalWidget *console;
    FileBrowser *fileBrowser;
    EnvironmentPane *envPane;
    PlotPane *plotPane;
    
    // Menus
    QMenu *fileMenu;
    QMenu *codeMenu;
    QMenu *viewMenu;
    QMenu *helpMenu;
    
    // Current file tracking
    QString currentFile;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_leftSplitter = nullptr;
    
    CodeEditor* getCurrentEditor();
    void addNewEditorTab(const QString &title = "Untitled");
    void updateTabTitle(int index, bool modified);
};

#endif // MAINWINDOW_H
