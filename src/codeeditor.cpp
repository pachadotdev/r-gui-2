#include "codeeditor.h"
#include "syntaxhighlighter.h"
#include "r_builtins.h"
#include <QPainter>
#include <QTextBlock>
#include <QFont>
#include <QFileInfo>
#include <QCompleter>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QToolTip>

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    lineNumberArea = new LineNumberArea(this);
    
    // Get current theme
    currentTheme = ThemeManager::instance().currentTheme();
    
    // Set font - try Hack first, fallback to monospace
    QFont font;
    QStringList fonts = {"Hack", "Noto Sans Mono", "Courier New", "Monospace"};
    for (const QString &fontName : fonts) {
        font = QFont(fontName, 12);
        if (QFontInfo(font).family() == fontName) {
            break;
        }
    }
    font.setStyleHint(QFont::TypeWriter);
    setFont(font);
    
    // Set tab width (4 spaces)
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    
    // Apply theme colors
    setStyleSheet(ThemeManager::instance().toStyleSheet(currentTheme));
    
    // Create syntax highlighter with theme
    highlighter = new RSyntaxHighlighter(document());
    highlighter->setTheme(currentTheme);

    // Setup autocompleter
    setupCompleter();
    
    // Connect signals
    connect(this, &CodeEditor::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);
    
    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::setupCompleter()
{
    m_completerModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completerModel, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setWrapAround(false);

    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &CodeEditor::insertCompletion);

    updateCompleterStyle();
}

void CodeEditor::setCompleter(QCompleter *completer)
{
    if (m_completer)
        m_completer->disconnect(this);

    m_completer = completer;

    if (!m_completer)
        return;

    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &CodeEditor::insertCompletion);
    updateCompleterStyle();
}

void CodeEditor::updateCompleterStyle()
{
    if (!m_completer || !m_completer->popup())
        return;

    QAbstractItemView *popup = m_completer->popup();
    popup->setFont(font());

    QString bg = currentTheme.background.name();
    QString fg = currentTheme.foreground.name();
    QString selBg = currentTheme.selection.name();
    QString lineHi = currentTheme.lineHighlight.name();

    QString style = QString(
        "QAbstractItemView {"
        "  background-color: %1;"
        "  color: %2;"
        "  selection-background-color: %3;"
        "  selection-color: %2;"
        "  border: 1px solid %4;"
        "  border-radius: 4px;"
        "  padding: 2px;"
        "}"
        "QAbstractItemView::item {"
        "  height: %5px;"
        "  padding: 2px 6px;"
        "}"
    ).arg(bg, fg, selBg, lineHi, QString::number(fontMetrics().height() + 4));

    popup->setStyleSheet(style);
}

QStringList CodeEditor::getInstalledRPackages()
{
    static QStringList s_cachedPackages;
    static qint64 s_lastScanTime = 0;
    qint64 now = QDateTime::currentSecsSinceEpoch();

    if (!s_cachedPackages.isEmpty() && (now - s_lastScanTime < 30)) {
        return s_cachedPackages;
    }

    QSet<QString> pkgSet;

    auto scanDir = [&pkgSet](const QString &dirPath) {
        QDir dir(dirPath);
        if (!dir.exists()) return;
        const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            QString path = dir.filePath(entry);
            if (QFile::exists(path + "/DESCRIPTION")) {
                pkgSet.insert(entry);
            }
        }
    };

    auto scanRDir = [&pkgSet, &scanDir](const QString &basePath) {
        QDir base(basePath);
        if (!base.exists()) return;
        scanDir(basePath);
        const QStringList subdirs = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subdirs) {
            QString subPath = base.filePath(sub);
            scanDir(subPath);
            QDir subDir(subPath);
            const QStringList subSubDirs = subDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &v : subSubDirs) {
                scanDir(subDir.filePath(v));
            }
        }
    };

    for (const char *envVar : {"R_LIBS_USER", "R_LIBS", "R_LIBS_SITE"}) {
        QString val = QString::fromLocal8Bit(qgetenv(envVar));
        if (!val.isEmpty()) {
#ifdef Q_OS_WIN
            const QStringList paths = val.split(';', Qt::SkipEmptyParts);
#else
            const QStringList paths = val.split(':', Qt::SkipEmptyParts);
#endif
            for (const QString &p : paths) {
                scanRDir(p);
            }
        }
    }

    scanDir("/usr/lib/R/library");
    scanDir("/usr/lib64/R/library");
    scanDir("/usr/local/lib/R/site-library");

    scanRDir(QDir::homePath() + "/R");
    scanRDir(QDir::homePath() + "/.R/library");
    scanRDir(QDir::homePath() + "/Library/R");
    scanDir("/Library/Frameworks/R.framework/Resources/library");

