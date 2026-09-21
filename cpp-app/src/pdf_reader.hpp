#pragma once
#include <QImage>
#include <QObject>
#include <QSizeF>
#include <QVector>
#include <compare>
#include <memory>
#include <variant>

namespace melearner::pdf {
struct TileKey {
  int page = 0;
  int scale = 16; // Sixteenths of one logical pixel per PDF point.
  int x = 0;
  int y = 0;
  auto operator<=>(const TileKey&) const = default;
};
struct Info { quint64 generation = 0; QVector<QSizeF> pages; };
struct Tile { quint64 generation = 0; TileKey key; QImage image; };
struct Error {
  enum Code { invalid, stale, cancelled, file, malformed, oversized, render } code;
  QString message;
};
using Result = std::variant<Info, Tile, Error>;

// PDFium is supplied by Qt PDF. All PDF and filesystem calls stay on this worker.
class PdfReader final : public QObject {
  Q_OBJECT
public:
  explicit PdfReader(QObject* parent = nullptr);
  ~PdfReader() override;
  quint64 open(QString root, QString path);
  quint64 tile(quint64 generation, TileKey key);
  // Cancels queued tiles for a document generation. A tile already in
  // QPdfDocument::render() cannot be interrupted by Qt PDF.
  void cancelTiles(quint64 generation);
  // Drops queued work and asynchronously unloads the active document after
  // any current Qt PDF call returns.
  void clear();
  void close();
signals:
  void finished(quint64 requestId, melearner::pdf::Result result);
private:
  class Worker;
  std::unique_ptr<Worker> worker_;
};
}
Q_DECLARE_METATYPE(melearner::pdf::Result)
