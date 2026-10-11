#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QColor>
#include <QDir>

struct EditorTheme {
    QString name;
    QString author;
    QString variant;
    QColor background;
    QColor foreground;
    QColor cursor;
    QColor selection;
    QColor lineHighlight;
    QColor lineNumber;
    QColor lineNumberBg;
    
    // 16 ANSI colors
    QColor color_01, color_02, color_03, color_04;
    QColor color_05, color_06, color_07, color_08;
    QColor color_09, color_10, color_11, color_12;
    QColor color_13, color_14, color_15, color_16;
    
    // Syntax colors (derived from ANSI colors)
    QColor keyword;
    QColor function;
    QColor string;
    QColor number;
    QColor comment;
    QColor operator_;
};

class ThemeManager : public QObject
{
    Q_OBJECT

public:
    static ThemeManager& instance();
    
    QStringList availableThemes() const;
    QStringList availableMonospaceFonts() const;
    QString monospaceFont() const;
    void setMonospaceFont(const QString &family);
    EditorTheme getTheme(const QString &name) const;
    EditorTheme currentTheme() const;
    void setCurrentTheme(const QString &name);
    
    QString toStyleSheet(const EditorTheme &theme) const;
    void applyTheme(const EditorTheme &theme) const;
    
private:
    ThemeManager();
    void initializeThemes();
    void scanJsonThemes();
    QString findThemesDirectory() const;
    EditorTheme loadThemeFromJson(const QString &themeName) const;
    EditorTheme parseJsonTheme(const QJsonObject &obj) const;
    
    QMap<QString, EditorTheme> themes;
    QStringList jsonThemeNames;
    QString themesDir;
    QString currentThemeName;
    QString monospaceFontName = QStringLiteral("Hack");
};

#endif // THEMEMANAGER_H
