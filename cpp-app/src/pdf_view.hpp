#pragma once
#include "pdf_reader.hpp"
#include <shadcn/navigation.hpp>
#include <QCache>
#include <QMap>
#include <QSet>

class PdfViewAccessible;

class PdfView final : public shadcn::ScrollArea {
  Q_OBJECT
public:
  explicit PdfView(QWidget* parent = nullptr);
  void open(const QString& root, const QString& path);
  void clear();
  void fitWidth();
  void setZoom(double zoom);
  void jumpToPage(int page); // One-based for the learner-facing controls.
  int cachedTiles() const { return cache_.size(); }
  // The render scale, in sixteenths of a point. A tile is keyed by it, so a
  // relayout at the same scale keeps every cached tile.
  int zoomForTesting() const { return scale_; }
signals:
  void errorOccurred(QString message);
  void pageChanged(int current, int total);
protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
private:
  friend class PdfViewAccessible;
  melearner::pdf::PdfReader reader_;
  QVector<QSizeF> pages_;
  QVector<int> tops_;
  quint64 generation_ = 0;
  quint64 openId_ = 0;
  quint64 clock_ = 0;
  int scale_ = 16;
  bool fit_ = true;
  struct Cached { QImage image; quint64 used; };
  QMap<melearner::pdf::TileKey, Cached> cache_;
  // A tile is in flight if its key appears in pending_; the paint loop tests
  // that per uncached tile, so a set answers it without copying the map.
  QMap<quint64, melearner::pdf::TileKey> pending_;
  QSet<melearner::pdf::TileKey> pendingKeys_;
  QMap<melearner::pdf::TileKey, quint64> failedTiles_;
  int accessiblePage_ = -1;
  quint64 pageTextRequest_ = 0;
  bool pageTextPending_ = false;
  bool pageTextLoaded_ = false;
  QString pageText_;
  QCache<int, QRectF> pageCharacterBounds_{256};
  quint64 characterBoundsRequest_ = 0;
  int characterBoundsRequestOffset_ = -1;
  void layoutPages(int previousScale = -1);
  void rememberFailedTile(const melearner::pdf::TileKey& key);
  void updateAccessiblePage(int page);
  void requestAccessiblePageText();
  void requestCharacterBounds(int offset);
  int currentPage() const;
};