#ifdef Q_OS_WIN
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    scanRDir(docPath + "/R/win-library");
#endif

    s_cachedPackages = pkgSet.values();
    s_cachedPackages.sort(Qt::CaseInsensitive);
    s_lastScanTime = now;
    return s_cachedPackages;
}

QStringList CodeEditor::getPackageExports(const QString &packageName)
{
    static QMap<QString, QStringList> s_cachedExports;
    if (s_cachedExports.contains(packageName)) {
        return s_cachedExports.value(packageName);
    }

    QList<QString> libDirs;
    for (const char *envVar : {"R_LIBS_USER", "R_LIBS", "R_LIBS_SITE"}) {
        QString val = QString::fromLocal8Bit(qgetenv(envVar));
        if (!val.isEmpty()) {
#ifdef Q_OS_WIN
            const QStringList paths = val.split(';', Qt::SkipEmptyParts);
#else
            const QStringList paths = val.split(':', Qt::SkipEmptyParts);
#endif
            libDirs.append(paths);
        }
    }

    libDirs.append("/usr/lib/R/library");
    libDirs.append("/usr/lib64/R/library");
    libDirs.append("/usr/local/lib/R/site-library");
    libDirs.append("/Library/Frameworks/R.framework/Resources/library");

    // Also check standard subdirectories under ~/R
    QDir homeR(QDir::homePath() + "/R");
    if (homeR.exists()) {
        for (const QString &sub : homeR.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString subPath = homeR.filePath(sub);
            libDirs.append(subPath);
            QDir subDir(subPath);
            for (const QString &v : subDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                libDirs.append(subDir.filePath(v));
            }
        }
    }

#ifdef Q_OS_WIN
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QDir winLib(docPath + "/R/win-library");
    if (winLib.exists()) {
        for (const QString &sub : winLib.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            libDirs.append(winLib.filePath(sub));
        }
    }
#endif

    QString namespacePath;
    for (const QString &dirPath : libDirs) {
        QString candidate = dirPath + "/" + packageName + "/NAMESPACE";
        if (QFile::exists(candidate)) {
            namespacePath = candidate;
            break;
        }
    }

    QSet<QString> exports;

    if (!namespacePath.isEmpty()) {
        QFile file(namespacePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString content = QString::fromUtf8(file.readAll());
            file.close();

            // Match export(a, b, c, ...) and exportPattern("...")
            // S3method(generic, class) -> generic, generic.class
            static const QRegularExpression exportRegex(R"(export\s*\(([^)]+)\))");
            QRegularExpressionMatchIterator it = exportRegex.globalMatch(content);
            while (it.hasNext()) {
                QRegularExpressionMatch match = it.next();
                QString raw = match.captured(1);
                const QStringList symbols = raw.split(',', Qt::SkipEmptyParts);
                for (QString sym : symbols) {
                    sym = sym.trimmed().remove('\"').remove('\'');
                    if (!sym.isEmpty() && (sym.at(0).isLetter() || sym.at(0) == '.')) {
                        exports.insert(sym);
                    }
                }
            }

            static const QRegularExpression s3Regex(R"(S3method\s*\(\s*([^,\s]+)\s*,\s*([^)\s]+)\s*\))");
            QRegularExpressionMatchIterator s3It = s3Regex.globalMatch(content);
            while (s3It.hasNext()) {
                QRegularExpressionMatch match = s3It.next();
                QString generic = match.captured(1).trimmed().remove('\"').remove('\'');
                QString cls = match.captured(2).trimmed().remove('\"').remove('\'');
                if (!generic.isEmpty()) {
                    exports.insert(generic);
                    if (!cls.isEmpty()) {
                        exports.insert(generic + "." + cls);
                    }
                }
            }
        }
    }

    // Also look for lazy-load datasets if pkg/data/ exists or R/sysdata.rda
    for (const QString &dirPath : libDirs) {
        QString dataDir = dirPath + "/" + packageName + "/data";
        QDir dDir(dataDir);
        if (dDir.exists()) {
            const QStringList dataFiles = dDir.entryList(QDir::Files);
            for (const QString &df : dataFiles) {
                QString name = QFileInfo(df).completeBaseName();
                if (!name.isEmpty() && (name.at(0).isLetter() || name.at(0) == '.')) {
                    exports.insert(name);
                }
            }
            break;
        }
    }

    QStringList result = exports.values();
    result.sort(Qt::CaseInsensitive);
    s_cachedExports.insert(packageName, result);
    return result;
}

