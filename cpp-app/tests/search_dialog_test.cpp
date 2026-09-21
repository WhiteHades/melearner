#include "search_dialog.hpp"
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class SearchDialogTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); }
  void searchesPagesAndActivatesByKeyboard() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto root = data.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Physics/Mechanics"));
    for (int index = 0; index < 150; ++index) {
      QFile lesson(root + QString("/Physics/Mechanics/%1 Motion.txt").arg(index));
      QVERIFY(lesson.open(QIODevice::WriteOnly)); lesson.write("Lesson");
    }
    melearner::library::Library library(data.path() + "/library.sqlite3");
    QSignalSpy opened(&library, &melearner::library::Library::opened);
    QSignalSpy scanned(&library, &melearner::library::Library::scanFinished);
    QVERIFY(library.open()); QTRY_COMPARE(opened.count(), 1);
    QVERIFY(library.scan(root)); QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 10000);
    SearchDialog dialog(library); QSignalSpy selected(&dialog, &SearchDialog::selected); dialog.show();
    auto* query = dialog.findChild<QLineEdit*>("searchQuery");
    auto* results = dialog.findChild<QListView*>("searchResults");
    query->setText("Motion"); QTRY_COMPARE(results->model()->rowCount(), 150);
    const auto last = results->model()->index(149, 0); results->scrollTo(last);
    QTRY_VERIFY(!last.data(Qt::UserRole).toString().isEmpty());
    query->setText("no match"); QCOMPARE(results->model()->rowCount(), 0);
    QTest::keyClick(query, Qt::Key_Return); QVERIFY(selected.isEmpty());
    query->setText("Physics"); QTRY_COMPARE(results->model()->rowCount(), 1);
    QTest::keyClick(query, Qt::Key_Down); QTest::keyClick(query, Qt::Key_Return);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(qvariant_cast<melearner::library::SearchRow>(selected.first().first()).courseName, QString("Physics"));
    QVERIFY(!dialog.isVisible());
  }
};
QTEST_MAIN(SearchDialogTest)
#include "search_dialog_test.moc"
