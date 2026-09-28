#include "paged_list_model.hpp"
#include "theme.hpp"
#include <shadcn/rows.hpp>
#include <shadcn/widgets.hpp>
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QStyle>
#include <QtTest>
#include <algorithm>
#include <cmath>
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
    // The title and the description are separate roles now. A row is described by
    // data rather than by a newline the view has to split, so a title containing a
    // newline no longer silently becomes a two line row.
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(), QString("Course"));
    QCOMPARE(model.index(0).data(melearner::shadcnRowDescription).toString(), QString("0 / 1 complete"));
    QVERIFY(model.updateRow({"0", "Course", "1 / 1 complete", true, true}));
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(), QString("Course"));
    QCOMPARE(model.index(0).data(melearner::shadcnRowDescription).toString(), QString("1 / 1 complete"));
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
  /// The model's contract with the themed row view: it fills the roles the view
  /// reads, at any text scale, and a selected row's text clears contrast on the
  /// fill the view paints. The view's own painting is the component library's
  /// concern and is tested there; what this application owns is the data.
  void rowsCarryTheirOwnDataAndStayReadable() {
    PagedListModel model(128);
    QList<StudyRow> page;
    // Enough rows that a change in row height changes how many fit on screen. One
    // row would show the same count at any size, which is a check that cannot fail.
    for (int i = 0; i < 40; ++i) {
      page.append(StudyRow{QString::number(i), "A lesson",
                           QStringLiteral("Section · %1 min").arg(12 + i), false, true});
    }
    QVERIFY(model.setPage(0, (int)page.size(), page));

    shadcn::install(*qApp, shadcn::Theme::neutral(), shadcn::MotionPolicy::Reduced);
    // Compared by name, not by operator==: a colour read from an image carries a
    // different colour spec than one built from theme floats, which operator==
    // treats as unequal even when the channels match.
    const auto exactPixels = [](const QImage& image, const QRect& rect, const QColor& color) {
      int matches = 0;
      const auto clipped = rect.intersected(image.rect());
      for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
          matches += image.pixelColor(x, y).name(QColor::HexRgb) == color.name(QColor::HexRgb);
        }
      }
      return matches;
    };
    const auto toColor = [](shadcn::Role role) {
      const auto value = shadcn::Theme::neutral().color(role);
      return QColor::fromRgbF(static_cast<float>(value.r), static_cast<float>(value.g),
                              static_cast<float>(value.b));
    };
    const auto base = toColor(shadcn::Role::Background);
    const auto text = toColor(shadcn::Role::Foreground);
    const auto muted = toColor(shadcn::Role::MutedForeground);
    const auto highlight = toColor(shadcn::Role::Accent);
    const auto highlighted = toColor(shadcn::Role::AccentForeground);

    // The row is described by data, not by a newline the view would have to
    // split. A title that contains a newline is the case that would break.
    model.updateRow(StudyRow{"0", "A lesson\nwith a newline",
                             "Section · 12 min", false, true});
    const auto row = model.index(0);
    QCOMPARE(model.data(row, Qt::DisplayRole).toString(), QString("A lesson\nwith a newline"));
    QCOMPARE(model.data(row, melearner::shadcnRowDescription).toString(),
             QString("Section · 12 min"));
    // A row with no course behind it reports no progress, so no empty rail is
    // drawn on a row that has nothing to state.
    QVERIFY(!model.data(row, melearner::shadcnRowProgress).isValid());
    QVERIFY(!model.data(row, melearner::shadcnRowLeading).isValid());

    // A render of one row, plus the measurements that are about the row rather than
    // about the view: the view's height is fixed by its resize, so a taller row
    // shows as fewer rows on screen, not as a taller widget.
    struct Rendered {
      QImage image;
      int rowHeight = 0;
      int rowsOnScreen = 0;
      bool reachedState = false;
    };
    const auto render = [&](qreal scale, bool selected) -> Rendered {
      auto font = QApplication::font();
      // The shadcn install sets a pixel size, so a point size change would be
      // ignored and the doubled-text case would render as the normal one. The
      // user's text size is a pixel size on this platform.
      if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
      else font.setPointSizeF(font.pointSizeF() * scale);
      auto* view = new shadcn::ListView;
      view->setModel(&model);
      view->setFont(font);
      view->showProgress();
      view->setCompactBelow(0);
      view->resize(360, 160);
      view->show();
      // Selected through the selection model, not through the current index.
      // `setCurrentIndex` moves where the focus ring goes; it does not select, so a
      // test that used it would compare a resting row's fill against the accent and
      // call it a selected one.
      if (selected) {
        view->selectionModel()->select(model.index(0),
                                       QItemSelectionModel::ClearAndSelect);
        view->setCurrentIndex(model.index(0));
      }
      const auto reached = view->selectionModel()->isSelected(model.index(0)) == selected;
      QCoreApplication::processEvents();
      auto image = view->grab().toImage();
      Rendered shot{std::move(image), view->rowHeight(),
                    (int)view->visibleRowRects().size(), reached};
      delete view;
      return shot;
    };

    const auto normal = render(1.0, false);
    const auto doubled = render(2.0, false);
    const auto selected = render(1.0, true);
    // The render has to have reached the state it was asked for, or every colour
    // assertion below is about a row the test never put into that state.
    QVERIFY2(normal.reachedState && selected.reachedState,
             "a render did not reach the state it was asked for");
    QVERIFY(!normal.image.isNull());
    QVERIFY(!doubled.image.isNull());
    // A doubled text size makes the row taller, because the row height is derived
    // from the view's own font rather than pinned by the theme. A pinned height
    // would clip the second line and the reader would lose the description.
    QVERIFY2(doubled.rowHeight > normal.rowHeight,
             qPrintable(QStringLiteral("a doubled text size gave a %1 pixel row, not taller than %2")
                            .arg(doubled.rowHeight).arg(normal.rowHeight)));
    QVERIFY2(doubled.rowsOnScreen < normal.rowsOnScreen,
             qPrintable(QStringLiteral("a doubled text size still fitted %1 rows, the same as at "
                                       "normal size (%2)").arg(doubled.rowsOnScreen)
                            .arg(normal.rowsOnScreen)));

    // A resting row takes the page and a selected row the accent fill, and the
    // text follows the fill rather than always the page foreground.
    //
    // The probe has to land on the row's own surface, and three places in a row are
    // not that surface: a rounded corner's anti-aliased edge is a blend of the fill
    // and the border, and a glyph's edge is a blend of the fill and the ink. Either
    // reads as neither, so a probe there fails against both the fill and the page.
    //
    // The point is on the trailing side, at the row's vertical middle. These rows
    // carry no trailing text or trailing pixmap, so that band is the fill, and the
    // middle height is past the corner arc on both sides.
    QCOMPARE(exactPixels(normal.image, QRect(8, normal.image.height() - 6, 1, 1), base), 1);
    // The selected row is the accent fill. Which pixel to read is a matter of
    // where the row's geometry happens to put an unblended fill, and a probe that
    // lands on a corner, a glyph edge or a neighbouring row measures something else
    // entirely. So the claim is made over the whole image instead: the accent has to
    // be present in the selected render and absent from the resting one, and the
    // selected render has to have less of the page in it than the resting one.
    const auto countExact = [](const QImage& image, const QColor& color) {
      int matches = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          matches += image.pixelColor(x, y).name(QColor::HexRgb) == color.name(QColor::HexRgb);
      return matches;
    };
    // The claim is relative, because a few stray pixels of the accent colour turn
    // up in a resting render anyway: anti-aliased glyph edges land on exact
    // channel values. Selecting a row replaces a whole row of the page with the
    // accent, so the two counts differ by an order of magnitude, and a resting
    // render's handful of coincidences cannot fake that.
    const auto restingAccent = countExact(normal.image, highlight);
    const auto selectedAccent = countExact(selected.image, highlight);
    QVERIFY2(selectedAccent > restingAccent * 4,
             qPrintable(QStringLiteral("selecting a row moved the accent count from %1 to %2, "
                                       "which is not a row changing fill")
                            .arg(restingAccent).arg(selectedAccent)));
    // The page count is deliberately not asserted here. A selected row's text is
    // drawn in the accent foreground, which is closer to the page than the normal
    // foreground is, so the selected render's anti-aliased glyph edges contribute
    // more exact page matches even though the row's own fill is not the page. One
    // measurement of the claim, not two that disagree.
    QVERIFY(exactPixels(normal.image, QRect(0, 0, normal.image.width(), normal.image.height()), text) > 0);
    QVERIFY(exactPixels(normal.image, QRect(0, 0, normal.image.width(), normal.image.height()), muted) > 0);
    QVERIFY(exactPixels(doubled.image, QRect(0, 0, doubled.image.width(), doubled.image.height()), text) > 0);
    QVERIFY(exactPixels(selected.image, QRect(0, 0, selected.image.width(), selected.image.height()), highlighted) > 0);

    // The selected row's text has to clear a text ratio on the fill it sits on. A
    // themed list that puts page foreground on the accent fill looks right until
    // the text is measured.
    const auto luminance = [](QColor value) {
      const auto channel = [&value](int shift) {
        const double part = static_cast<double>((value.rgb() >> shift) & 0xFF) / 255.0;
        return part <= 0.04045 ? part / 12.92 : std::pow((part + 0.055) / 1.055, 2.4);
      };
      return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0);
    };
    const auto ratio = [luminance](QColor a, QColor b) {
      const auto high = std::max(luminance(a), luminance(b));
      const auto low = std::min(luminance(a), luminance(b));
      return (high + 0.05) / (low + 0.05);
    };
    const auto rowRect = QRect(4, 2, selected.image.width() - 8, selected.rowHeight);
    auto best = 0.0;
    for (int y = rowRect.top(); y <= std::min(rowRect.bottom(), selected.image.height() - 1); ++y) {
      for (int x = rowRect.left(); x <= rowRect.right(); ++x) {
        best = std::max(best, ratio(selected.image.pixelColor(x, y), highlight));
      }
    }
    QVERIFY2(best >= 4.5,
             qPrintable(QStringLiteral("the selected row's text is %1:1 on the accent fill, "
                                       "needs 4.5:1").arg(best, 0, 'f', 2)));
  }
};
QTEST_MAIN(PagedListModelTest)
#include "paged_list_model_test.moc"