QString CodeEditor::getFunctionCallTip(const QString &funcName, const QString &pkg)
{
    static QMap<QString, QString> s_cachedCallTips;
    QString cacheKey = pkg.isEmpty() ? funcName : (pkg + "::" + funcName);
    if (s_cachedCallTips.contains(cacheKey)) {
        return s_cachedCallTips.value(cacheKey);
    }

    QList<QString> candidatePkgs;
    if (!pkg.isEmpty()) {
        candidatePkgs.append(pkg);
    } else {
        candidatePkgs.append("base");
        candidatePkgs.append("stats");
        candidatePkgs.append("graphics");
        candidatePkgs.append("utils");
        candidatePkgs.append("methods");
        candidatePkgs.append("datasets");
        for (const QString &p : getInstalledRPackages()) {
            if (!candidatePkgs.contains(p)) {
                candidatePkgs.append(p);
            }
        }
    }

    QList<QString> libDirs;
    for (const char *envVar : {"R_LIBS_USER", "R_LIBS", "R_LIBS_SITE"}) {
        QString val = QString::fromLocal8Bit(qgetenv(envVar));
        if (!val.isEmpty()) {
#ifdef Q_OS_WIN
            const QStringList paths = val.split(';', Qt::SkipEmptyParts);
#else
            const QStringList paths = val.split(':', Qt::SkipEmptyParts);
#endif
            libDirs.append(paths);
        }
    }
    libDirs.append("/usr/lib/R/library");
    libDirs.append("/usr/lib64/R/library");
    libDirs.append("/usr/local/lib/R/site-library");
    libDirs.append("/Library/Frameworks/R.framework/Resources/library");

    QDir homeR(QDir::homePath() + "/R");
    if (homeR.exists()) {
        for (const QString &sub : homeR.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString subPath = homeR.filePath(sub);
            libDirs.append(subPath);
            QDir subDir(subPath);
            for (const QString &v : subDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                libDirs.append(subDir.filePath(v));
            }
        }
    }

    QString usageStr;
    QString exampleStr;

    for (const QString &p : candidatePkgs) {
        for (const QString &lib : libDirs) {
            QString htmlPath = lib + "/" + p + "/html/" + funcName + ".html";
            if (!QFile::exists(htmlPath)) {
                // Try 00Index.html or help page alias
                continue;
            }

            QFile f(htmlPath);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString html = QString::fromUtf8(f.readAll());
                f.close();

                // Extract Usage section: <h3>Usage</h3>\s*<pre[^>]*>(.*?)</pre>
                static const QRegularExpression usageRegex(R"(<h3>Usage</h3>\s*<pre[^>]*>(.*?)</pre>)", QRegularExpression::DotMatchesEverythingOption);
                QRegularExpressionMatch uMatch = usageRegex.match(html);
                if (uMatch.hasMatch()) {
                    usageStr = uMatch.captured(1).trimmed();
                    // Clean HTML tags and entities
                    usageStr.remove(QRegularExpression("<[^>]*>"));
                    usageStr.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&#39;", "'").replace("&quot;", "\"");
                    // Take the relevant function signature line if multiple
                    QStringList uLines = usageStr.split('\n', Qt::SkipEmptyParts);
                    for (const QString &ul : uLines) {
                        QString trimmed = ul.trimmed();
                        if (trimmed.startsWith(funcName + "(") || trimmed.startsWith(funcName + " (")) {
                            usageStr = trimmed;
                            break;
                        }
                    }
                    if (!uLines.isEmpty() && !usageStr.startsWith(funcName)) {
                        usageStr = uLines.first().trimmed();
                    }
                }

                // Extract first meaningful example line from: <h3>Examples</h3>\s*<pre[^>]*>(.*?)</pre>
                static const QRegularExpression exRegex(R"(<h3>Examples</h3>\s*<pre[^>]*>(.*?)</pre>)", QRegularExpression::DotMatchesEverythingOption);
                QRegularExpressionMatch exMatch = exRegex.match(html);
                if (exMatch.hasMatch()) {
                    QString rawEx = exMatch.captured(1);
                    rawEx.remove(QRegularExpression("<[^>]*>"));
                    rawEx.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&#39;", "'").replace("&quot;", "\"");
                    const QStringList exLines = rawEx.split('\n');
                    for (const QString &el : exLines) {
                        QString trimmed = el.trimmed();
                        if (trimmed.startsWith("#") || trimmed.isEmpty() || trimmed.startsWith("##")) continue;
                        if (trimmed.contains(funcName + "(")) {
                            exampleStr = trimmed;
                            break;
                        }
                    }
                }
            }
            if (!usageStr.isEmpty() || !exampleStr.isEmpty()) break;
        }
        if (!usageStr.isEmpty() || !exampleStr.isEmpty()) break;
    }

    QString tip;
    if (!usageStr.isEmpty()) {
        tip += "<b>" + usageStr.toHtmlEscaped() + "</b>";
    }
    if (!exampleStr.isEmpty()) {
        if (!tip.isEmpty()) tip += "<br>";
        tip += "<span style='color:#888;'><i>e.g. </i></span><code>" + exampleStr.toHtmlEscaped() + "</code>";
    }

    if (!tip.isEmpty()) {
        s_cachedCallTips.insert(cacheKey, tip);
    }
    return tip;
}

