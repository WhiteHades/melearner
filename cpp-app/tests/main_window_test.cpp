#include "main_window.hpp"
#include "pdf_view.hpp"
#include "search_dialog.hpp"
#include "theme.hpp"

#include <QDir>
#include <QAccessible>
#include <QComboBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialog>
#include <QFile>
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
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTreeView>
#include <QWheelEvent>
#include <shadcn/widgets.hpp>
#include <shadcn/data.hpp>
#include <shadcn/overlays.hpp>
#include <QtTest>
#include "course_document_view.hpp"
#include <QWebEngineProfile>
#include <zip.h>

namespace {
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
               melearner::roleColor(shadcn::Role::Foreground));
      QCOMPARE(popup->palette().color(QPalette::Inactive, QPalette::ToolTipText),
               melearner::roleColor(shadcn::Role::Background));
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
    QCOMPARE(settings->menu()->actions().size(), 4); // Three choices plus a separator.
    for (auto* action : settings->menu()->actions()) QVERIFY(!action->menu());
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
    QVERIFY(window.findChildren<QWebEngineProfile*>().isEmpty());
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
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
        painter.drawText(50, 50, QString("Local PDF lesson · page %1").arg(page));
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
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* back = window.findChild<QPushButton*>("backToLibrary");
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    QCOMPARE(pdf->cachedTiles(), 0);
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
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
