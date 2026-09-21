#pragma once
#include "pdf_reader.hpp"
#include <QAbstractScrollArea>
#include <QMap>

class PdfView final : public QAbstractScrollArea {
  Q_OBJECT
public:
  explicit PdfView(QWidget* parent = nullptr);
  void open(const QString& root, const QString& path);
  void clear();
  void fitWidth();
  void setZoom(double zoom);
  void jumpToPage(int page); // One-based for the learner-facing controls.
  int cachedTiles() const { return cache_.size(); }
signals:
  void statusChanged(QString message);
  void pageChanged(int current, int total);
protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
private:
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
  QMap<quint64, melearner::pdf::TileKey> pending_;
  QMap<melearner::pdf::TileKey, quint64> failedTiles_;
  void layoutPages(int previousScale = -1);
  void rememberFailedTile(const melearner::pdf::TileKey& key);
  int currentPage() const;
};
