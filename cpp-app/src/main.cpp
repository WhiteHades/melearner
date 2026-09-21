#include "main_window.hpp"
#include "single_instance.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSurfaceFormat>

int main(int argc, char** argv) {
  QSurfaceFormat format;
  format.setDepthBufferSize(0); format.setStencilBufferSize(0);
  QSurfaceFormat::setDefaultFormat(format);
  QApplication application(argc, argv);
  Q_INIT_RESOURCE(assets);
  QApplication::setOrganizationName("WhiteHades");
  QApplication::setApplicationName("melearner-cpp-v1");
  QApplication::setApplicationDisplayName("melearner");
  QApplication::setApplicationVersion(MELEARNER_VERSION);
  QApplication::setDesktopFileName("io.github.whitehades.melearner");
  QCommandLineParser arguments;
  arguments.setApplicationDescription("Study local courses with native video, documents, and lesson notes.");
  arguments.addOption({"software-decoding", "Disable hardware video decoding for troubleshooting or qualification."});
  arguments.addHelpOption(); arguments.addVersionOption(); arguments.process(application);
  const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (!QDir().mkpath(data)) {
    QMessageBox::critical(nullptr, "melearner", "Cannot create the C++ data directory. Check its permissions."); return 1;
  }
  SingleInstance instance(data);
  const auto acquired = instance.acquire();
  if (acquired == SingleInstance::Result::Forwarded) return 0;
  if (acquired == SingleInstance::Result::Error) { QMessageBox::critical(nullptr, "melearner", instance.error()); return 1; }
  MainWindow window(QDir(data).filePath("library-v1.sqlite3"), nullptr, arguments.isSet("software-decoding"));
  QObject::connect(&instance, &SingleInstance::activationRequested, &window, [&window] {
    if (window.isMinimized()) window.showNormal();
    window.raise(); window.activateWindow();
  });
  window.show();
  return application.exec();
}
