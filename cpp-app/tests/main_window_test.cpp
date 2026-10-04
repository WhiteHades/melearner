#include "main_window.hpp"
#include "pdf_view.hpp"
#include "search_dialog.hpp"
#include "theme.hpp"

#include <QDir>
#include <QAction>
#include <QAccessible>
#include <QComboBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QTcpServer>
#include <QBuffer>
#include <QImage>
#include <QEventLoop>
#include <memory>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLayout>
#include <QHelpEvent>
#include <QToolTip>
#include <QMenu>
#include <QPainter>
#include <QPdfWriter>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTreeView>
#include <QWheelEvent>
#include <sqlite3.h>
#include <shadcn/widgets.hpp>
#include <shadcn/data.hpp>
#include <shadcn/overlays.hpp>
#include <QtTest>
#include "course_document_view.hpp"
#include <QWebEngineProfile>
#include <zip.h>

namespace {
class ScopedTestSettings final {
public:
  explicit ScopedTestSettings(const QString& path)
      : organization_(QCoreApplication::organizationName()),
        application_(QCoreApplication::applicationName()),
        format_(QSettings::defaultFormat()) {
    QSettings originalIniSettings(QSettings::IniFormat, QSettings::UserScope,
                                  organization_, application_);
    QDir originalIniRoot = QFileInfo(originalIniSettings.fileName()).absoluteDir();
    while ((!application_.isEmpty() && originalIniRoot.dirName() == application_)
        || (!organization_.isEmpty() && originalIniRoot.dirName() == organization_)) {
      if (!originalIniRoot.cdUp()) break;
    }
    iniPath_ = originalIniRoot.absolutePath();
    QCoreApplication::setOrganizationName(QStringLiteral("melearner-scroll-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("main-window"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
    QSettings settings;
    settings.clear();
    settings.sync();
  }
  ~ScopedTestSettings() {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, iniPath_);
    QCoreApplication::setOrganizationName(organization_);
    QCoreApplication::setApplicationName(application_);
    QSettings::setDefaultFormat(format_);
  }
private:
  const QString organization_;
  const QString application_;
  const QSettings::Format format_;
  QString iniPath_;
};

QVariant browserValue(QWebEngineView* view, const QString& script) {
  if (!view) return {};
  auto result = std::make_shared<QVariant>();
  QEventLoop loop;
  const QPointer<QEventLoop> waiting(&loop);
  view->page()->runJavaScript(script, [result, waiting](const QVariant& value) {
    *result = value;
    if (waiting) waiting->quit();
  });
  QTimer::singleShot(1500, &loop, &QEventLoop::quit);
  loop.exec();
  return *result;
}
}

class MainWindowTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); melearner::installTheme(true, 14); }

  void automaticUpdatePreferenceDefaultsOnAndPersistsOptOut() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    ScopedTestSettings isolatedSettings(files.path());
    QSettings preferences;
    preferences.setValue("updates/lastCheck", QDateTime::currentSecsSinceEpoch());
    preferences.sync();

    const auto database = files.path() + "/library.sqlite3";
    {
      MainWindow window(database);
      window.show();
      auto* settings = window.findChild<QPushButton*>("appearance");
      QVERIFY(settings && settings->menu());
      auto* automatic = settings->menu()->findChild<QAction*>("automaticUpdates");
      QVERIFY(automatic);
      QVERIFY(automatic->isCheckable());
      QVERIFY(automatic->isChecked());

      automatic->setChecked(false);
      preferences.sync();
      QVERIFY(!preferences.value("updates/automatic", true).toBool());
    }

    {
      MainWindow reopened(database);
      reopened.show();
      auto* settings = reopened.findChild<QPushButton*>("appearance");
      QVERIFY(settings && settings->menu());
      auto* automatic = settings->menu()->findChild<QAction*>("automaticUpdates");
      QVERIFY(automatic);
      QVERIFY(!automatic->isChecked());
      QVERIFY(!preferences.value("updates/automatic", true).toBool());
    }
  }

  void courseEntryDatabaseFailureRevealsCanvas() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    const auto section = root + "/Broken Course/01 Section";
    QVERIFY(QDir().mkpath(section));
    QFile lesson(section + "/01 Lesson.txt");
    QVERIFY(lesson.open(QIODevice::WriteOnly));
    QCOMPARE(lesson.write("A lesson"), qint64(8));
    lesson.close();

    const auto database = files.path() + "/library.sqlite3";
    MainWindow window(database);
    window.show();
    auto* choose = window.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY(choose && choose->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QVERIFY(courses);
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    QTRY_VERIFY(choose->isEnabled());

    sqlite3* rawDatabase = nullptr;
    const QByteArray databasePath = QFile::encodeName(database);
    QCOMPARE(sqlite3_open(databasePath.constData(), &rawDatabase), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> databaseHandle(rawDatabase, sqlite3_close);
    char* rawError = nullptr;
    const int dropResult = sqlite3_exec(rawDatabase, "DROP TABLE lessons", nullptr, nullptr, &rawError);
    const QByteArray dropError = rawError ? QByteArray(rawError) : QByteArray();
    sqlite3_free(rawError);
    QVERIFY2(dropResult == SQLITE_OK, dropError.constData());

    const auto index = courses->model()->index(0, 0);
    QTRY_VERIFY(courses->visualRect(index).isValid() && !courses->visualRect(index).isEmpty());
    const auto rowRect = courses->visualRect(index);
    QTest::mouseClick(courses->viewport(), Qt::LeftButton, Qt::NoModifier, rowRect.center());

    auto* status = window.findChild<QWidget*>("appStatus");
    auto* reveal = window.findChild<QWidget*>("courseCanvasReveal");
    QVERIFY(status && reveal);
    QTRY_VERIFY(status->isVisible());
    QTRY_VERIFY(reveal->property("revealProgress").toDouble() < 1.0);
    QTRY_VERIFY_WITH_TIMEOUT(reveal->property("revealProgress").toDouble() >= 1.0, 1000);
  }

  void nonVideoReaderCanvasesClipCornersAfterResizeAndSwitch() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    const auto section = root + "/Canvas Course/01 Section";
    QVERIFY(QDir().mkpath(section));
    QFile html(section + "/01 Browser.html");
    QVERIFY(html.open(QIODevice::WriteOnly));
    const QByteArray htmlContents(
        "<!doctype html><html><head><style>html,body{margin:0;min-height:100%;background:#19e63c}body{min-height:1600px}</style>"
        "</head><body>Bright browser canvas</body></html>");
    QCOMPARE(html.write(htmlContents), qint64(htmlContents.size()));
    html.close();
    {
      QPdfWriter writer(section + "/02 PDF.pdf");
      writer.setResolution(72);
      QPainter painter(&writer);
      painter.fillRect(QRect(0, 0, writer.width(), writer.height()), QColor(25, 90, 240));
      painter.drawText(60, 100, "Bright PDF canvas");
    }
    QFile prose(section + "/03 Prose.txt");
    QVERIFY(prose.open(QIODevice::WriteOnly));
    QCOMPARE(prose.write("Bright prose canvas"), qint64(19));
    prose.close();

    MainWindow window(files.path() + "/library.sqlite3");
    window.resize(1000, 760);
    window.show();
    auto* choose = window.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY(choose && choose->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    const auto courseIndex = courses->model()->index(0, 0);
    QTRY_VERIFY(courses->visualRect(courseIndex).isValid());
    QTest::mouseClick(courses->viewport(), Qt::LeftButton, Qt::NoModifier,
                      courses->visualRect(courseIndex).center());
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTRY_COMPARE(lessons->model()->rowCount(), 1);
    const auto sectionIndex = lessons->model()->index(0, 0);
    lessons->expand(sectionIndex);
    QTRY_COMPARE(lessons->model()->rowCount(sectionIndex), 3);

    const QColor appBackground = melearner::roleColor(&window, shadcn::Role::Background);
    const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    const auto capture = [&window, &captures](const QString& name) {
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/" + name + ".png"));
    };
    const auto sampleWindow = [&window](QWidget* surface, QPoint localPoint) {
      const QImage image = window.grab().toImage();
      const QPoint windowPoint = surface->mapTo(&window, localPoint);
      const qreal ratio = image.devicePixelRatio();
      return image.pixelColor(qRound(windowPoint.x() * ratio), qRound(windowPoint.y() * ratio));
    };
    const auto checkRoundedSurface = [&](QWidget* surface, QColor bright, QPoint brightPoint) {
      QTRY_VERIFY(surface && surface->isVisible());
      QTRY_VERIFY(sampleWindow(surface, brightPoint) == bright);
      QCOMPARE(sampleWindow(surface, QPoint(1, 1)).rgba(), appBackground.rgba());
    };
    const auto openLesson = [&window, lessons](int row) {
      if (!lessons->isVisible()) {
        auto* toggle = window.findChild<QPushButton*>("toggleOutline");
        QVERIFY(toggle && toggle->isVisible());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(lessons->isVisible());
      }
      const auto index = lessons->model()->index(row, 0, lessons->model()->index(0, 0));
      QVERIFY(index.isValid());
      lessons->scrollTo(index);
      QTRY_VERIFY(lessons->visualRect(index).isValid());
      lessons->setCurrentIndex(index);
      QTest::keyClick(lessons, Qt::Key_Return);
    };

    openLesson(0);
    QWebEngineView* browser = nullptr;
    QTRY_VERIFY((browser = window.findChild<QWebEngineView*>("documentBrowser")) && browser->isVisible());
    QTRY_COMPARE(browserValue(browser, "document.body && getComputedStyle(document.body).backgroundColor").toString(),
                 QString("rgb(25, 230, 60)"));
    checkRoundedSurface(browser, QColor(25, 230, 60), browser->rect().center());
    capture("reader-browser");
    browser->page()->runJavaScript("window.scrollTo(0,300)");
    QTRY_COMPARE(browserValue(browser, "window.scrollY").toInt(), 300);
    browser->page()->runJavaScript("window.scrollTo(0,0)");
    QTRY_COMPARE(browserValue(browser, "window.scrollY").toInt(), 0);

    window.resize(560, 720);
    QTRY_VERIFY(window.width() <= 560);
    checkRoundedSurface(browser, QColor(25, 230, 60), browser->rect().center());
    capture("reader-browser-compact");

    openLesson(1);
    auto* pdf = window.findChild<PdfView*>();
    QVERIFY(pdf);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto pdfPalette = pdf->palette();
    pdfPalette.setColor(QPalette::Window, QColor(245, 120, 20));
    pdf->setPalette(pdfPalette);
    pdf->viewport()->setPalette(pdfPalette);
    pdf->viewport()->update();
    checkRoundedSurface(pdf->viewport(), QColor(25, 90, 240), pdf->viewport()->rect().center());
    capture("reader-pdf");

    openLesson(2);
    auto* document = window.findChild<QTextEdit*>("documentText");
    QVERIFY(document);
    QTRY_VERIFY(document->isVisible());
    QCOMPARE(document->document()->documentMargin(), melearner::themeFor(document).radius() * 1.4);
    document->viewport()->setObjectName("proseTestViewport");
    document->viewport()->setStyleSheet("QWidget#proseTestViewport { background:#f019d2; }");
    checkRoundedSurface(document->viewport(), QColor(240, 25, 210), document->viewport()->rect().center());
    capture("reader-prose");
  }

  void nativeReadersAndLibraryPresentation() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    const auto section = root + "/Native Readers/Section";
    QVERIFY(QDir().mkpath(section));
    QVERIFY(QDir().mkpath(section + "/.assets"));
    QTcpServer offlineProbe; QVERIFY(offlineProbe.listen(QHostAddress::LocalHost));
    const auto probeUrl = QString("http://127.0.0.1:%1/probe").arg(offlineProbe.serverPort());
    QFile style(section + "/.assets/layout.css"); QVERIFY(style.open(QIODevice::WriteOnly));
    QVERIFY(style.write("#layout{display:grid;grid-template-columns:120px 1fr}#layout strong{color:rgb(12,34,56)}") > 0); style.close();
    QImage localImage(8, 8, QImage::Format_RGB32); localImage.fill(QColor(10, 190, 220));
    QVERIFY(localImage.save(section + "/.assets/local.png"));
    QFile outside(files.path() + "/outside.txt"); QVERIFY(outside.open(QIODevice::WriteOnly));
    outside.write("private-outside-course"); outside.close();
    QString outsideAsset = QStringLiteral(".assets/outside.txt");
#if defined(Q_OS_WIN)
    // QFile::link creates a Windows shortcut, which requires a .lnk suffix.
    outsideAsset += QStringLiteral(".lnk");
#endif
    const auto outsideLink = section + "/" + outsideAsset;
    QVERIFY(QFile::link(outside.fileName(), outsideLink));
    QVERIFY(QFileInfo(outsideLink).isSymLink());
    const QList<QPair<QString, QByteArray>> lessons{
      {"01 Reading.md", "# Markdown lesson\n\nRead this **in the app** with *emphasis*.\n\n- First idea\n- Second idea\n\n![Local image](.assets/local.png)\n\n| Name | Value |\n| --- | --- |\n| Data | **Strong** |\n"},
      {"02 Reading.html", (QByteArray("<!doctype html><html><head><link rel=\"stylesheet\" href=\".assets/layout.css?cache=1\"></head><body><h1>HTML lesson</h1><div id=\"layout\"><strong>Read this here too.</strong><img src=\".assets/local.png\"></div><canvas id=\"paint\" width=\"8\" height=\"8\"></canvas><script>const c=document.getElementById('paint').getContext('2d');c.fillStyle='#0abedc';c.fillRect(0,0,8,8);document.body.dataset.canvas=c.getImageData(0,0,1,1).data.join(',');fetch('.assets/outside.txt').then(r=>r.text()).then(t=>document.body.dataset.outside=t).catch(()=>document.body.dataset.outside='blocked');fetch('") + probeUrl.toUtf8() + "').catch(()=>document.body.dataset.network='blocked');</script></body></html>").replace(".assets/outside.txt", outsideAsset.toUtf8())},
      {"03 rom-cu.csv", "Address,Value\r\n0,\"A, B\"\r\n1,\"C\"\"D\"\r\n"},
      {"05 output.hex.txt", "v3.0 hex words addressed\n0000: 01 02 03"}};
    for (const auto& [name, bytes] : lessons) {
      QFile lesson(section + "/" + name);
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write(bytes), qint64(bytes.size()));
    }
    const auto workbookPath = QFile::encodeName(section + "/04 rom-cu.xlsx");
    auto* archive = zip_open(workbookPath.constData(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    QVERIFY(archive);
    const QList<QPair<QByteArray, QByteArray>> entries{
      {"xl/workbook.xml", "<workbook xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"Registers\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/></Relationships>"},
      {"xl/sharedStrings.xml", "<sst><si><t>Address</t></si><si><t>Value</t></si></sst>"},
      {"xl/worksheets/sheet1.xml", "<worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c></row><row r=\"2\"><c r=\"A2\"><v>42</v></c><c r=\"B2\" t=\"inlineStr\"><is><t>Register</t></is></c></row></sheetData></worksheet>"}};
    for (const auto& [name, bytes] : entries) {
      auto* source = zip_source_buffer(archive, bytes.constData(), bytes.size(), 0);
      QVERIFY(source);
      QVERIFY(zip_file_add(archive, name.constData(), source, ZIP_FL_OVERWRITE) >= 0);
    }
    QCOMPARE(zip_close(archive), 0);
    for (int course = 1; course <= 11; ++course) {
      const auto path = root + QString("/Course %1/Section").arg(course, 2, 10, QChar('0'));
      QVERIFY(QDir().mkpath(path));
      QFile lesson(path + "/Reading.txt");
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QVERIFY(lesson.write("A quiet place to learn.") > 0);
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.resize(1440, 900); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<shadcn::ListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 12);
    QCOMPARE(courses->verticalScrollBar()->width(), 4);
    const auto copyIndex = courses->model()->index(0, 0);
    courses->setCurrentIndex(copyIndex);
    courses->setFocus();
    QApplication::clipboard()->clear();
    QTest::keyClick(courses, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(QApplication::clipboard()->text(),
                 copyIndex.data(Qt::DisplayRole).toString());
    QTimer::singleShot(0, &window, [] {
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
        menu->setActiveAction(menu->actions().at(1));
        QTest::keyClick(menu, Qt::Key_Return);
      }
    });
    const auto rowCenter = courses->visualRect(copyIndex).center();
    QContextMenuEvent selectRow(QContextMenuEvent::Mouse, rowCenter,
                               courses->viewport()->mapToGlobal(rowCenter));
    QApplication::sendEvent(courses->viewport(), &selectRow);
    QPointer<QDialog> selection = window.findChild<QDialog*>("textSelection"); QVERIFY(selection);
    QTRY_VERIFY(selection->isVisible());
    auto* selectedText = selection->findChild<QTextEdit*>("selectedText"); QVERIFY(selectedText);
    QCOMPARE(selectedText->toPlainText(), copyIndex.data(Qt::AccessibleTextRole).toString());
    selection->reject(); QTRY_VERIFY(!selection || !selection->isVisible());
    const auto capture = [&window](const QString& name) {
      const auto path = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      return path.isEmpty() || window.grab().save(path + "/" + name + ".png");
    };
    auto* list = window.findChild<QPushButton*>("listView");
    auto* cards = window.findChild<QPushButton*>("cardsView");
    QTest::mouseClick(cards, Qt::LeftButton);
    QTRY_COMPARE(courses->presentation(), shadcn::ListPresentation::Cards);
    QTest::qWait(300);
    const auto rects = courses->visibleRowRects();
    QVERIFY(rects.size() >= 3);
    QVERIFY(rects[1].left() > rects[0].left());
    QVERIFY(rects[0].right() < rects[1].left());
    int firstRowRight = 0;
    for (const auto& rect : rects) {
      QVERIFY(rect.left() >= 0);
      QVERIFY(rect.right() < courses->viewport()->width());
      if (rect.top() == rects.first().top()) firstRowRight = std::max(firstRowRight, rect.right());
    }
    QVERIFY2(courses->viewport()->width() - 1 - firstRowRight <= 4,
      qPrintable(QString("Viewport %1, grid %2, first row end %3, rectangles %4")
        .arg(courses->viewport()->width()).arg(courses->gridSize().width()).arg(firstRowRight).arg(rects.size())));
    QVERIFY(window.findChild<QWidget*>("libraryCanvas")->width() <= 1120);
    QVERIFY(capture("library-cards"));
    window.resize(560, 720); QTest::qWait(100);
    QCOMPARE(courses->verticalScrollBar()->width(), 4);
    const auto narrowCards = courses->visibleRowRects();
    QVERIFY(!narrowCards.isEmpty());
    for (const auto& rect : narrowCards) {
      QVERIFY(rect.left() >= 0);
      QVERIFY(rect.right() < courses->viewport()->width());
      QCOMPARE(rect.left(), narrowCards.first().left());
    }
    QVERIFY(capture("library-cards-narrow"));
    auto* scroll = courses->verticalScrollBar();
    scroll->setValue(0);
    const int wheelTarget = std::min(scroll->maximum(), scroll->singleStep() * QApplication::wheelScrollLines());
    QVERIFY(wheelTarget > 0);
    const auto wheelPoint = courses->viewport()->rect().center();
    QWheelEvent wheel(wheelPoint, courses->viewport()->mapToGlobal(wheelPoint),
      QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(courses->viewport(), &wheel);
    QCOMPARE(scroll->value(), 0);
    QTest::qWait(35);
    QVERIFY(scroll->value() > 0 && scroll->value() < wheelTarget);
    QTRY_COMPARE(scroll->value(), wheelTarget);
    scroll->setValue(0);
    window.resize(1440, 900); QTest::qWait(100);
    QTest::mouseClick(list, Qt::LeftButton);
    QTRY_COMPARE(courses->presentation(), shadcn::ListPresentation::List);
    QVERIFY(capture("library-list"));
    const auto hoverPoint = courses->visualRect(courses->model()->index(0, 0)).center();
    QHelpEvent tooltipEvent(QEvent::ToolTip, hoverPoint, courses->viewport()->mapToGlobal(hoverPoint));
    QApplication::sendEvent(courses->viewport(), &tooltipEvent);
    QTRY_VERIFY(QToolTip::isVisible());
    bool nativeTooltip = false;
    for (auto* popup : QApplication::topLevelWidgets()) {
      if (popup->windowType() != Qt::ToolTip || !popup->isVisible()) continue;
      nativeTooltip = true;
      QCOMPARE(popup->palette().color(QPalette::Inactive, QPalette::ToolTipBase),
               QColor("#111111"));
      QCOMPARE(popup->palette().color(QPalette::Inactive, QPalette::ToolTipText),
               QColor("#ffffff"));
      const auto path = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!path.isEmpty()) QVERIFY(popup->grab().save(path + "/native-tooltip.png"));
    }
    QVERIFY(nativeTooltip);
    QToolTip::hideText();
    auto* statsButton = window.findChild<QPushButton*>("navStats");
    QCOMPARE(statsButton->text(), QString("Stats"));
    QTest::mouseClick(statsButton, Qt::LeftButton);
    auto* activityChart = window.findChild<shadcn::Chart*>("activityChart");
    auto* mediaChart = window.findChild<shadcn::Chart*>("mediaChart");
    QVERIFY(activityChart && mediaChart);
    QTRY_VERIFY(!activityChart->property("melearnerCopyText").toString().isEmpty());
    QTRY_VERIFY(!mediaChart->property("melearnerCopyText").toString().isEmpty());
    QTRY_VERIFY(!window.findChild<QLabel*>("statsStatus")->isVisible());
    for (const auto* name : {"coursesValue", "coursesDetail", "completionValue", "completionDetail",
                             "watchedValue", "watchedDetail", "storageValue", "storageDetail"}) {
      auto* label = window.findChild<QLabel*>(name);
      QVERIFY(label);
      auto* accessible = QAccessible::queryAccessibleInterface(label);
      QVERIFY(accessible);
      QVERIFY2(accessible->text(QAccessible::Name).contains(label->text()), name);
    }
    QVERIFY(!window.findChild<QLabel*>("statsHeading"));
    auto* mediaTable = window.findChild<shadcn::Table*>("mediaTable"); QVERIFY(mediaTable);
    const auto countIndex = mediaTable->model()->index(0, 1);
    const auto cellCenter = mediaTable->visualRect(countIndex).center();
    QApplication::clipboard()->clear();
    QTimer::singleShot(0, &window, [] {
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
        menu->setActiveAction(menu->actions().first());
        QTest::keyClick(menu, Qt::Key_Return);
      }
    });
    QContextMenuEvent copyCell(QContextMenuEvent::Mouse, cellCenter,
                              mediaTable->viewport()->mapToGlobal(cellCenter));
    QApplication::sendEvent(mediaTable->viewport(), &copyCell);
    QCOMPARE(QApplication::clipboard()->text(), countIndex.data().toString());
    QCoreApplication::processEvents();
    QTest::qWait(300); // Capture the chart after its short entrance animation.
    QVERIFY(capture("stats-wide"));
    window.resize(560, 720); QCoreApplication::processEvents();
    QVERIFY(window.findChild<QWidget*>("statsCanvas")->width() <= 512);
    QVERIFY(capture("stats-narrow"));
    window.resize(1440, 900);
    QTest::mouseClick(statsButton, Qt::LeftButton);
    auto* settings = window.findChild<QPushButton*>("appearance");
    QVERIFY(settings && settings->menu());
    bool hasUpdateCheck = false;
    bool hasAutomaticUpdates = false;
    for (auto* action : settings->menu()->actions()) {
      QVERIFY(!action->menu());
      hasUpdateCheck |= action->objectName() == "checkForUpdates";
      hasAutomaticUpdates |= action->objectName() == "automaticUpdates";
    }
    QVERIFY(hasUpdateCheck && hasAutomaticUpdates);
    bool plainAbout = false;
    bool aboutCaptured = false;
    QTimer::singleShot(200, &window, [&] {
      auto* close = window.findChild<QPushButton*>("aboutClose");
      auto* dialog = close ? qobject_cast<shadcn::Dialog*>(close->window()) : nullptr;
      if (!dialog) return;
      plainAbout = !dialog->findChild<QWidget*>("shadcnDialogFooter")->isVisible();
      const auto path = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      aboutCaptured = path.isEmpty() || dialog->grab().save(path + "/about-plain.png");
      dialog->reject();
    });
    settings->menu()->actions().last()->trigger();
    QVERIFY(plainAbout); QVERIFY(aboutCaptured);
    const auto index = courses->model()->index(11, 0);
    QTRY_COMPARE(index.data().toString(), QString("Native Readers"));
    courses->setCurrentIndex(index);
    QTest::keyClick(courses, Qt::Key_Return);
    auto* document = window.findChild<QTextEdit*>("documentText");
    auto* next = window.findChild<QPushButton*>("nextLesson");
    auto* actions = window.findChild<QWidget*>("lessonActions");
    QTRY_VERIFY(window.findChild<QWebEngineView*>("documentBrowser"));
    QPointer<QWebEngineView> browser = window.findChild<QWebEngineView*>("documentBrowser");
    QSignalSpy deniedResources(window.findChild<melearner::CourseDocumentView*>(),
                              &melearner::CourseDocumentView::resourceDenied);
    QTRY_VERIFY(browserValue(browser, "document.body?.innerText ?? ''").toString().contains("Markdown lesson"));
    QCOMPARE(browserValue(browser, "document.querySelector('strong').textContent").toString(), QString("in the app"));
    QTRY_VERIFY(browserValue(browser, "document.querySelector('img').complete && document.querySelector('img').naturalWidth===8").toBool());
    QCOMPARE(browserValue(browser, "document.querySelectorAll('table').length").toInt(), 1);
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    browser->setFocus();
    auto* browserAccessibility = QAccessible::queryAccessibleInterface(browser);
    QVERIFY(browserAccessibility);
    QVERIFY(browserAccessibility->role() != QAccessible::Animation);
    browserValue(browser, "(()=>{const r=document.createRange();r.selectNodeContents(document.querySelector('strong'));const s=getSelection();s.removeAllRanges();s.addRange(r)})()");
    QTRY_COMPARE(browser->page()->selectedText(), QString("in the app"));
    QApplication::clipboard()->clear();
    QTest::keyClick(browser->focusProxy() ? browser->focusProxy() : browser.data(), Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(QApplication::clipboard()->text(), QString("in the app"));
    QTRY_VERIFY(next->isEnabled());
    QVERIFY(!next->accessibleDescription().isEmpty());
    auto* previous = window.findChild<QPushButton*>("previousLesson");
    QVERIFY(previous); QTRY_VERIFY(!previous->isEnabled());
    QVERIFY(next->parentWidget() == previous->parentWidget());
    QVERIFY(next->height() >= window.fontMetrics().height() * 2);
    auto* outline = window.findChild<shadcn::TreeView*>("lessons");
    QVERIFY(outline);
    const auto sectionIndex = outline->model()->index(0, 0);
    QTRY_VERIFY(!sectionIndex.data(Qt::DisplayRole).toString().isEmpty());
    QCOMPARE(sectionIndex.data(melearner::shadcnRowDescription).toString(), QString());
    QVERIFY(!sectionIndex.data(melearner::shadcnRowTrailingText).toString().isEmpty());
    QTRY_VERIFY(outline->currentIndex().isValid());
    const auto lessonIndex = outline->currentIndex();
    QCOMPARE(lessonIndex.data(melearner::shadcnRowDescription).toString(), QString("Reading"));
    QVERIFY(!lessonIndex.data(Qt::AccessibleDescriptionRole).toString().isEmpty());
    QCOMPARE(window.findChild<QWidget*>("courseOutline")->layout()->contentsMargins(), QMargins());
    QVERIFY(!window.findChild<QWidget*>("documentTools")->isVisible());
    QVERIFY(actions->mapTo(&window, QPoint()).y() < browser->mapTo(&window, QPoint()).y());
    auto* routeTitle = window.findChild<QLabel*>("routeTitle"); QVERIFY(routeTitle);
    QVERIFY(routeTitle->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    QVERIFY(routeTitle->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
    QVERIFY(browser->isVisible());
    QVERIFY(capture("reader-markdown"));
    const QStringList expected{"HTML lesson", "A, B", "Register", "v3.0 hex words"};
    for (int item = 0; item < expected.size(); ++item) {
      QTRY_VERIFY(next->isEnabled());
      QTest::mouseClick(next, Qt::LeftButton);
      for (const auto* label : window.findChildren<QLabel*>())
        QVERIFY(!label->text().startsWith("Opening document"));
      if (item == 0) {
        QTRY_VERIFY(!browser);
        QTRY_VERIFY(window.findChild<QWebEngineView*>("documentBrowser"));
        browser = window.findChild<QWebEngineView*>("documentBrowser");
        QTRY_VERIFY(browserValue(browser, "document.body?.innerText ?? ''").toString().contains(expected[item]));
        QTRY_COMPARE(browserValue(browser, "getComputedStyle(document.getElementById('layout')).display").toString(), QString("grid"));
        QCOMPARE(browserValue(browser, "getComputedStyle(document.querySelector('strong')).color").toString(), QString("rgb(12, 34, 56)"));
        QCOMPARE(browserValue(browser, "document.body.dataset.canvas").toString(), QString("10,190,220,255"));
        browserValue(browser, "fetch('.assets/layout.css').then(r=>r.text()).then(t=>document.body.dataset.localfetch=t.includes('grid')?'ok':'wrong').catch(()=>document.body.dataset.localfetch='failed')");
        QTRY_COMPARE(browserValue(browser, "document.body.dataset.localfetch").toString(), QString("ok"));
        QTRY_VERIFY(!deniedResources.isEmpty());
        QVERIFY(deniedResources.first().first().toString().endsWith("/" + outsideAsset));
        const auto denied = browserValue(browser, "document.body.dataset.outside").toString();
        QVERIFY(denied.isEmpty() || denied == "blocked");
        QTRY_COMPARE(browserValue(browser, "document.body.dataset.network").toString(), QString("blocked"));
        QVERIFY(!offlineProbe.hasPendingConnections());
      } else QTRY_VERIFY2(document->toPlainText().contains(expected[item]),
                         qPrintable(QString("Expected %1; displayed %2").arg(expected[item], document->toPlainText())));
      QVERIFY(capture(QString("reader-%1").arg(item)));
    }
    QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    QTRY_VERIFY(!window.findChild<QWebEngineView*>("documentBrowser"));
    auto* warmTimeout = window.findChild<QTimer*>("documentWarmTimeout");
    QVERIFY(warmTimeout && warmTimeout->isActive());
    warmTimeout->start(1);
    QTRY_VERIFY(window.findChildren<QWebEngineProfile*>().isEmpty());
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
  }

  void warmOfflineDocuments() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    for (int i = 0; i < 2; ++i) {
      QFile file(files.filePath(QString("%1.html").arg(i)));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QString("<html><body>Reading %1<canvas id='drawing'></canvas>"
                         "<script>document.body.dataset.ready='yes'</script></body></html>").arg(i).toUtf8());
    }
    melearner::CourseDocumentView reader;
    reader.resize(800, 600); reader.show();
    QPointer<QWebEngineProfile> profile;
    QUrl previousUrl;
    for (int i = 0; i < 6; ++i) {
      QElapsedTimer elapsed; elapsed.start();
      reader.open(files.path(), files.filePath(QString("%1.html").arg(i % 2)));
      QTRY_VERIFY(reader.findChild<QWebEngineView*>("documentBrowser"));
      auto* browser = reader.findChild<QWebEngineView*>("documentBrowser");
      QTRY_COMPARE(browserValue(browser, "document.body?.dataset.ready ?? ''").toString(), QString("yes"));
      QCOMPARE(browserValue(browser, "document.body.innerText").toString(), QString("Reading %1").arg(i % 2));
      qInfo().noquote() << "HTML_RENDER sample=" << i << "ready_ms=" << elapsed.elapsed();
      auto* currentProfile = browser->page()->profile();
      QVERIFY(currentProfile->isOffTheRecord());
      if (i) {
        QVERIFY2(profile == currentProfile, "Each reading restarts its private WebEngine profile");
        QVERIFY(previousUrl.host() != browser->url().host());
        browserValue(browser, QString("fetch('%1').then(()=>document.body.dataset.stale='allowed')"
                                     ".catch(()=>document.body.dataset.stale='blocked')").arg(previousUrl.toString()));
        QTRY_COMPARE(browserValue(browser, "document.body.dataset.stale").toString(), QString("blocked"));
      }
      profile = currentProfile; previousUrl = browser->url();
    }
    reader.suspend();
    QVERIFY(profile);
    QVERIFY(reader.findChildren<QWebEngineView*>().isEmpty());
    auto* idle = reader.findChild<QTimer*>("documentWarmTimeout");
    QVERIFY(idle && idle->isActive());
    idle->start(1);
    QTRY_VERIFY(!profile);
    // A pending file preparation must never recreate a hidden document.
    reader.open(files.path(), files.filePath("0.html"));
    reader.suspend();
    QTest::qWait(100);
    QVERIFY(reader.findChildren<QWebEngineView*>().isEmpty());
    reader.clear();
    QVERIFY(!profile);
    QVERIFY(reader.findChildren<QWebEngineView*>().isEmpty());
  }

  void importSearchCompleteAndRestoreProgress() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Reading Course/01 Section"));
    for (const auto& item : {QPair<QString, QByteArray>{"01 Introduction.txt", "Introduction text."},
                              {"02 Follow-up.txt", "Follow-up text."}}) {
      QFile lesson(root + "/Reading Course/01 Section/" + item.first);
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write(item.second), qint64(item.second.size()));
    }

    const auto database = files.path() + "/library.sqlite3";
    {
      MainWindow window(database);
      window.show();
      auto* choose = window.findChild<QPushButton*>("chooseRoot");
      QTRY_VERIFY(choose && choose->isEnabled());
      window.chooseRoot(root);
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_COMPARE(courses->model()->rowCount(), 1);
      auto* greeting = window.findChild<QLabel*>("learnerGreeting"); QVERIFY(greeting);
      auto* listMode = window.findChild<QPushButton*>("listView"); QVERIFY(listMode);
      auto* cardsMode = window.findChild<QPushButton*>("cardsView"); QVERIFY(cardsMode);
      QCOMPARE(listMode->mapTo(&window, QPoint(0, listMode->height() / 2)).y(),
               greeting->mapTo(&window, QPoint(0, greeting->height() / 2)).y());
      QCOMPARE(cardsMode->mapTo(&window, QPoint(0, cardsMode->height() / 2)).y(),
               greeting->mapTo(&window, QPoint(0, greeting->height() / 2)).y());

      // The library's search result is a real route: select it and read the file.
      window.activateWindow();
      QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
      auto* search = window.findChild<SearchDialog*>();
      QTRY_VERIFY(search && search->isVisible());
      auto* query = search->findChild<QLineEdit*>("searchQuery");
      QVERIFY(query);
      query->setText("Introduction");
      auto* results = search->findChild<QListView*>("searchResults");
      QTRY_COMPARE(results->model()->rowCount(), 1);
      results->setCurrentIndex(results->model()->index(0, 0));
      QTest::keyClick(results, Qt::Key_Return);

      auto* document = window.findChild<QTextEdit*>("documentText");
      QTRY_COMPARE(document->toPlainText(), QString("Introduction text."));
      QVERIFY(document->isVisible());

      // Search selected a real lesson and opened it in its course route. The
      // course outline remains optional at both compact and desktop widths.
      auto* lessons = window.findChild<QTreeView*>("lessons");
      auto* toggle = window.findChild<QPushButton*>("toggleOutline");
      QVERIFY(lessons && toggle);
      for (const int width : {560, 1280}) {
        window.resize(width, 720);
        QTRY_VERIFY(window.width() <= width);
        if (width == 560) {
          QTRY_VERIFY(toggle->isVisible());
          QTRY_VERIFY(document->isVisible());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(lessons->isVisible());
          QVERIFY(!document->isVisible());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(document->isVisible());
          QVERIFY(!lessons->isVisible());
          const auto* viewer = window.findChild<QScrollArea*>("lessonScroll");
          QTRY_VERIFY(viewer->mapTo(&window, QPoint()).x() < 50);
          QTRY_VERIFY(document->mapTo(&window, QPoint(document->width(), 0)).x() <= window.width());
        } else {
          const auto* outline = window.findChild<QWidget*>("courseOutline");
          const auto* viewer = window.findChild<QScrollArea*>("lessonScroll");
          QCOMPARE(outline->minimumWidth(), 0);
          QTRY_VERIFY(toggle->isVisible());
          if (!lessons->isVisible()) QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(lessons->isVisible());
          QTRY_COMPARE(outline->width(), 320);
          QTRY_VERIFY(document->isVisible());
          QTRY_VERIFY(outline->mapTo(&window, QPoint(outline->width(), 0)).x() <=
                      viewer->mapTo(&window, QPoint()).x());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(!outline->isVisible());
          QTRY_VERIFY(document->isVisible());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(outline->isVisible());
          QTRY_VERIFY(document->isVisible());
          auto* transition = window.findChild<QVariantAnimation*>("outlineReveal"); QVERIFY(transition);
          if (!melearner::reducedMotion() && !melearner::highContrast()) {
            QCOMPARE(transition->duration(), 220);
            QCOMPARE(transition->startValue().toDouble(), 0.0);
            transition->setCurrentTime(40);
            QVERIFY(outline->width() > 0 && outline->width() < 320);
            QVERIFY(document->isVisible());
          }
          QTRY_COMPARE(transition->state(), QAbstractAnimation::Stopped);
          QCOMPARE(outline->graphicsEffect()->property("opacity").toDouble(), 1.0);
          lessons->setFocus();
          QTest::keyClick(&window, Qt::Key_Comma); QTest::keyClick(&window, Qt::Key_O);
          QVERIFY(!outline->isVisible());
          QCOMPARE(transition->state(), QAbstractAnimation::Stopped);
          document->setFocus();
          QTest::keyClick(&window, Qt::Key_Comma); QTest::keyClick(&window, Qt::Key_O);
          QVERIFY(outline->isVisible());
          {
            const int flashTime = QApplication::cursorFlashTime();
            const auto restore = qScopeGuard([flashTime] { QApplication::setCursorFlashTime(flashTime); });
            QApplication::setCursorFlashTime(0);
            QTest::mouseClick(toggle, Qt::LeftButton); QVERIFY(!outline->isVisible());
            QTest::mouseClick(toggle, Qt::LeftButton); QVERIFY(outline->isVisible());
            QCOMPARE(transition->state(), QAbstractAnimation::Stopped);
          }
          const auto* outlineScroll = lessons->verticalScrollBar();
          QCOMPARE(outlineScroll->width(), 4);
        }
        if (const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS"); !captures.isEmpty())
          QVERIFY(window.grab().save(captures + QString("/course-%1.png").arg(width)));
      }
      auto* searchButton = window.findChild<QPushButton*>("searchButton");
      QVERIFY(searchButton);
      QVERIFY(!searchButton->isVisible());
      window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
      search = window.findChild<SearchDialog*>();
      QTRY_VERIFY(search && search->isVisible());
      search->reject();

      auto* complete = window.findChild<QPushButton*>("markComplete");
      QTRY_VERIFY(complete && complete->isEnabled());
      QTest::mouseClick(complete, Qt::LeftButton);
      QTRY_COMPARE(complete->text(), QString("Mark incomplete"));
      QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
      QVERIFY(courses->isVisible());
    }

    // A new window on the same database proves completion survived close/reopen.
    MainWindow reopened(database);
    reopened.show();
    auto* choose = reopened.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY(choose->isEnabled());
    reopened.chooseRoot(root);
    QTRY_VERIFY(choose->isEnabled());
    auto* courses = reopened.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto largeFont = originalFont;
    if (largeFont.pixelSize() > 0) largeFont.setPixelSize(largeFont.pixelSize() * 2);
    else largeFont.setPointSizeF(largeFont.pointSizeF() * 2);
    QApplication::setFont(largeFont);
    reopened.resize(560, 720);
    QTRY_VERIFY(reopened.width() <= 560);
    QTRY_COMPARE(courses->horizontalScrollBar()->maximum(), 0);
    auto* resume = reopened.findChild<QPushButton*>("resumeLesson");
    QTRY_VERIFY(resume->isVisible());
    QCOMPARE(reopened.findChild<shadcn::Progress*>("resumeProgress")->value(), 50);
    QTRY_COMPARE(reopened.findChild<QLabel*>("resumeLessonTitle")->text(), QString("02 Follow-up"));
    QTest::mouseClick(resume, Qt::LeftButton);
    auto* complete = reopened.findChild<QPushButton*>("markComplete");
    auto* document = reopened.findChild<QTextEdit*>("documentText");
    QTRY_VERIFY2(complete->isEnabled(), qPrintable(reopened.findChild<QLabel*>("appStatus")->text()));
    QTRY_COMPARE(reopened.findChild<QLabel*>("lessonTitle")->text(), QString("02 Follow-up"));
    QTRY_COMPARE(document->toPlainText(), QString("Follow-up text."));

    // Resume chooses the unfinished lesson. Search back to the completed lesson
    // to prove its saved state survived closing and reopening the library.
    reopened.activateWindow();
    QTest::keyClick(&reopened, Qt::Key_Slash);
    auto* search = reopened.findChild<SearchDialog*>();
    QTRY_VERIFY(search && search->isVisible());
    auto* query = search->findChild<QLineEdit*>("searchQuery");
    query->setText("Introduction");
    auto* results = search->findChild<QListView*>("searchResults");
    QTRY_COMPARE(results->model()->rowCount(), 1);
    results->setCurrentIndex(results->model()->index(0, 0));
    QTest::keyClick(results, Qt::Key_Return);
    QTRY_COMPARE(document->toPlainText(), QString("Introduction text."));
    QTRY_COMPARE(complete->accessibleName(), QString("Mark incomplete"));
  }

  void openPdfFromCourseOutline() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/PDF Course/Section"));
    {
      QPdfWriter writer(root + "/PDF Course/Section/Reading.pdf");
      writer.setResolution(72);
      QPainter painter(&writer);
      for (int page = 1; page <= 3; ++page) {
        if (page > 1) QVERIFY(writer.newPage());
        painter.drawText(50, 50, page == 3 ? QStringLiteral("Short page 東京")
                                           : QString("Local PDF lesson · page %1 café 東京").arg(page));
      }
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.show();
    auto* homeTitle = window.findChild<QLabel*>("routeTitle"); QVERIFY(homeTitle);
    QCOMPARE(homeTitle->text(), QString("meLearner"));
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    QTest::keyClick(courses, Qt::Key_Return);
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTRY_COMPARE(lessons->model()->rowCount(), 1);
    QTest::keyClick(lessons, Qt::Key_Return);
    auto* pdf = window.findChild<PdfView*>();
    QVERIFY(pdf);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* pdfAccessible = QAccessible::queryAccessibleInterface(pdf);
    QVERIFY(pdfAccessible);
    QTRY_VERIFY(pdfAccessible->text(QAccessible::Name).contains("1 of 3"));
    pdf->setFocus();
    QTRY_VERIFY(pdf->hasFocus());
    QVERIFY(pdfAccessible->state().focusable);
    auto* pdfText = pdfAccessible->textInterface();
    QVERIFY(pdfText);
    const QString firstPageText = QStringLiteral("Local PDF lesson · page 1 café 東京");
    QTRY_VERIFY(pdfText->text(0, pdfText->characterCount()).contains(firstPageText));
    QVERIFY(pdfText->characterCount() >= firstPageText.size());
    QTRY_VERIFY(!pdfText->characterRect(0).isEmpty());
    QVERIFY(pdfText->offsetAtPoint(pdfText->characterRect(0).center()) >= 0);
    pdfText->setCursorPosition(3);
    QCOMPARE(pdfText->cursorPosition(), 3);
    pdfText->setSelection(0, 0, 3);
    QCOMPARE(pdfText->selectionCount(), 1);
    int selectionStart = -1;
    int selectionEnd = -1;
    pdfText->selection(0, &selectionStart, &selectionEnd);
    QCOMPARE(selectionStart, 0);
    QCOMPARE(selectionEnd, 3);
    window.resize(1920, 1080);
    QTRY_VERIFY(pdf->width() <= 1000);
    if (const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS"); !captures.isEmpty())
      QVERIFY(window.grab().save(captures + "/reader-pdf.png"));
    auto* zoom = window.findChild<QComboBox*>("pdfZoom");
    QVERIFY(zoom);
    bool zoomInvalidatedTiles = false;
    connect(zoom, &QComboBox::activated, &window, [&] { zoomInvalidatedTiles = pdf->cachedTiles() == 0; });
    QTest::keyClick(zoom, Qt::Key_End);
    QCOMPARE(pdf->zoomForTesting(), 64);
    QVERIFY(zoomInvalidatedTiles);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* page = window.findChild<QLineEdit*>("pdfPage");
    QVERIFY(page);
    page->setFocus();
    QTRY_VERIFY(page->hasFocus());
    page->setText("3");
    QTest::keyClick(page, Qt::Key_Return);
    QTRY_COMPARE(page->text(), QString("3"));
    const QString thirdPageText = QStringLiteral("Short page 東京");
    QTRY_VERIFY(pdfAccessible->text(QAccessible::Name).contains("3 of 3"));
    QTRY_VERIFY(pdfText->text(0, pdfText->characterCount()).contains(thirdPageText));
    QCOMPARE(pdfText->selectionCount(), 0);
    QCOMPARE(pdfText->cursorPosition(), 0);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* back = window.findChild<QPushButton*>("backToLibrary");
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    QCOMPARE(pdf->cachedTiles(), 0);
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
  }

  void libraryScrollPersistsAcrossTabSwitchesAndRestart() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    ScopedTestSettings isolatedSettings(files.path());
    const auto root = files.path() + "/Courses";
    for (int course = 1; course <= 24; ++course) {
      const auto section = root + QString("/Course %1/Section").arg(course, 2, 10, QChar('0'));
      QVERIFY(QDir().mkpath(section));
      QFile lesson(section + "/Reading.txt");
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      const QByteArray content("A quiet place to learn.");
      QCOMPARE(lesson.write(content), qint64(content.size()));
    }
    const auto database = files.path() + "/library.sqlite3";
    int coursesPosition = 0;
    int statsPosition = 0;
    {
      MainWindow window(database);
      window.resize(560, 620); window.show();
      QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
      window.chooseRoot(root);
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_COMPARE(courses->model()->rowCount(), 24);
      auto* courseScroll = courses->verticalScrollBar();
      QTRY_VERIFY(courseScroll->maximum() > 0);
      coursesPosition = courseScroll->maximum() * 2 / 3;
      QVERIFY(coursesPosition > 0);
      courseScroll->setValue(coursesPosition);

      auto* statsButton = window.findChild<QPushButton*>("navStats");
      auto* stack = window.findChild<shadcn::Tabs*>("libraryStack");
      auto* statsScroll = window.findChild<shadcn::ScrollArea*>("statsScroll");
      QVERIFY(statsButton && stack && statsScroll);
      QTest::mouseClick(statsButton, Qt::LeftButton);
      QTRY_COMPARE(stack->currentValue(), QString("stats"));
      QTRY_VERIFY(statsScroll->verticalScrollBar()->maximum() > 0);
      QCOMPARE(courseScroll->value(), coursesPosition);
      statsPosition = statsScroll->verticalScrollBar()->maximum() / 2;
      QVERIFY(statsPosition > 0);
      statsScroll->verticalScrollBar()->setValue(statsPosition);

      QTest::mouseClick(statsButton, Qt::LeftButton);
      QTRY_COMPARE(stack->currentValue(), QString("courses"));
      QCOMPARE(courseScroll->value(), coursesPosition);
      QTest::mouseClick(statsButton, Qt::LeftButton);
      QTRY_COMPARE(stack->currentValue(), QString("stats"));
      QCOMPARE(statsScroll->verticalScrollBar()->value(), statsPosition);
      // Closing on Stats saves its position; switching back above saved Courses.
    }

    QSettings().sync();
    MainWindow reopened(database);
    reopened.resize(560, 620); reopened.show();
    QTRY_VERIFY(reopened.findChild<QPushButton*>("chooseRoot")->isEnabled());
    reopened.chooseRoot(root);
    auto* courses = reopened.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 24);
    QTRY_COMPARE(courses->verticalScrollBar()->value(), coursesPosition);
    auto* statsButton = reopened.findChild<QPushButton*>("navStats");
    auto* stack = reopened.findChild<shadcn::Tabs*>("libraryStack");
    auto* statsScroll = reopened.findChild<shadcn::ScrollArea*>("statsScroll");
    QVERIFY(statsButton && stack && statsScroll);
    QTest::mouseClick(statsButton, Qt::LeftButton);
    QTRY_COMPARE(stack->currentValue(), QString("stats"));
    QTRY_COMPARE(statsScroll->verticalScrollBar()->value(), statsPosition);
  }

  void readerScrollPersistsWhenReopened() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    ScopedTestSettings isolatedSettings(files.path());
    const auto root = files.path() + "/Courses";
    const auto section = root + "/Scroll Course/Section";
    QVERIFY(QDir().mkpath(section));
    QString html = "<!doctype html><html><body><h1>Long HTML lesson</h1>";
    for (int paragraph = 0; paragraph < 160; ++paragraph)
      html += QString("<p>Lesson paragraph %1 contains enough text to make a tall page.</p>").arg(paragraph);
    html += "</body></html>";
    QFile htmlLesson(section + "/01 Long HTML.html");
    QVERIFY(htmlLesson.open(QIODevice::WriteOnly));
    QCOMPARE(htmlLesson.write(html.toUtf8()), qint64(html.toUtf8().size()));
    htmlLesson.close();
    {
      QPdfWriter writer(section + "/02 Long PDF.pdf");
      writer.setResolution(72);
      QPainter painter(&writer);
      for (int page = 1; page <= 8; ++page) {
        if (page > 1) QVERIFY(writer.newPage());
        painter.drawText(50, 50, QString("Scroll PDF page %1").arg(page));
      }
    }
    for (int lesson = 3; lesson <= 42; ++lesson) {
      QFile extra(section + QString("/%1 Outline item.txt").arg(lesson, 2, 10, QChar('0')));
      QVERIFY(extra.open(QIODevice::WriteOnly));
      QVERIFY(extra.write("An outline item.") > 0);
    }

    MainWindow window(files.path() + "/library.sqlite3");
    window.resize(1000, 650); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    const auto openCourse = [&] {
      QTRY_VERIFY(!courses->model()->index(0, 0).data(Qt::UserRole).toString().isEmpty());
      const auto index = courses->model()->index(0, 0);
      QTest::mouseClick(courses->viewport(), Qt::LeftButton, Qt::NoModifier,
                        courses->visualRect(index).center());
    };
    openCourse();
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTRY_COMPARE(lessons->model()->rowCount(), 1);
    auto sectionIndex = lessons->model()->index(0, 0);
    QTRY_VERIFY(!sectionIndex.data(Qt::DisplayRole).toString().isEmpty());
    lessons->expand(sectionIndex);
    QTRY_COMPARE(lessons->model()->rowCount(sectionIndex), 42);
    auto* outlineScroll = lessons->verticalScrollBar();
    QTRY_VERIFY(outlineScroll->maximum() > 0);
    const auto htmlIndex = lessons->model()->index(0, 0, sectionIndex);
    QTRY_COMPARE(htmlIndex.data(Qt::DisplayRole).toString(), QString("01 Long HTML"));
    lessons->scrollTo(htmlIndex);
    QTRY_VERIFY(!lessons->visualRect(htmlIndex).isEmpty());
    QSignalSpy htmlLoaded(window.findChild<melearner::CourseDocumentView*>(), &melearner::CourseDocumentView::loaded);
    QTest::mouseClick(lessons->viewport(), Qt::LeftButton, Qt::NoModifier,
                      lessons->visualRect(htmlIndex).center());
    QTRY_VERIFY(window.findChild<QWebEngineView*>("documentBrowser"));
    QPointer<QWebEngineView> browser = window.findChild<QWebEngineView*>("documentBrowser");
    QTRY_VERIFY2(browserValue(browser, "document.body?.innerText ?? ''").toString().contains("Long HTML lesson"),
      qPrintable(QString("Lesson: %1; URL: %2; body: %3").arg(window.findChild<QLabel*>("lessonTitle")->text(),
        browser ? browser->url().toString() : QString("missing"), browserValue(browser, "document.body?.innerText ?? ''").toString().left(300))));
    QTRY_VERIFY(browserValue(browser, "document.body.scrollHeight > window.innerHeight").toBool());
    QTRY_VERIFY(!htmlLoaded.isEmpty() && htmlLoaded.last()[0].toBool());
    const int htmlPosition = 1200;
    browserValue(browser, QString("window.scrollTo(0, %1); window.scrollY").arg(htmlPosition));
    QTRY_VERIFY(browserValue(browser, "window.scrollY").toInt() >= htmlPosition - 2);
    QTRY_VERIFY(browser->page()->scrollPosition().y() >= htmlPosition - 2);
    QTRY_COMPARE(lessons->currentIndex().data(Qt::DisplayRole).toString(), QString("01 Long HTML"));
    const int outlinePosition = outlineScroll->maximum() * 2 / 3;
    outlineScroll->setValue(outlinePosition);

    auto* back = window.findChild<QPushButton*>("backToLibrary");
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    openCourse();
    QTRY_VERIFY(window.findChild<QWebEngineView*>("documentBrowser"));
    browser = window.findChild<QWebEngineView*>("documentBrowser");
    QTRY_VERIFY(browserValue(browser, "document.body?.innerText ?? ''").toString().contains("Long HTML lesson"));
    QTRY_COMPARE(outlineScroll->value(), outlinePosition);
    QTRY_VERIFY(browserValue(browser, "window.scrollY").toInt() >= htmlPosition - 2);

    sectionIndex = lessons->model()->index(0, 0);
    const auto pdfIndex = lessons->model()->index(1, 0, sectionIndex);
    QTRY_COMPARE(pdfIndex.data(Qt::DisplayRole).toString(), QString("02 Long PDF"));
    lessons->scrollTo(pdfIndex);
    QTRY_VERIFY(!lessons->visualRect(pdfIndex).isEmpty());
    QTest::mouseClick(lessons->viewport(), Qt::LeftButton, Qt::NoModifier,
                      lessons->visualRect(pdfIndex).center());
    auto* pdf = window.findChild<PdfView*>();
    QVERIFY(pdf);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    QTRY_VERIFY(pdf->verticalScrollBar()->maximum() > 0);
    const int pdfPosition = pdf->verticalScrollBar()->maximum() / 2;
    pdf->verticalScrollBar()->setValue(pdfPosition);
    outlineScroll->setValue(outlinePosition);

    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    openCourse();
    pdf = window.findChild<PdfView*>();
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    QTRY_COMPARE(pdf->verticalScrollBar()->value(), pdfPosition);
    QTRY_COMPARE(outlineScroll->value(), outlinePosition);
  }

  void keyboardPopupAndTextInputStayScoped() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    for (const auto& name : {"Keyboard", "Second"}) {
      QVERIFY(QDir().mkpath(root + "/" + name + "/Section"));
      QFile lesson(root + "/" + name + "/Section/Read.txt");
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write("Keyboard lesson"), qint64(15));
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 2);
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));

    courses->setFocus();
    QTest::keyClick(courses, Qt::Key_Question, Qt::ShiftModifier);
    QPointer<QDialog> shortcuts = window.findChild<QDialog*>("shortcutHelp");
    QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    auto* shortcutList = shortcuts->findChild<QListWidget*>();
    QVERIFY(shortcutList);
    for (int row = 0; row < shortcutList->count(); ++row) {
      const auto text = shortcutList->item(row)->text();
      QVERIFY(!text.contains("F1") && !text.contains("Ctrl+Space"));
    }
    const auto capturePath = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!capturePath.isEmpty()) {
      auto* openingEffect = shortcuts->findChild<QGraphicsOpacityEffect*>();
      QVERIFY(openingEffect);
      QTRY_COMPARE(openingEffect->opacity(), 1.0);
      QVERIFY(shortcuts->grab().save(capturePath + "/vim-shortcuts.png"));
    }
    shortcuts->reject();
    QTRY_VERIFY(shortcuts.isNull());

    QLineEdit input(&window);
    input.show();
    input.setFocus();
    QTest::keyClick(&input, Qt::Key_J);
    QCOMPARE(input.text(), QString("j"));
    QTest::mouseClick(window.findChild<QPushButton*>("showShortcuts"), Qt::LeftButton);
    shortcuts = window.findChild<QDialog*>("shortcutHelp");
    QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    shortcuts->reject();
    QTRY_VERIFY(shortcuts.isNull());

    window.activateWindow();
    courses->setFocus();
    QTRY_VERIFY(courses->hasFocus());
    courses->setCurrentIndex(courses->model()->index(1, 0));
    QTest::keyClick(courses, Qt::Key_G);
    input.setFocus();
    QTest::keyClick(&input, Qt::Key_X);
    QCOMPARE(input.text(), QString("jx"));
    window.activateWindow();
    courses->setFocus();
    QTRY_VERIFY(courses->hasFocus());
    QTest::keyClick(courses, Qt::Key_G);
    QCOMPARE(courses->currentIndex().row(), 1);
    QTest::keyClick(courses, Qt::Key_G);
    QCOMPARE(courses->currentIndex().row(), 0);

    QTest::keyClick(courses, Qt::Key_Colon, Qt::ShiftModifier);
    QPointer<QDialog> palette = window.findChild<QDialog*>("commandPalette");
    QTRY_VERIFY(palette && palette->isVisible());
    auto* paletteSearch = palette->findChild<QLineEdit*>("keyboardPopupCommand");
    if (!paletteSearch) paletteSearch = palette->findChild<QLineEdit*>();
    QVERIFY(paletteSearch);
    paletteSearch->setText("last item");
    QTest::keyClick(paletteSearch, Qt::Key_Return);
    QTRY_VERIFY(palette.isNull() || !palette->isVisible());
    QTRY_COMPARE(courses->currentIndex().row(), 1);
    courses->setFocus();
    QTest::keyClick(courses, Qt::Key_Return);
    QTRY_VERIFY(window.findChild<QWidget*>("courseOutline")->isVisible());
    auto* courseOutline = window.findChild<shadcn::TreeView*>("lessons");
    courseOutline->setFocus();
    QTest::keyClick(courseOutline, Qt::Key_Comma);
    QTest::keyClick(courseOutline, Qt::Key_B);
    QTRY_VERIFY(courses->isVisible());
  }
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
