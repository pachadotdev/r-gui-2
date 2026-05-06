#include "terminalwidget.h"
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTextStream>
#include <QRegularExpression>

TerminalWidget::TerminalWidget(const QString &shell, QWidget *parent)
    : QTermWidget(0, parent)
    , shellPath(shell)
{
    // Get current theme
    currentTheme = ThemeManager::instance().currentTheme();
    
    // Set font to Hack Nerd Font Mono for Powerline/icon support
    // Note: qtermwidget may show a "variable-width font" warning due to how it checks
    // font metrics with Nerd Fonts' extra glyphs, but the font works correctly
    QFont font;
    font.setFamily("Hack Nerd Font Mono");
    font.setPointSize(10);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    setTerminalFont(font);
    
    // Set terminal size hint
    setTerminalSizeHint(false);
    
    // Enable scroll on output
    setScrollBarPosition(QTermWidget::ScrollBarRight);
    
    // Set color scheme based on theme
    setTheme(currentTheme);
    
    // Setup copy/paste shortcuts using QShortcut (more reliable than keyPressEvent)
    copyShortcut = new QShortcut(QKeySequence("Ctrl+Shift+C"), this);
    connect(copyShortcut, &QShortcut::activated, this, &QTermWidget::copyClipboard);
    
    pasteShortcut = new QShortcut(QKeySequence("Ctrl+Shift+V"), this);
    connect(pasteShortcut, &QShortcut::activated, this, &QTermWidget::pasteClipboard);
    
    // Set shell if provided
    if (!shell.isEmpty()) {
        shellPath = shell;
        setShellProgram(shell);
        
        // Check if this is R and set appropriate arguments
        QString shellName = QFileInfo(shell).fileName().toLower();
        if (shellName == "r") {
            QStringList rArgs;
            rArgs << "--interactive" << "--no-save";
            QTermWidget::setArgs(rArgs);
        }
    } else {
        // Use system default shell
        QString defaultShell = qgetenv("SHELL");
        if (defaultShell.isEmpty()) {
            defaultShell = "/bin/bash";
        }
        shellPath = defaultShell;
        setShellProgram(defaultShell);
    }
    
    // Set environment variables for proper UTF-8 support
    QProcessEnvironment sysEnv = QProcessEnvironment::systemEnvironment();
    QStringList env;
    
    // Convert system environment to string list, skipping locale vars we'll override
    for (const QString &key : sysEnv.keys()) {
        if (key != "LANG" && key != "LC_ALL" && key != "TERM" && key != "R_PROFILE_USER") {
            env << QString("%1=%2").arg(key, sysEnv.value(key));
        }
    }
    
    // Add UTF-8 locale settings
    env << "LANG=en_US.UTF-8";
    env << "LC_ALL=en_US.UTF-8";
    env << "TERM=xterm-256color";

    // Tell R where to write plot snapshots (must match the path PlotPane watches)
    QString qPlotDir = QDir::tempPath() + "/q_plots";
    env << "Q_PLOT_DIR=" + qPlotDir;
    
    // Setup R profile for silent loading
    QString shellName = QFileInfo(shellPath).fileName().toLower();
    if (shellName == "r") {
        QString initScriptPath = QDir::tempPath() + "/q_init_" + QString::number(QCoreApplication::applicationPid()) + ".R";
        QFile initScript(initScriptPath);
        if (initScript.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&initScript);
            out << "local({\n";
            out << "  orig_prof <- Sys.getenv('Q_ORIGINAL_R_PROFILE_USER')\n";
            out << "  if (nzchar(orig_prof) && file.exists(orig_prof)) {\n";
            out << "    source(orig_prof)\n";
            out << "  } else {\n";
            out << "    if (file.exists('.Rprofile')) source('.Rprofile')\n";
            out << "    else if (file.exists(file.path(Sys.getenv('HOME'), '.Rprofile'))) source(file.path(Sys.getenv('HOME'), '.Rprofile'))\n";
            out << "  }\n";
            out << "  if (requireNamespace('qide', quietly=TRUE)) {\n";
            out << "    library(qide)\n";
            out << "    qide::init_monitor('/tmp/q_env.json')\n";
            out << "  }\n";
            out << "\n";
            out << "  # ── Q plot capture ────────────────────────────────────────────────────\n";
            out << "  local({\n";
            out << "    plot_dir <- Sys.getenv('Q_PLOT_DIR', unset = file.path(tempdir(), 'q_plots'))\n";
            out << "    dir.create(plot_dir, showWarnings = FALSE, recursive = TRUE)\n";
            out << "    index_file <- file.path(plot_dir, 'q_plot_index.txt')\n";
            out << "    dev_counter  <- 0L\n";
            out << "    snap_counter <- 0L\n";
            out << "    last_snap_size <- -1L\n";
            out << "\n";
            out << "    # Override the default device so plots never open an X11 window.\n";
            out << "    # width/height arrive in inches; convert to pixels at 96 dpi.\n";
            out << "    # Enable the display list so recordPlot() can capture the state later.\n";
            out << "    options(device = function(width = 7, height = 5, ...) {\n";
            out << "      dev_counter <<- dev_counter + 1L\n";
            out << "      fname <- file.path(plot_dir, sprintf('q_dev_%06d.png', dev_counter))\n";
            out << "      grDevices::png(fname,\n";
            out << "                     width  = round(width  * 96),\n";
            out << "                     height = round(height * 96),\n";
            out << "                     res    = 96, ...)\n";
            out << "      grDevices::dev.control(displaylist = 'enable')\n";
            out << "      invisible(NULL)\n";
            out << "    })\n";
            out << "\n";
            out << "    # After every top-level expression: snapshot if a device is open.\n";
            out << "    # We always write q_current.png, then compare its file size against\n";
            out << "    # the previous snapshot to detect actual plot changes.  This avoids\n";
            out << "    # the display-list-length false-negative (two different plots with the\n";
            out << "    # same number of primitives) that plagued the old approach.\n";
            out << "    addTaskCallback(function(expr, value, ok, visible) {\n";
            out << "      if (grDevices::dev.cur() != 1L) {\n";
            out << "        tryCatch({\n";
            out << "          recorded <- grDevices::recordPlot()\n";
            out << "          if (length(recorded[[1]]) > 0L) {\n";
            out << "            curr_file <- file.path(plot_dir, 'q_current.png')\n";
            out << "            grDevices::png(curr_file, width=800L, height=600L, res=96L)\n";
            out << "            grDevices::replayPlot(recorded)\n";
            out << "            grDevices::dev.off()\n";
            out << "            new_size <- file.size(curr_file)\n";
            out << "            if (!identical(new_size, last_snap_size)) {\n";
            out << "              last_snap_size <<- new_size\n";
            out << "              snap_counter <<- snap_counter + 1L\n";
            out << "              snap_file <- file.path(plot_dir,\n";
            out << "                                     sprintf('q_snap_%06d.png', snap_counter))\n";
            out << "              file.copy(curr_file, snap_file, overwrite = TRUE)\n";
            out << "              writeLines(snap_file, index_file)\n";
            out << "            }\n";
            out << "          }\n";
            out << "        }, error = function(e) NULL)\n";
            out << "      }\n";
            out << "      TRUE\n";
            out << "    }, name = 'q_plot_capture')\n";
            out << "  })\n";
            out << "})\n";
            initScript.close();
            
            QString originalProfile = sysEnv.value("R_PROFILE_USER");
            if (!originalProfile.isEmpty()) {
                env << "Q_ORIGINAL_R_PROFILE_USER=" + originalProfile;
            }
            env << "R_PROFILE_USER=" + initScriptPath;
        }
    }
    
    setEnvironment(env);
    
    // Set working directory to user's home
    setWorkingDirectory(QDir::homePath());
    
    // Set key bindings (default should work, but we can customize if needed)
    setKeyBindings("default");
    
    // Disable bracketed paste mode to avoid ^[[200~ sequences when pasting
    disableBracketedPasteMode(true);
    
    // Start the shell
    startShellProgram();
}

