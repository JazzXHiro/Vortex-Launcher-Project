#include "vortex_bridge.h"
#include "vortex_log.h"
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

using namespace Qt::StringLiterals;

// Defined in ui/controller_support.cpp — publishes the gamepad to QML as the
// "controller" context property. A no-op when no XInput pad is present.
void installControllerSupport(QQmlApplicationEngine &engine);

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);

  // In a build tree the exe sits at out/build/<config>/VortexLauncher.exe while
  // the data files (local_game_dirs.txt, preferences.json, Images/, …) sit at
  // the project root three levels up; in a deployed build they sit beside the
  // exe. CMakeLists.txt in the presumed root is what tells the two apart.
  {
    QDir appDir(QCoreApplication::applicationDirPath());
    QDir devDir = appDir;
    devDir.cdUp(); // <config> → build
    devDir.cdUp(); // build    → out
    devDir.cdUp(); // out      → project root

    if (QFile::exists(devDir.absolutePath() + "/CMakeLists.txt")) {
        QDir::setCurrent(devDir.absolutePath());
    } else {
        QDir::setCurrent(appDir.absolutePath());
    }
  }

  // Open the log now that the working directory is settled, so the session
  // header is the first thing in the file and every fetch the scan performs
  // lands in it. Also switches stdout to unbuffered, which is what makes the
  // console usable as a live view.
  vlog::init();
  if (!vlog::file_path().empty())
    vlog::line("Vortex", "Logging to " + vlog::file_path());

  // The "Basic" style, so the custom Button backgrounds and contentItems in
  // GameDetails.qml work without warnings.
  QQuickStyle::setStyle("Basic");

  QQmlApplicationEngine engine;

  VortexBridge bridge;
  engine.rootContext()->setContextProperty("vortexApi", &bridge);

  // Must happen before load() so main.qml can bind to "controller" while it is
  // being created.
  installControllerSupport(engine);

  const QUrl url(u"qrc:/Vortex/ui/main.qml"_s);

  // Exit cleanly if QML fails to load
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
          QCoreApplication::exit(-1);
      },
      Qt::QueuedConnection);

  engine.load(url);

  return app.exec();
}
