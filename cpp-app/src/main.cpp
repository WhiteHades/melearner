#include "main_window.hpp"
#include "single_instance.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <string_view>

namespace {

void configureCommandLineParser(QCommandLineParser& arguments) {
  arguments.setApplicationDescription("Study local courses with native video, documents, and progress tracking.");
  arguments.addOption({"software-decoding", "Disable hardware video decoding for troubleshooting or qualification."});
  arguments.addHelpOption();
  arguments.addVersionOption();
}

void configureApplicationIdentity() {
  QCoreApplication::setOrganizationName("WhiteHades");
  QCoreApplication::setApplicationName("melearner-cpp-v1");
  QCoreApplication::setApplicationVersion(MELEARNER_VERSION);
}

bool requestsInformationalOutput(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--version" || argument == "-v" || argument == "--help" ||
        argument == "-h" || argument == "--help-all") {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (requestsInformationalOutput(argc, argv)) {
    QCoreApplication application(argc, argv);
    configureApplicationIdentity();
    QCommandLineParser arguments;
    configureCommandLineParser(arguments);
    arguments.process(application);
    return 0;
  }

  QSurfaceFormat format;
  format.setDepthBufferSize(0); format.setStencilBufferSize(0);
  QSurfaceFormat::setDefaultFormat(format);
  QApplication application(argc, argv);
  configureApplicationIdentity();
  Q_INIT_RESOURCE(assets);
  QApplication::setApplicationDisplayName("melearner");
  QApplication::setDesktopFileName("io.github.whitehades.melearner");
  QCommandLineParser arguments;
  configureCommandLineParser(arguments);
  arguments.process(application);
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
