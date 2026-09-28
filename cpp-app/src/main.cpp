#include "main_window.hpp"
#include "single_instance.hpp"
#include "theme.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <string_view>

namespace {

void configureCommandLineParser(QCommandLineParser& arguments) {
  arguments.setApplicationDescription("Study local courses with native video, documents, and progress tracking.");
  arguments.addOption({"software-decoding", "Disable hardware video decoding for troubleshooting or qualification."});
  // A folder is accepted so the desktop entry can hand over a root the user
  // chose. It is optional: without one the application opens on the library it
  // already has, or asks for a root folder.
  arguments.addPositionalArgument("folder",
    QCoreApplication::translate("main", "Course root folder: the folder holding your Course folders."),
    QCoreApplication::translate("main", "[folder]"));
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
  // The shadcn style replaces the platform style, so it must own the theme and
  // the interface font before any widget is constructed. The colour mode starts
  // at the desktop preference and is corrected once the saved settings arrive.
  melearner::installAppearance(melearner::systemPrefersDark(), 14);
  QApplication::setApplicationDisplayName("melearner");
  QApplication::setDesktopFileName("io.github.whitehades.melearner");
  QCommandLineParser arguments;
  configureCommandLineParser(arguments);
  arguments.process(application);
  // These two failures happen before there is a window to own a dialog, so they
  // are reported on stderr instead. A message box would also be the one
  // interface surface left outside the component library.
  const auto fail = [](const QString& message) {
    QTextStream(stderr) << "melearner: " << message << Qt::endl;
    return 1;
  };
  const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (!QDir().mkpath(data)) {
    return fail(QStringLiteral("Cannot create the data directory. Check its permissions."));
  }
  SingleInstance instance(data);
  const auto acquired = instance.acquire();
  if (acquired == SingleInstance::Result::Forwarded) return 0;
  if (acquired == SingleInstance::Result::Error) return fail(instance.error());
  MainWindow window(QDir(data).filePath("library-v1.sqlite3"), nullptr, arguments.isSet("software-decoding"));
  QObject::connect(&instance, &SingleInstance::activationRequested, &window, [&window] {
    if (window.isMinimized()) window.showNormal();
    window.raise(); window.activateWindow();
  });
  window.show();
  // A folder handed over by the desktop entry becomes the course root. A file is
  // refused rather than guessed at: a library is built from the folders under a
  // root, and the folder holding a lesson file is a course or a section, never a
  // root, so any ancestor this picked would be a guess.
  const auto requested = arguments.positionalArguments();
  if (!requested.isEmpty()) {
    const auto given = QFileInfo(requested.first()).absoluteFilePath();
    if (QFileInfo(given).isDir()) {
      window.chooseRootWhenOpen(given);
    } else {
      return fail(QCoreApplication::translate("main",
        "Expected a course root folder, but '%1' is a file. Choose the folder that "
        "contains your Course folders.").arg(requested.first()));
    }
  }
  return application.exec();
}
