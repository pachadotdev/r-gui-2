#include "syntaxhighlighter.h"

RSyntaxHighlighter::RSyntaxHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    setTheme(ThemeManager::instance().currentTheme());
}

void RSyntaxHighlighter::setLanguage(Language lang)
{
    m_language = lang;
    setTheme(m_theme);   // rebuild rules for the new language
}

void RSyntaxHighlighter::setTheme(const EditorTheme &theme)
{
    m_theme = theme;
    highlightingRules.clear();

    switch (m_language) {
    case Language::CPP:      buildCPPRules(theme);      break;
    case Language::Markdown: buildMarkdownRules(theme); break;
    default:                 buildRRules(theme);        break;
    }

    rehighlight();
}

// ── R ─────────────────────────────────────────────────────────────────────────

void RSyntaxHighlighter::buildRRules(const EditorTheme &theme)
{
    HighlightingRule rule;

    keywordFormat.setForeground(theme.keyword);
    keywordFormat.setFontWeight(QFont::Bold);
    for (const QString &kw : QStringList{
            "\\bif\\b", "\\belse\\b", "\\bfor\\b", "\\bwhile\\b", "\\brepeat\\b",
            "\\bfunction\\b", "\\breturn\\b", "\\bnext\\b", "\\bbreak\\b",
            "\\bTRUE\\b", "\\bFALSE\\b", "\\bNULL\\b", "\\bNA\\b", "\\bNaN\\b",
            "\\bInf\\b", "\\bin\\b"}) {
        rule.pattern = QRegularExpression(kw);
        rule.format  = keywordFormat;
        highlightingRules.append(rule);
    }

    functionFormat.setForeground(theme.function);
    rule.pattern = QRegularExpression("\\b[A-Za-z0-9_\\.]+(?=\\s*\\()");
    rule.format  = functionFormat;
    highlightingRules.append(rule);

    numberFormat.setForeground(theme.number);
    rule.pattern = QRegularExpression("\\b[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?\\b");
    rule.format  = numberFormat;
    highlightingRules.append(rule);

    operatorFormat.setForeground(theme.operator_);
    for (const QString &op : QStringList{
            "\\+", "-", "\\*", "/", "\\^", "%%", "%/%",
            "==", "!=", "<=", ">=", "<", ">",
            "&&", "\\|\\|", "\\&", "\\|", "!",
            "<-", "<<-", "->>", "->", "=", "~", "\\|>"}) {
        rule.pattern = QRegularExpression(op);
        rule.format  = operatorFormat;
        highlightingRules.append(rule);
    }

    stringFormat.setForeground(theme.string);
    rule.pattern = QRegularExpression("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\"");
    rule.format  = stringFormat;
    highlightingRules.append(rule);
    rule.pattern = QRegularExpression("'[^'\\\\]*(\\\\.[^'\\\\]*)*'");
    highlightingRules.append(rule);

    commentFormat.setForeground(theme.comment);
    commentFormat.setFontItalic(true);
    rule.pattern = QRegularExpression("#[^\n]*");
    rule.format  = commentFormat;
    highlightingRules.append(rule);
}

// ── C / C++ ───────────────────────────────────────────────────────────────────