QStringList CodeEditor::getSessionVariables() const
{
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString envFilePath = QDir(tempDir).filePath("rgui2_env.json");

    QFile file(envFilePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return {};

    QJsonObject root = doc.object();
    QJsonArray objects = root["objects"].toArray();
    QStringList varNames;
    varNames.reserve(objects.size());
    for (const auto &val : objects) {
        varNames.append(val.toString());
    }
    return varNames;
}

QStringList CodeEditor::getDocumentWords() const
{
    QString text = toPlainText();
    static const QRegularExpression wordRegex(QStringLiteral(R"(\b[A-Za-z.][A-Za-z0-9_.]*\b)"));
    QRegularExpressionMatchIterator it = wordRegex.globalMatch(text);
    QSet<QString> words;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString word = match.captured(0);
        if (word.length() >= 2) {
            words.insert(word);
        }
    }
    return words.values();
}

QStringList CodeEditor::getBuiltinRCompletions() const
{
    return getStandardRBuiltins();
}

QStringList CodeEditor::getBuiltinCPPCompletions() const
{
    static const QStringList s_builtinCPP = {
        "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
        "continue", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
        "operator", "private", "protected", "public", "reinterpret_cast", "return", "short",
        "signed", "sizeof", "static", "static_cast", "struct", "switch", "template", "this",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "while",
        "std", "vector", "string", "map", "unordered_map", "set", "unordered_set",
        "pair", "tuple", "unique_ptr", "shared_ptr", "make_unique", "make_shared",
        "cout", "cin", "cerr", "endl", "include", "define", "ifdef", "ifndef", "endif"
    };
    return s_builtinCPP;
}

QSet<QString> CodeEditor::getLoadedPackages() const
{
    QSet<QString> pkgs;

    // 1. Scan document for library(...) and require(...) calls
    QString text = toPlainText();
    static const QRegularExpression loadPkgRegex(
        QStringLiteral(R"((?:library|require)\s*\(\s*["']?([A-Za-z0-9._]+)["']?\s*(?:,[^)]*)?\))")
    );
    QRegularExpressionMatchIterator it = loadPkgRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString pkg = match.captured(1).trimmed();
        if (!pkg.isEmpty()) {
            pkgs.insert(pkg);
        }
    }

    // 2. Check rgui2_env.json for session packages if saved
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString envFilePath = QDir(tempDir).filePath("rgui2_env.json");
    QFile file(envFilePath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            for (const char *key : {"packages", "loaded_packages", "namespaces", "search"}) {
                if (root.contains(key) && root[key].isArray()) {
                    for (const auto &v : root[key].toArray()) {
                        QString p = v.toString();
                        if (p.startsWith("package:")) p = p.mid(8);
                        if (!p.isEmpty()) pkgs.insert(p);
                    }
                }
            }
        }
    }

    return pkgs;
}

