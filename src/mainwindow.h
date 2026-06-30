#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QMenuBar>
#include <QStatusBar>
#include <QSplitter>

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
    void adjustAllTerminalFontSize(int delta);  // +1 / -1 / 0 = reset

private:
    void createMenus();
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void closeEvent(QCloseEvent *event) override;

    // Editor tabs (center-top)
    QTabWidget *editorTabs  = nullptr;

    // Console tabs (center-bottom)
    QTabWidget *consoleTabs = nullptr;

    // Right-panel tab widget (Help / Environment / Plots)
    QTabWidget *m_rightTabs = nullptr;

    // Splitters
    QSplitter  *m_outerSplitter  = nullptr;  // horizontal: files | editor+console | right
    QSplitter  *m_centerSplitter = nullptr;  // vertical:   editorTabs | consoleTabs

    // Components
    TerminalWidget  *console     = nullptr;
    FileBrowser     *fileBrowser = nullptr;
    EnvironmentPane *envPane     = nullptr;
    PlotPane        *plotPane    = nullptr;
    HelpPane        *helpPane    = nullptr;

    // Menus
    QMenu *fileMenu = nullptr;
    QMenu *codeMenu = nullptr;
    QMenu *viewMenu = nullptr;
    QMenu *helpMenu = nullptr;

    QString currentFile;
    int     m_globalFontSize = 12;  // shared font size for editors + terminals
    QString m_currentDir;           // last directory opened via Open Directory

    CodeEditor *getCurrentEditor();
    void addNewEditorTab(const QString &title = "Untitled");
    void updateTabTitle(int index, bool modified);
};

#endif // MAINWINDOW_H