void RSyntaxHighlighter::buildCPPRules(const EditorTheme &theme)
{
    HighlightingRule rule;

    // Preprocessor directives
    preprocessorFormat.setForeground(theme.keyword);
    preprocessorFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("^\\s*#\\s*\\w+");
    rule.format  = preprocessorFormat;
    highlightingRules.append(rule);

    // Keywords
    keywordFormat.setForeground(theme.keyword);
    keywordFormat.setFontWeight(QFont::Bold);
    for (const QString &kw : QStringList{
            "\\bauto\\b",     "\\bbool\\b",      "\\bbreak\\b",
            "\\bcase\\b",     "\\bchar\\b",       "\\bclass\\b",
            "\\bconst\\b",    "\\bconstexpr\\b",  "\\bcontinue\\b",
            "\\bdefault\\b",  "\\bdelete\\b",     "\\bdo\\b",
            "\\bdouble\\b",   "\\belse\\b",       "\\benum\\b",
            "\\bexplicit\\b", "\\bextern\\b",     "\\bfalsе\\b",
            "\\bfloat\\b",    "\\bfor\\b",        "\\bfriend\\b",
            "\\bgoto\\b",     "\\bif\\b",         "\\binline\\b",
            "\\bint\\b",      "\\blong\\b",       "\\bmutable\\b",
            "\\bnamespace\\b","\\bnew\\b",        "\\bnullptr\\b",
            "\\boperator\\b", "\\boverride\\b",   "\\bprivate\\b",
            "\\bprotected\\b","\\bpublic\\b",     "\\breturn\\b",
            "\\bshort\\b",    "\\bsizeof\\b",     "\\bstatic\\b",
            "\\bstruct\\b",   "\\bswitch\\b",     "\\btemplate\\b",
            "\\bthis\\b",     "\\bthrow\\b",      "\\btrue\\b",
            "\\btypedef\\b",  "\\btypename\\b",   "\\bunion\\b",
            "\\bunsigned\\b", "\\busing\\b",      "\\bvirtual\\b",
            "\\bvoid\\b",     "\\bvolatile\\b",   "\\bwhile\\b",
            "\\bsize_t\\b",   "\\bstd\\b"}) {
        rule.pattern = QRegularExpression(kw);
        rule.format  = keywordFormat;
        highlightingRules.append(rule);
    }

    // Functions
    functionFormat.setForeground(theme.function);
    rule.pattern = QRegularExpression("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()");
    rule.format  = functionFormat;
    highlightingRules.append(rule);

    // Numbers (decimal, hex, octal, float)
    numberFormat.setForeground(theme.number);
    rule.pattern = QRegularExpression(
        "\\b(0[xX][0-9A-Fa-f]+|0[0-7]*|[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?)[uUlLfF]*\\b");
    rule.format  = numberFormat;
    highlightingRules.append(rule);

    // Operators
    operatorFormat.setForeground(theme.operator_);
    rule.pattern = QRegularExpression(
        "\\+\\+|--|->|::|[+\\-*/%&|^~<>=!]=?|[<>]{2}=?|&&|\\|\\|");
    rule.format  = operatorFormat;
    highlightingRules.append(rule);

    // Strings
    stringFormat.setForeground(theme.string);
    rule.pattern = QRegularExpression("\"([^\"\\\\]|\\\\.)*\"");
    rule.format  = stringFormat;
    highlightingRules.append(rule);
    // Character literals
    rule.pattern = QRegularExpression("'([^'\\\\]|\\\\.)'");
    highlightingRules.append(rule);

    // Line comments (//) - added last so they override everything above
    commentFormat.setForeground(theme.comment);
    commentFormat.setFontItalic(true);
    rule.pattern = QRegularExpression("//[^\n]*");
    rule.format  = commentFormat;
    highlightingRules.append(rule);

    // Block comment delimiters (multi-line handling in highlightBlock)
    m_blockCommentStart = QRegularExpression("/\\*");
    m_blockCommentEnd   = QRegularExpression("\\*/");
}

// ── Markdown ──────────────────────────────────────────────────────────────────

void RSyntaxHighlighter::buildMarkdownRules(const EditorTheme &theme)
{
    HighlightingRule rule;

    // Headings  (#, ##, …)
    headingFormat.setForeground(theme.keyword);
    headingFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("^#{1,6}[^\\n]*");
    rule.format  = headingFormat;
    highlightingRules.append(rule);

    // Bold  **text** or __text__
    boldFormat.setForeground(theme.function);
    boldFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("(\\*\\*|__).+?(\\*\\*|__)");
    rule.format  = boldFormat;
    highlightingRules.append(rule);

    // Italic  *text* or _text_
    italicFormat.setForeground(theme.function);
    italicFormat.setFontItalic(true);
    rule.pattern = QRegularExpression("(\\*|_)(?!\\1).+?(\\*|_)");
    rule.format  = italicFormat;
    highlightingRules.append(rule);

    // Inline code  `code`
    stringFormat.setForeground(theme.string);
    rule.pattern = QRegularExpression("`[^`]+`");
    rule.format  = stringFormat;
    highlightingRules.append(rule);

    // Links and images  [text](url)  ![alt](url)
    operatorFormat.setForeground(theme.operator_);
    rule.pattern = QRegularExpression("!?\\[[^\\]]*\\]\\([^)]*\\)");
    rule.format  = operatorFormat;
    highlightingRules.append(rule);

    // Blockquote / horizontal rule
    commentFormat.setForeground(theme.comment);
    commentFormat.setFontItalic(true);
    rule.pattern = QRegularExpression("^>.*");
    rule.format  = commentFormat;
    highlightingRules.append(rule);

    // HTML comments <!-- ... -->  (single-line approximation)
    rule.pattern = QRegularExpression("<!--.*-->");
    highlightingRules.append(rule);

    // List markers
    numberFormat.setForeground(theme.number);
    rule.pattern = QRegularExpression("^\\s*([-*+]|\\d+\\.)\\s");
    rule.format  = numberFormat;
    highlightingRules.append(rule);
}

// ── highlightBlock ────────────────────────────────────────────────────────────

void RSyntaxHighlighter::highlightBlock(const QString &text)
{
    // Apply all single-line rules first
    for (const HighlightingRule &rule : highlightingRules) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    // C/C++ multi-line block comments (/* ... */)
    if (m_language == Language::CPP) {
        setCurrentBlockState(0);
        int start = 0;
        if (previousBlockState() != 1)
            start = m_blockCommentStart.match(text).capturedStart();

        while (start >= 0) {
            QRegularExpressionMatch endMatch = m_blockCommentEnd.match(text, start);
            int end = endMatch.capturedStart();
            int len;
            if (end == -1) {
                setCurrentBlockState(1);
                len = text.length() - start;
            } else {
                len = end - start + endMatch.capturedLength();
            }
            setFormat(start, len, commentFormat);
            start = m_blockCommentStart.match(text, start + len).capturedStart();
        }
    }
}

