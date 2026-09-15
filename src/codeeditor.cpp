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
#include <QProcess>
#include <QHelpEvent>

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

void CodeEditor::setSuggestionsEnabled(bool enabled)
{
    m_suggestionsEnabled = enabled;
    if (!m_suggestionsEnabled && m_completer && m_completer->popup()) {
        m_completer->popup()->hide();
    }
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

QString CodeEditor::findRscriptBinary()
{
    static QString s_cachedPath;
    if (!s_cachedPath.isEmpty()) return s_cachedPath;

    QString rHome = QString::fromLocal8Bit(qgetenv("R_HOME"));
    if (!rHome.isEmpty()) {
#ifdef Q_OS_WIN
        QString p = rHome + "/bin/x64/Rscript.exe";
        if (!QFileInfo::exists(p)) p = rHome + "/bin/Rscript.exe";
#else
        QString p = rHome + "/bin/Rscript";
#endif
        if (QFileInfo::exists(p)) {
            s_cachedPath = p;
            return p;
        }
    }

    QString found = QStandardPaths::findExecutable("Rscript");
    if (!found.isEmpty()) {
        s_cachedPath = found;
        return found;
    }

#ifdef Q_OS_WIN
    for (const QString &dir : {"C:/Program Files/R", "C:/R"}) {
        QDir d(dir);
        if (d.exists()) {
            for (const QString &sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString candidate = d.filePath(sub) + "/bin/x64/Rscript.exe";
                if (QFileInfo::exists(candidate)) {
                    s_cachedPath = candidate;
                    return candidate;
                }
            }
        }
    }
#else
    for (const char *path : {"/usr/bin/Rscript", "/usr/local/bin/Rscript", "/Library/Frameworks/R.framework/Resources/bin/Rscript"}) {
        if (QFileInfo::exists(path)) {
            s_cachedPath = path;
            return path;
        }
    }
#endif

    s_cachedPath = "Rscript";
    return s_cachedPath;
}

void CodeEditor::fetchFunctionInfoFromR(const QString &funcName, const QString &pkg,
                                       QStringList &outArgs, QString &outCallTip)
{
    if (funcName.isEmpty()) return;

    QString rscript = findRscriptBinary();
    if (rscript.isEmpty()) return;

    QString script = QString(R"(
invisible(utils:::rc.settings())
fun <- "%1"
pkg <- "%2"
if (nzchar(pkg)) try(suppressPackageStartupMessages(library(pkg, character.only=TRUE)), silent=TRUE)
if (!nzchar(pkg)) {
  for (p in unique(c(loadedNamespaces(), "base", "stats", "graphics", "utils", "methods", "datasets", .packages(all.available=TRUE)))) {
    if (tryCatch(exists(fun, where=asNamespace(p), inherits=FALSE), error=function(e) FALSE)) {
      pkg <- p
      try(suppressPackageStartupMessages(library(pkg, character.only=TRUE)), silent=TRUE)
      break
    }
  }
}
res_args <- tryCatch(utils:::functionArgs(fun, ""), error=function(e) character())
if (length(res_args) == 0) {
  f <- tryCatch(utils::argsAnywhere(fun), error=function(e) NULL)
  if (!is.null(f)) {
    nms <- names(formals(f))
    res_args <- ifelse(nms == "...", "...", paste0(nms, "="))
  }
}

usage <- ""
h <- tryCatch(help(fun, package=if (nzchar(pkg)) pkg else NULL), error=function(e) NULL)
if (!is.null(h) && length(h) > 0) {
  try({
    db <- utils:::.getHelpFile(h)
    out <- capture.output(tools::Rd2txt(db, stages="render", options=list(underline_titles=FALSE)))
    u_idx <- grep("^Usage:", out)
    if (length(u_idx) > 0) {
      next_sec <- grep("^[A-Z][a-zA-Z ]*:", out)
      next_sec <- next_sec[next_sec > u_idx]
      end_idx <- if (length(next_sec) > 0) next_sec[1] - 1 else length(out)
      usage_lines <- out[(u_idx + 1):end_idx]
      usage <- paste(trimws(usage_lines), collapse="\n")
    }
  }, silent=TRUE)
}
if (!nzchar(usage)) {
  f <- tryCatch(utils::argsAnywhere(fun), error=function(e) NULL)
  if (!is.null(f)) {
    txt <- paste(deparse(args(f)), collapse=" ")
    txt <- sub("^function\\s*", paste0(fun), txt)
    txt <- sub("\\s*NULL$", "", txt)
    usage <- txt
  }
}

cat("===ARGS===\n")
cat(res_args, sep="\n")
cat("\n===USAGE===\n")
cat(trimws(usage))
)").arg(funcName, pkg);

    QProcess proc;
    proc.start(rscript, QStringList() << "--vanilla" << "-e" << script);
    if (proc.waitForFinished(1500)) {
        QString output = QString::fromUtf8(proc.readAllStandardOutput());
        int argsIdx = output.indexOf("===ARGS===");
        int usageIdx = output.indexOf("===USAGE===");
        if (argsIdx >= 0) {
            int endArgs = (usageIdx >= 0) ? usageIdx : output.length();
            QString argsSection = output.mid(argsIdx + 10, endArgs - (argsIdx + 10)).trimmed();
            if (!argsSection.isEmpty()) {
                const QStringList rawList = argsSection.split('\n', Qt::SkipEmptyParts);
                for (const QString &raw : rawList) {
                    QString a = raw.trimmed();
                    if (a.isEmpty()) continue;
                    if (a.endsWith('=')) {
                        a = a.left(a.length() - 1).trimmed() + " = ";
                    }
                    outArgs.append(a);
                }
            }
        }
        if (usageIdx >= 0) {
            outCallTip = output.mid(usageIdx + 11).trimmed();
        }
    }
}

static QMap<QString, QStringList> s_cachedArgNames;
static QMap<QString, QString> s_cachedCallTips;

void CodeEditor::ensureFunctionInfo(const QString &funcName, const QString &pkg)
{
    QString cacheKey = pkg.isEmpty() ? funcName : (pkg + "::" + funcName);
    if (s_cachedArgNames.contains(cacheKey) && s_cachedCallTips.contains(cacheKey)) {
        return;
    }

    QStringList args;
    QString callTip;
    fetchFunctionInfoFromR(funcName, pkg, args, callTip);

    s_cachedArgNames.insert(cacheKey, args);
    s_cachedCallTips.insert(cacheKey, callTip);
}

QString CodeEditor::getFunctionCallTip(const QString &funcName, const QString &pkg)
{
    QString cacheKey = pkg.isEmpty() ? funcName : (pkg + "::" + funcName);
    ensureFunctionInfo(funcName, pkg);
    QString rawTip = s_cachedCallTips.value(cacheKey);
    QStringList args = s_cachedArgNames.value(cacheKey);

    QString title = !pkg.isEmpty() ? (pkg + "::" + funcName) : funcName;

    if (!rawTip.isEmpty()) {
        return QString("<div style='font-family:monospace; font-size:11px; padding:2px;'><b>%1</b>\n%2</div>")
               .arg(title.toHtmlEscaped(), rawTip.trimmed().toHtmlEscaped());
    }
    if (!args.isEmpty()) {
        return QString("<div style='font-family:monospace; font-size:11px; padding:2px;'><b>%1</b>(%2)</div>")
               .arg(title.toHtmlEscaped(), args.join(", ").toHtmlEscaped());
    }
    return QString();
}

QStringList CodeEditor::getFunctionArgNames(const QString &funcName, const QString &pkg)
{
    QString cacheKey = pkg.isEmpty() ? funcName : (pkg + "::" + funcName);
    ensureFunctionInfo(funcName, pkg);
    return s_cachedArgNames.value(cacheKey);
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

void CodeEditor::updateCompleterModel(bool packageContextOnly, const QString &pkgScope,
                                       bool argContext, const QString &argFuncName)
{
    if (!m_completerModel)
        return;

    QSet<QString> items;

    if (argContext) {
        for (const QString &a : getFunctionArgNames(argFuncName, pkgScope)) {
            items.insert(a);
        }
    } else if (!pkgScope.isEmpty()) {
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

CodeEditor::FunctionCallContext CodeEditor::getFunctionCallContext(const QTextCursor &cursor)
{
    FunctionCallContext ctx;
    int globalPos = cursor.position();
    int startPos = qMax(0, globalPos - 2000);
    QTextCursor scanCursor = cursor;
    scanCursor.setPosition(startPos);
    scanCursor.setPosition(globalPos, QTextCursor::KeepAnchor);
    QString text = scanCursor.selectedText();
    text.replace(QChar(0x2029), '\n');

    int len = text.length();
    int depth = 0;
    int callOpenIdx = -1;

    // Scan backwards from cursor position
    for (int i = len - 1; i >= 0; --i) {
        QChar c = text.at(i);
        if (c == ')' || c == ']' || c == '}') {
            ++depth;
        } else if (c == '(' || c == '[' || c == '{') {
            if (depth > 0) {
                --depth;
            } else if (c == '(') {
                callOpenIdx = i;
                break;
            }
        }
    }

    if (callOpenIdx < 0) {
        return ctx;
    }

    // Look at identifier before callOpenIdx
    QString prefixBeforeParen = text.left(callOpenIdx).trimmed();
    static const QRegularExpression fnRegex(R"((?:([A-Za-z0-9._]+):::?)?([A-Za-z0-9._]+)$)");
    QRegularExpressionMatch fnMatch = fnRegex.match(prefixBeforeParen);
    if (!fnMatch.hasMatch()) {
        return ctx;
    }

    QString fn = fnMatch.captured(2);
    if (fn == "if" || fn == "while" || fn == "for" || fn == "function" || fn == "switch") {
        return ctx;
    }

    ctx.insideCall = true;
    ctx.pkgScope = fnMatch.captured(1);
    ctx.funcName = fn;

    // Text between '(' and cursor
    QString argsText = text.mid(callOpenIdx + 1);

    // Split by top-level commas within this call
    int argDepth = 0;
    int lastCommaIdx = -1;
    bool inQuote = false;
    QChar quoteChar;

    for (int i = 0; i < argsText.length(); ++i) {
        QChar c = argsText.at(i);
        if (inQuote) {
            if (c == quoteChar && (i == 0 || argsText.at(i - 1) != '\\')) {
                inQuote = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            inQuote = true;
            quoteChar = c;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++argDepth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (argDepth > 0) --argDepth;
        } else if (c == ',' && argDepth == 0) {
            lastCommaIdx = i;
        }
    }

    QString currentSeg = (lastCommaIdx >= 0) ? argsText.mid(lastCommaIdx + 1) : argsText;

    // Check if currentSeg contains '=' at top level
    bool hasEqual = false;
    int eqDepth = 0;
    bool eqInQuote = false;
    QChar eqQuoteChar;
    for (int i = 0; i < currentSeg.length(); ++i) {
        QChar c = currentSeg.at(i);
        if (eqInQuote) {
            if (c == eqQuoteChar && (i == 0 || currentSeg.at(i - 1) != '\\')) eqInQuote = false;
            continue;
        }
        if (c == '"' || c == '\'') { eqInQuote = true; eqQuoteChar = c; continue; }
        if (c == '(' || c == '[' || c == '{') ++eqDepth;
        else if (c == ')' || c == ']' || c == '}') { if (eqDepth > 0) --eqDepth; }
        else if (c == '=' && eqDepth == 0) { hasEqual = true; break; }
    }

    if (!hasEqual) {
        ctx.isArgNameContext = true;
        ctx.currentArgPrefix = currentSeg.trimmed();
    } else {
        ctx.isArgNameContext = false;
    }

    return ctx;
}

bool CodeEditor::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip && m_currentLanguage == RSyntaxHighlighter::Language::R) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        QTextCursor tc = cursorForPosition(helpEvent->pos());
        FunctionCallContext ctx = getFunctionCallContext(tc);
        QString funcName;
        QString pkgScope;
        if (ctx.insideCall) {
            funcName = ctx.funcName;
            pkgScope = ctx.pkgScope;
        } else {
            tc.select(QTextCursor::WordUnderCursor);
            QString word = tc.selectedText().trimmed();
            if (!word.isEmpty() && (word.at(0).isLetter() || word.at(0) == '.')) {
                funcName = word;
            }
        }

        if (!funcName.isEmpty() && funcName != "if" && funcName != "while" && funcName != "for" && funcName != "function" && funcName != "switch") {
            QString tip = getFunctionCallTip(funcName, pkgScope);
            if (!tip.isEmpty()) {
                // Show tooltip offset to the right side so it does not block the code line
                QRect cr = cursorRect(tc);
                QPoint globalPos = mapToGlobal(cr.topRight()) + QPoint(20, 0);
                if (globalPos.x() < helpEvent->globalPos().x()) {
                    globalPos = helpEvent->globalPos() + QPoint(20, 0);
                }
                QToolTip::showText(globalPos, tip, this);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
    return QPlainTextEdit::viewportEvent(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *e)
{
    if (m_completer && m_completer->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            e->ignore();
            return;
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Space:
            m_completer->popup()->hide();
            break;
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

    static const QString eow("~!@#$%^&*()_+{}|:\"<>?,/;'[]\\-= ");
    const bool hasModifier = (e->modifiers() != Qt::NoModifier) && !ctrlOrShift;

    if (!isShortcut) {
        QPlainTextEdit::keyPressEvent(e);
    }

    if (!m_completer || (!m_suggestionsEnabled && !isShortcut))
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
    bool isArgContext = false;
    QString argFuncName;

    if (nsMatch.hasMatch()) {
        pkgScope = nsMatch.captured(1);
        completionPrefix = nsMatch.captured(2);
    } else if (pkgMatch.hasMatch()) {
        isPkgContext = true;
        completionPrefix = pkgMatch.captured(1);
    } else if (m_currentLanguage == RSyntaxHighlighter::Language::R) {
        FunctionCallContext ctx = getFunctionCallContext(textCursor());
        if (ctx.insideCall && ctx.isArgNameContext) {
            isArgContext = true;
            argFuncName = ctx.funcName;
            pkgScope = ctx.pkgScope;
            completionPrefix = ctx.currentArgPrefix;
        } else {
            completionPrefix = textUnderCursor();
        }
    } else {
        completionPrefix = textUnderCursor();
    }

    const bool isNsContext = !pkgScope.isEmpty() && !isArgContext;

    if (!isShortcut && (hasModifier || e->text().isEmpty()
                        || (!isNsContext && !isArgContext && completionPrefix.length() < 1)
                        || (eow.contains(e->text().right(1)) && !isPkgContext && !isNsContext && !isArgContext && e->text().right(1) != "." && e->text().right(1) != "_"))) {
        m_completer->popup()->hide();
        return;
    }

    updateCompleterModel(isPkgContext, pkgScope, isArgContext, argFuncName);

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
