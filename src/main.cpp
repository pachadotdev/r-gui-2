#include "mainwindow.h"
#include <QApplication>
#include <QFontDatabase>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Load bundled Hack fonts from QRC resources so both the code editor
    // (Qt widgets) and the terminal always use a readable
    // monospace font regardless of what is installed system-wide.
    for (const char *res : {
            ":/fonts/hack-regular.ttf",
            ":/fonts/hack-bold.ttf",
            ":/fonts/hack-italic.ttf",
            ":/fonts/hack-bolditalic.ttf"
         }) {
        QFontDatabase::addApplicationFont(res);
    }
    
    app.setApplicationName("RGUI2");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("RGUI2");
    app.setDesktopFileName("q");  // Without .desktop suffix
    
    // Suppress portal registration warning (app not installed as a .desktop entry)
    // and KDE file system watcher noise.
    qputenv("QT_LOGGING_RULES",
            "qt.qpa.services=false;"
            "kf.kio.widgets.kdirmodel.debug=false;"
            "kf.jobwidgets.debug=false");
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
