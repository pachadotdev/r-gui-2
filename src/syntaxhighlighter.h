#ifndef RSYNTAXHIGHLIGHTER_H
#define RSYNTAXHIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTextCharFormat>
#include "thememanager.h"

class RSyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    enum class Language { R, CPP, Markdown, PlainText };

    explicit RSyntaxHighlighter(QTextDocument *parent = nullptr);
    void setTheme(const EditorTheme &theme);
    void setLanguage(Language lang);
    Language language() const { return m_language; }

protected:
    void highlightBlock(const QString &text) override;

private:
    void buildRRules(const EditorTheme &theme);
    void buildCPPRules(const EditorTheme &theme);
    void buildMarkdownRules(const EditorTheme &theme);

    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<HighlightingRule> highlightingRules;

    Language m_language = Language::R;
    EditorTheme m_theme;

    QTextCharFormat keywordFormat;
    QTextCharFormat functionFormat;
    QTextCharFormat commentFormat;
    QTextCharFormat stringFormat;
    QTextCharFormat numberFormat;
    QTextCharFormat operatorFormat;
    QTextCharFormat preprocessorFormat;
    QTextCharFormat headingFormat;
    QTextCharFormat boldFormat;
    QTextCharFormat italicFormat;

    // For C/C++ multi-line block comments
    QRegularExpression m_blockCommentStart;
    QRegularExpression m_blockCommentEnd;
};

#endif // RSYNTAXHIGHLIGHTER_H