TerminalWidget::~TerminalWidget()
{
    // QTermWidget handles cleanup
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);
    
    // Copy action
    QAction *copyAction = menu->addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence("Ctrl+Shift+C"));
    copyAction->setEnabled(selectedText().length() > 0);
    connect(copyAction, &QAction::triggered, this, &QTermWidget::copyClipboard);
    
    // Paste action
    QAction *pasteAction = menu->addAction(tr("Paste"));
    pasteAction->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(pasteAction, &QAction::triggered, this, &QTermWidget::pasteClipboard);
    
    menu->addSeparator();
    
    // Clear action
    QAction *clearAction = menu->addAction(tr("Clear"));
    connect(clearAction, &QAction::triggered, this, [this]() {
        QString shellName = QFileInfo(shellPath).fileName().toLower();
        if (shellName == "r") {
            executeCommand("clear()");
        } else {
            executeCommand("clear");
        }
    });
    
    menu->exec(event->globalPos());
    delete menu;
}

void TerminalWidget::setTheme(const EditorTheme &theme)
{
    currentTheme = theme;
    
    // Use built-in schemes based on theme brightness
    // Don't try to create custom schemes - just use what's available
    int brightness = theme.background.red() + theme.background.green() + theme.background.blue();
    
    if (brightness < 384) {
        // Dark or medium theme - use WhiteOnBlack
        setColorScheme("WhiteOnBlack");
    } else {
        // Light theme - use BlackOnWhite
        setColorScheme("BlackOnWhite");
    }
}

void TerminalWidget::setArgs(const QStringList &args)
{
    QTermWidget::setArgs(args);
}

void TerminalWidget::writeToShell(const QString &text)
{
    sendText(text);
}

void TerminalWidget::executeCommand(const QString &command)
{
    sendText(command + "\n");
}



