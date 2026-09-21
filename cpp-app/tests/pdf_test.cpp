#include "pdf_reader.hpp"
#include "pdf_view.hpp"
#include <QFile>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace pdf = melearner::pdf;

QString makePdf(const QString& directory, const QString& name, int pageCount) {
  const auto path = directory + "/" + name;
  QPdfWriter writer(path); writer.setResolution(72);
  QPainter painter(&writer);
  if (!painter.isActive()) return {};
  for (int page = 0; page < pageCount; ++page) {
    if (page && !writer.newPage()) return {};
    painter.fillRect(0, 0, 300, 300, Qt::red);
    painter.drawText(30, 350, QString("Page %1").arg(page + 1));
  }
  painter.end();
  return path;
}

class PdfTest final : public QObject {
  Q_OBJECT
private slots:
  void rendersTilesAndBoundsCache() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto path = data.path() + "/lesson.pdf";
    {
      QPdfWriter writer(path); writer.setResolution(72);
      QPainter painter(&writer);
      for (int page = 0; page < 500; ++page) {
        if (page) QVERIFY(writer.newPage());
        painter.fillRect(0, 0, 300, 300, Qt::red);
        painter.drawText(30, 350, QString("Page %1").arg(page + 1));
      }
    }
    pdf::PdfReader reader; QSignalSpy results(&reader, &pdf::PdfReader::finished);
    const auto generation = reader.open(data.path(), path); QVERIFY(generation);
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 10000);
    auto result = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Info>(result));
    QCOMPARE(std::get<pdf::Info>(result).pages.size(), 500);
    QVERIFY(reader.tile(generation, {499, 16, 0, 0}));
    QTRY_COMPARE(results.count(), 1);
    result = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Tile>(result));
    const auto image = std::get<pdf::Tile>(result).image;
    QVERIFY(image.width() <= 512 && image.height() <= 512);
    QVERIFY(image.pixelColor(100, 100).red() > 200);
    QVERIFY(image.pixelColor(100, 100).green() < 100);
    QVERIFY(reader.tile(generation + 1, {0, 16, 0, 0})); QTRY_COMPARE(results.count(), 1);
    result = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QCOMPARE(std::get<pdf::Error>(result).code, pdf::Error::stale);
    PdfView view; view.resize(600, 500); view.show(); view.open(data.path(), path);
    QTRY_VERIFY_WITH_TIMEOUT(view.cachedTiles() > 0, 10000);
    for (int page = 1; page <= 80; ++page) {
      view.jumpToPage(page); QTest::qWait(10); QVERIFY(view.cachedTiles() <= 64);
    }
    view.setZoom(2); view.jumpToPage(500); QTRY_VERIFY(view.cachedTiles() > 0);
    QVERIFY(view.cachedTiles() <= 64);
    view.clear(); QCOMPARE(view.cachedTiles(), 0);
    reader.close();
  }

  void queuedCancellationEmitsOneTerminalPerAcceptedRequest() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto path = makePdf(data.path(), "queued.pdf", 1); QVERIFY(!path.isEmpty());

    pdf::PdfReader reader; QSignalSpy results(&reader, &pdf::PdfReader::finished);
    const auto openId = reader.open(data.path(), path); QVERIFY(openId);
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 10000);
    const auto info = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Info>(info));
    const auto generation = std::get<pdf::Info>(info).generation;

    QVector<quint64> accepted;
    accepted.reserve(64);
    for (int index = 0; index < 64; ++index) {
      if (const auto id = reader.tile(generation, {0, 16, 0, 0}); id != 0) {
        accepted.append(id);
      }
    }
    QVERIFY(accepted.size() > 1);
    reader.clear();
    const int acceptedCount = accepted.size();
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), acceptedCount, 10000);

    QHash<quint64, int> seen;
    int cancelled = 0;
    for (const auto& event : results) {
      const auto requestId = event.at(0).toULongLong();
      ++seen[requestId];
      const auto result = qvariant_cast<pdf::Result>(event.at(1));
      if (const auto* error = std::get_if<pdf::Error>(&result)) {
        if (error->code == pdf::Error::cancelled) ++cancelled;
      }
    }
    QCOMPARE(seen.size(), acceptedCount);
    for (const auto requestId : accepted) QCOMPARE(seen.value(requestId), 1);
    QVERIFY(cancelled > 0);
    reader.close();
  }

  void clearInvalidatesTheActiveGeneration() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto path = makePdf(data.path(), "clear.pdf", 1); QVERIFY(!path.isEmpty());

    pdf::PdfReader reader; QSignalSpy results(&reader, &pdf::PdfReader::finished);
    QVERIFY(reader.open(data.path(), path));
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 10000);
    const auto info = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Info>(info));
    const auto generation = std::get<pdf::Info>(info).generation;

    reader.clear();
    const auto requestId = reader.tile(generation, {0, 16, 0, 0});
    QVERIFY(requestId);
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 5000);
    const auto result = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Error>(result));
    QCOMPARE(std::get<pdf::Error>(result).code, pdf::Error::stale);
    reader.close();
  }

  void rapidSelectionRelayoutKeepsCurrentDocumentAndBoundsCache() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto first = makePdf(data.path(), "first.pdf", 2); QVERIFY(!first.isEmpty());
    const auto second = makePdf(data.path(), "second.pdf", 4); QVERIFY(!second.isEmpty());

    PdfView view; view.resize(700, 500); view.show();
    QSignalSpy pages(&view, &PdfView::pageChanged);
    for (int index = 0; index < 8; ++index) {
      view.open(data.path(), first);
      view.open(data.path(), second);
    }
    auto sawSecond = [&pages] {
      return std::any_of(pages.cbegin(), pages.cend(), [](const auto& event) {
        return event.at(1).toInt() == 4;
      });
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawSecond(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(view.cachedTiles() > 0, 10000);
    QVERIFY(view.cachedTiles() <= 64);

    view.jumpToPage(3);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(pages.cbegin(), pages.cend(), [](const auto& event) {
      return event.at(0).toInt() == 3 && event.at(1).toInt() == 4;
    }), 5000);
    pages.clear();
    view.setZoom(2.0);
    view.resize(520, 420);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(pages.cbegin(), pages.cend(), [](const auto& event) {
      return event.at(0).toInt() == 3 && event.at(1).toInt() == 4;
    }), 5000);
    QVERIFY(view.cachedTiles() <= 64);

    for (int index = 0; index < 24; ++index) {
      view.resize(500 + (index % 4) * 80, 400 + (index % 3) * 60);
      view.setZoom(0.75 + (index % 4) * 0.25);
      view.jumpToPage(index % 4 + 1);
      QTest::qWait(2);
      QVERIFY(view.cachedTiles() <= 64);
    }
    view.clear();
    QCOMPARE(view.cachedTiles(), 0);
  }

  void rejectsSparsePdfAboveTheFileLimit() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto path = data.path() + "/oversized.pdf";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(2LL * 1024 * 1024 * 1024 + 1));
    file.close();

    pdf::PdfReader reader; QSignalSpy results(&reader, &pdf::PdfReader::finished);
    QVERIFY(reader.open(data.path(), path));
    QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 10000);
    const auto result = qvariant_cast<pdf::Result>(results.takeFirst().at(1));
    QVERIFY(std::holds_alternative<pdf::Error>(result));
    QCOMPARE(std::get<pdf::Error>(result).code, pdf::Error::oversized);
    reader.close();
  }
};
QTEST_MAIN(PdfTest)
#include "pdf_test.moc"