void CodeEditor::updateCompleterModel(bool packageContextOnly, const QString &pkgScope)
{
    if (!m_completerModel)
        return;

    QSet<QString> items;

    if (!pkgScope.isEmpty()) {
        for (const QString &fn : getPackageExports(pkgScope)) {
            items.insert(fn);
        }
    } else if (packageContextOnly) {
        for (const QString &pkg : getInstalledRPackages()) {
            items.insert(pkg);
        }
    } else if (m_currentLanguage == RSyntaxHighlighter::Language::CPP) {
        for (const QString &w : getBuiltinCPPCompletions()) items.insert(w);
        for (const QString &w : getDocumentWords()) items.insert(w);
    } else {
        for (const QString &w : getBuiltinRCompletions()) items.insert(w);
        for (const QString &w : getSessionVariables()) items.insert(w);
        for (const QString &w : getDocumentWords()) items.insert(w);
        for (const QString &pkg : getLoadedPackages()) {
            for (const QString &exportedSym : getPackageExports(pkg)) {
                items.insert(exportedSym);
            }
        }
        for (const QString &pkg : getInstalledRPackages()) items.insert(pkg);
    }

    QStringList list = items.values();
    list.sort(Qt::CaseInsensitive);
    m_completerModel->setStringList(list);
}

QString CodeEditor::textUnderCursor() const
{
    QTextCursor tc = textCursor();
    QString linePrefix = tc.block().text().left(tc.positionInBlock());
    int i = linePrefix.length() - 1;
    while (i >= 0) {
        QChar ch = linePrefix.at(i);
        if (ch.isLetterOrNumber() || ch == '_' || ch == '.') {
            --i;
        } else {
            break;
        }
    }
    return linePrefix.mid(i + 1);
}

void CodeEditor::insertCompletion(const QString &completion)
{
    if (!m_completer || m_completer->widget() != this)
        return;

    QTextCursor tc = textCursor();
    QString prefix = m_completer->completionPrefix();

    if (!prefix.isEmpty() && tc.position() >= prefix.length()) {
        tc.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, prefix.length());
    }
    tc.insertText(completion);
    setTextCursor(tc);
}

void CodeEditor::checkAndShowCallTip(const QString &linePrefix)
{
    // Detect if cursor is directly after ( or inside arguments of funcName( or pkg::funcName(
    static const QRegularExpression callRegex(R"((?:([A-Za-z0-9._]+):::?)?([A-Za-z0-9._]+)\s*\([^()]*$)");
    QRegularExpressionMatch match = callRegex.match(linePrefix);
    if (!match.hasMatch()) {
        QToolTip::hideText();
        return;
    }

    QString pkg = match.captured(1);
    QString funcName = match.captured(2);

    if (funcName == "if" || funcName == "while" || funcName == "for" || funcName == "function") {
        QToolTip::hideText();
        return;
    }

    QString tip = getFunctionCallTip(funcName, pkg);
    if (!tip.isEmpty()) {
        QPoint pos = mapToGlobal(cursorRect().bottomLeft());
        pos.setY(pos.y() + 4);
        QToolTip::showText(pos, tip, this);
    } else {
        QToolTip::hideText();
    }
}

void CodeEditor::keyPressEvent(QKeyEvent *e)
{
    if (m_completer && m_completer->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            e->ignore();
            return;
        case Qt::Key_Escape:
            m_completer->popup()->hide();
            e->accept();
            return;
        default:
            break;
        }
    }

    const bool isShortcut = (e->modifiers().testFlag(Qt::ControlModifier) && e->key() == Qt::Key_Space);
    if (!m_completer && !isShortcut) {
        QPlainTextEdit::keyPressEvent(e);
        return;
    }

    const bool ctrlOrShift = e->modifiers().testFlag(Qt::ControlModifier) ||
                             e->modifiers().testFlag(Qt::ShiftModifier);
    if (!isShortcut && (ctrlOrShift && e->text().isEmpty())) {
        QPlainTextEdit::keyPressEvent(e);
        return;
    }

    static const QString eow("~!@#$%^&*()_+{}|:\"<>?,/;'[]\\-=");
    const bool hasModifier = (e->modifiers() != Qt::NoModifier) && !ctrlOrShift;

    if (!isShortcut) {
        QPlainTextEdit::keyPressEvent(e);
    }

    if (!m_completer)
        return;

    QString linePrefix = textCursor().block().text().left(textCursor().positionInBlock());

    // Check if inside pkg::... or pkg:::...
    static const QRegularExpression nsRegex(R"(([A-Za-z0-9._]+):::?([A-Za-z0-9._]*)$)");
    QRegularExpressionMatch nsMatch = nsRegex.match(linePrefix);

    // Check if inside library(...) or require(...) where only package names should be suggested
    static const QRegularExpression pkgRegex(R"((?:library|require)\s*\(\s*["']?([A-Za-z0-9._]*)$)");
    QRegularExpressionMatch pkgMatch = pkgRegex.match(linePrefix);

    QString completionPrefix;
    QString pkgScope;
    bool isPkgContext = false;

    if (nsMatch.hasMatch()) {
        pkgScope = nsMatch.captured(1);
        completionPrefix = nsMatch.captured(2);
    } else if (pkgMatch.hasMatch()) {
        isPkgContext = true;
        completionPrefix = pkgMatch.captured(1);
    } else {
        completionPrefix = textUnderCursor();
    }

    const bool isNsContext = !pkgScope.isEmpty();

    if (!isShortcut && (hasModifier || e->text().isEmpty()
                        || (!isNsContext && completionPrefix.length() < 1)
                        || (eow.contains(e->text().right(1)) && !isPkgContext && !isNsContext && e->text().right(1) != "." && e->text().right(1) != "_"))) {
        m_completer->popup()->hide();
        return;
    }

    updateCompleterModel(isPkgContext, pkgScope);

    if (completionPrefix != m_completer->completionPrefix()) {
        m_completer->setCompletionPrefix(completionPrefix);
        m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
    }

    if (m_completer->completionCount() > 0) {
        QRect cr = cursorRect();
        cr.setWidth(m_completer->popup()->sizeHintForColumn(0)
                    + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 20);
        m_completer->complete(cr);
    } else {
        m_completer->popup()->hide();
    }

    if (m_currentLanguage == RSyntaxHighlighter::Language::R) {
        checkAndShowCallTip(linePrefix);
    }
}

