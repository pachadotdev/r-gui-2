#include "mainwindow.h"
#include <QApplication>
#include <QFontDatabase>
#include <QtLogging>

static QtMessageHandler previousHandler = nullptr;

// Qt's Wayland plugin warns whenever a combo box/menu grabs the mouse; it is
// harmless, so drop it.
static void filterMessages(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (msg.contains(QLatin1String("grabbing the mouse only for popup windows")))
        return;
    if (previousHandler)
        previousHandler(type, ctx, msg);
}

int main(int argc, char *argv[])
{
    // Must be set before QApplication is created to take effect. Suppresses
    // the portal registration warning (app not installed as a .desktop entry)
    // and KDE file system watcher noise.
    qputenv("QT_LOGGING_RULES",
            "qt.qpa.services=false;"
            "kf.kio.widgets.kdirmodel.debug=false;"
            "kf.jobwidgets.debug=false");
    previousHandler = qInstallMessageHandler(filterMessages);

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
    
    app.setApplicationName("Simpler");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("Simpler");
    app.setDesktopFileName("simpler");  // Without .desktop suffix
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
