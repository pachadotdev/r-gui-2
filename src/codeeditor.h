#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <QPlainTextEdit>
#include <QObject>
#include <QCompleter>
#include <QStringListModel>
#include "thememanager.h"
#include "syntaxhighlighter.h"

class QPaintEvent;
class QResizeEvent;
class QKeyEvent;
class QFocusEvent;
class QEvent;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);
    ~CodeEditor() override;
    
    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth();
    void setTheme(const EditorTheme &theme);
    void setFontSize(int pt);
    void setLanguage(RSyntaxHighlighter::Language lang);
    void setLanguageFromFile(const QString &filePath);

    void setCompleter(QCompleter *completer);
    QCompleter *completer() const { return m_completer; }

    void setSuggestionsEnabled(bool enabled);
    bool suggestionsEnabled() const { return m_suggestionsEnabled; }

    struct FunctionCallContext {
        bool insideCall = false;
        QString funcName;
        QString pkgScope;
        QString currentArgPrefix;
        bool isArgNameContext = false;
    };

    static FunctionCallContext getFunctionCallContext(const QTextCursor &cursor);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    bool viewportEvent(QEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect &rect, int dy);
    void insertCompletion(const QString &completion);

private:
    void setupCompleter();
    void updateCompleterModel(bool packageContextOnly, const QString &pkgScope = QString(),
                               bool argContext = false, const QString &argFuncName = QString());
    void updateCompleterStyle();
    QString textUnderCursor() const;
    static QString findRscriptBinary();
    static void ensureFunctionInfo(const QString &funcName, const QString &pkg);
    static bool fetchFunctionInfoFromR(const QString &funcName, const QString &pkg,
                                       QStringList &outArgs, QString &outCallTip);
    static QStringList getInstalledRPackages();
    static QStringList getPackageExports(const QString &packageName);
    static QString getFunctionCallTip(const QString &funcName, const QString &pkg = QString());
    static QStringList getFunctionArgNames(const QString &funcName, const QString &pkg = QString());
    QSet<QString> getLoadedPackages() const;
    QStringList getSessionVariables() const;
    QStringList getDocumentWords() const;
    QStringList getBuiltinRCompletions() const;
    QStringList getBuiltinCPPCompletions() const;

    QWidget *lineNumberArea;
    RSyntaxHighlighter *highlighter;
    EditorTheme currentTheme;
    int m_fontSize = 12;

    QCompleter *m_completer = nullptr;
    QStringListModel *m_completerModel = nullptr;
    RSyntaxHighlighter::Language m_currentLanguage = RSyntaxHighlighter::Language::R;
    bool m_suggestionsEnabled = true;
};

// Line number area widget
class LineNumberArea : public QWidget
{
public:
    LineNumberArea(CodeEditor *editor) : QWidget(editor), codeEditor(editor) {}

    QSize sizeHint() const override {
        return QSize(codeEditor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        codeEditor->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditor *codeEditor;
};

#endif // CODEEDITOR_H
