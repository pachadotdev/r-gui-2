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
class HelpPane;

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
    void adjustAllTerminalFontSize(int delta);  // +1 or -1; 0 = reset

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
    QDockWidget *helpDock;
    
    // Console tabs
    QTabWidget *consoleTabs;
    
    // Components
    TerminalWidget *console;
    FileBrowser *fileBrowser;
    EnvironmentPane *envPane;
    PlotPane *plotPane;
    HelpPane *helpPane;
    
    // Menus
    QMenu *fileMenu;
    QMenu *codeMenu;
    QMenu *viewMenu;
    QMenu *helpMenu;
    
    // Current file tracking
    QString currentFile;
    int m_globalFontSize = 11;  // shared font size for editors + terminals
    QString m_currentDir;         // last directory opened via Open Directory
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_leftSplitter = nullptr;
    
    CodeEditor* getCurrentEditor();
    void addNewEditorTab(const QString &title = "Untitled");
    void updateTabTitle(int index, bool modified);
};

#endif // MAINWINDOW_H
