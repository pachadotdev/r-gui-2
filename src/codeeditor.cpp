#include "codeeditor.h"
#include "syntaxhighlighter.h"
#include <QPainter>
#include <QTextBlock>
#include <QFont>
#include <QFileInfo>

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
}

void CodeEditor::setLanguage(RSyntaxHighlighter::Language lang)
{
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
