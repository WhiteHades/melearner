#include "paged_list_model.hpp"
#include <QSignalSpy>
#include <QtTest>

class PagedListModelTest final : public QObject {
  Q_OBJECT
private slots:
  void requestsAndBounds() {
    PagedListModel model(128);
    QSignalSpy requests(&model, &PagedListModel::pageRequested);
    model.reset();
    QCOMPARE(requests.size(), 1);
    QList<StudyRow> rows;
    for (int i = 0; i < 128; ++i) rows.append({QString::number(i), "Course", "0 / 1 complete", false, true});
    QVERIFY(model.setPage(0, 1000, rows));
    QCOMPARE(model.rowCount(), 1000);
    QCOMPARE(model.index(0).data().toString(), QString("Course\n0 / 1 complete"));
    QVERIFY(model.updateRow({"0", "Course", "1 / 1 complete", true, true}));
    QCOMPARE(model.index(0).data().toString(), QString("Course\n1 / 1 complete"));
    QVERIFY(!model.updateRow({"missing", "", ""}));
    QVERIFY(model.updateRow({"0", "<img src='local'>", "A & B"}));
    QCOMPARE(model.index(0).data(Qt::ToolTipRole).toString(), QString("<qt>&lt;img src='local'&gt;<br>A &amp; B</qt>"));
    for (int page = 1; page < 7; ++page) {
      model.index(page * 128).data();
      model.index(page * 128).data();
      QCoreApplication::processEvents();
      QCOMPARE(requests.size(), page + 1);
      QVERIFY(model.setPage(page * 128, 1000, rows));
      QVERIFY(model.cachedRows() <= 4 * 128);
    }
    QVERIFY(!model.setPage(1, 1000, rows));
    QVERIFY(!model.setPage(0, -1, rows));
  }
  void quickScrollCoalescesLatestPage() {
    PagedListModel model(128);
    QSignalSpy requests(&model, &PagedListModel::pageRequested);
    model.reset();
    QList<StudyRow> rows(128);
    QVERIFY(model.setPage(0, 2000, rows));
    for (int page = 1; page < 9; ++page) model.index(page * 128).data();
    QCoreApplication::processEvents();
    QCOMPARE(requests.size(), 5);
    QVERIFY(model.setPage(128, 2000, rows));
    QCoreApplication::processEvents();
    QCOMPARE(requests.size(), 6);
    QCOMPARE(requests.last().first().toInt(), 8 * 128);
  }
};
QTEST_GUILESS_MAIN(PagedListModelTest)
#include "paged_list_model_test.moc"
