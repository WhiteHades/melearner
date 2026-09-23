#include "paged_list_model.hpp"
#include <QApplication>
#include <QImage>
#include <QListView>
#include <QPainter>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QtTest>
#include <utility>

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
      QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
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
    QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(requests.size(), 5);
    QVERIFY(model.setPage(128, 2000, rows));
    QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(requests.size(), 6);
    QCOMPARE(requests.last().first().toInt(), 8 * 128);
  }
  void delegatePaintsNormalDoubleTextAndSelection() {
    PagedListModel model(128);
    QVERIFY(model.setPage(0, 1, {StudyRow{"lesson", "A lesson", "Section · 12 min", false, true}}));
    StudyItemDelegate delegate;
    QListView view;
    view.resize(360, 160);

    struct PaintedItem {
      QImage image;
      QRect titleRect;
      QRect detailRect;
    };
    const auto exactPixels = [](const QImage& image, const QRect& rect, const QColor& color) {
      int matches = 0;
      const auto clipped = rect.intersected(image.rect());
      for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
          matches += image.pixelColor(x, y) == color;
        }
      }
      return matches;
    };

    const auto render = [&](qreal scale, bool selected) {
      auto font = QApplication::font();
      font.setPointSizeF(font.pointSizeF() * scale);
      QStyleOptionViewItem option;
      option.initFrom(&view);
      option.widget = &view;
      option.font = font;
      option.fontMetrics = QFontMetrics(font);
      option.state = QStyle::State_Enabled;
      if (selected) option.state |= QStyle::State_Selected;
      const QColor base("#f1f1f1");
      const QColor text("#101010");
      const QColor muted("#505050");
      const QColor highlight("#1d4ed8");
      const QColor highlighted("#ffffff");
      for (const auto group : {QPalette::Active, QPalette::Inactive}) {
        option.palette.setColor(group, QPalette::Base, base);
        option.palette.setColor(group, QPalette::Text, text);
        option.palette.setColor(group, QPalette::PlaceholderText, muted);
        option.palette.setColor(group, QPalette::Highlight, highlight);
        option.palette.setColor(group, QPalette::HighlightedText, highlighted);
      }
      option.rect = QRect(QPoint(), QSize(360, delegate.sizeHint(option, model.index(0)).height()));
      QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
      image.fill(base);
      QPainter painter(&image);
      delegate.paint(&painter, option, model.index(0));
      auto titleFont = font;
      titleFont.setWeight(QFont::DemiBold);
      titleFont.setPointSizeF(titleFont.pointSizeF() * 1.06);
      const QFontMetrics titleMetrics(titleFont);
      const QFontMetrics detailMetrics(font);
      const int titleTop = 10;
      return PaintedItem{std::move(image),
                         QRect(16, titleTop, 328, titleMetrics.lineSpacing()),
                         QRect(16, titleTop + titleMetrics.lineSpacing() + 1, 328, detailMetrics.lineSpacing())};
    };

    const auto normal = render(1.0, false);
    const auto doubled = render(2.0, false);
    const auto selected = render(1.0, true);
    QVERIFY(!normal.image.isNull());
    QVERIFY(!doubled.image.isNull());
    QVERIFY(doubled.image.height() > normal.image.height());
    QCOMPARE(normal.image.pixelColor(2, normal.image.height() / 2), QColor("#f1f1f1"));
    QCOMPARE(selected.image.pixelColor(2, selected.image.height() / 2), QColor("#1d4ed8"));
    QVERIFY(exactPixels(normal.image, normal.titleRect, QColor("#101010")) > 0);
    QVERIFY(exactPixels(normal.image, normal.detailRect, QColor("#505050")) > 0);
    QVERIFY(exactPixels(doubled.image, doubled.titleRect, QColor("#101010")) > 0);
    QVERIFY(exactPixels(doubled.image, doubled.detailRect, QColor("#505050")) > 0);
    QVERIFY(exactPixels(selected.image, selected.titleRect, QColor("#ffffff")) > 0);
    QVERIFY(exactPixels(selected.image, selected.detailRect, QColor("#ffffff")) > 0);
  }
};
QTEST_MAIN(PagedListModelTest)
#include "paged_list_model_test.moc"
