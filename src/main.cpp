// FrameStudio (called Video Compressor up to version 2.4) - Qt app to edit videos and export them at a chosen
// quality or size, with a preview of the final file size.
//
// The estimate is made by actually encoding a few short samples of the video with the chosen settings,
// so it is reliable even with very different kinds of content (screen recordings, camera footage,
// animations...). Encoding is done by FFmpeg, bundled with the app.

#include "mainwindow.h"

#include <QApplication>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Andrea Galet");   // where QSettings keeps the preferences (see settings.h)
    QApplication::setApplicationName("FrameStudio");
    QApplication::setApplicationVersion(APP_VERSION);

    // the first time: the preferences of Video Compressor, as the app was called before
    if (QSettings settings; settings.allKeys().isEmpty()) {
        const QSettings old("Andrea Galet", "Video Compressor");
        for (const QString &key : old.allKeys())
            settings.setValue(key, old.value(key));
    }

    MainWindow window;
    window.show();
    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.setInput(args.at(1));
    return QApplication::exec();
}