void CodeEditor::focusInEvent(QFocusEvent *event)
{
    if (m_completer)
        m_completer->setWidget(this);
    QPlainTextEdit::focusInEvent(event);
}

int CodeEditor::lineNumberAreaWidth()
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    
    int space = 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void CodeEditor::updateLineNumberAreaWidth(int /* newBlockCount */)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        lineNumberArea->scroll(0, dy);
    else
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    
    QRect cr = contentsRect();
    lineNumberArea->setGeometry(QRect(cr.left(), cr.top(),
                                      lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;
    
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        
        selection.format.setBackground(currentTheme.lineHighlight);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }
    
    setExtraSelections(extraSelections);
}

void CodeEditor::setFontSize(int pt)
{
    m_fontSize = pt;
    QFont f = font();
    f.setPointSize(pt);
    setFont(f);
    // Defensive: a per-widget stylesheet wins over the app-wide one,
    // so even if a theme stylesheet sets font-size we keep ours.
    setStyleSheet(styleSheet() + QString(" QPlainTextEdit { font-size: %1pt; }").arg(pt));
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    updateLineNumberAreaWidth(0);
    updateCompleterStyle();
}

void CodeEditor::setLanguage(RSyntaxHighlighter::Language lang)
{
    m_currentLanguage = lang;
    if (highlighter)
        highlighter->setLanguage(lang);
}

void CodeEditor::setLanguageFromFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    RSyntaxHighlighter::Language lang = RSyntaxHighlighter::Language::PlainText;
    if (suffix == "r" || suffix == "rmd" || suffix == "qmd" || suffix == "rproject")
        lang = RSyntaxHighlighter::Language::R;
    else if (suffix == "cpp" || suffix == "hpp" || suffix == "cc" || suffix == "cxx"
             || suffix == "c" || suffix == "h")
        lang = RSyntaxHighlighter::Language::CPP;
    else if (suffix == "md" || suffix == "markdown" || suffix == "rmd" || suffix == "qmd")
        lang = RSyntaxHighlighter::Language::Markdown;
    setLanguage(lang);
}

void CodeEditor::setTheme(const EditorTheme &theme)
{
    currentTheme = theme;
    setStyleSheet(ThemeManager::instance().toStyleSheet(theme));
    // Re-apply the current font size so the theme stylesheet can't clobber it.
    setFontSize(m_fontSize);
    if (highlighter) {
        highlighter->setTheme(theme);
        highlighter->rehighlight();
    }
    updateCompleterStyle();
    highlightCurrentLine();
    viewport()->update();
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), currentTheme.lineNumberBg);
    
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QString number = QString::number(blockNumber + 1);
            painter.setPen(currentTheme.lineNumber);
            painter.drawText(0, top, lineNumberArea->width() - 5, fontMetrics().height(),
                           Qt::AlignRight, number);
        }
        
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
