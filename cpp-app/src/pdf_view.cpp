#include "pdf_view.hpp"
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace pdf = melearner::pdf;
PdfView::PdfView(QWidget* parent) : QAbstractScrollArea(parent) {
  setObjectName("pdfPages"); setAccessibleName(tr("PDF pages"));
  setFocusPolicy(Qt::StrongFocus); setFrameShape(QFrame::NoFrame);
  connect(&reader_, &pdf::PdfReader::finished, this, [this](quint64 id, const pdf::Result& result) {
    if (const auto* info = std::get_if<pdf::Info>(&result)) {
      if (id != openId_) return;
      generation_ = info->generation; pages_ = info->pages; layoutPages();
      emit statusChanged(tr("%1 pages").arg(pages_.size())); viewport()->update();
    } else if (const auto* tile = std::get_if<pdf::Tile>(&result)) {
      const auto pending = pending_.find(id);
      if (pending == pending_.end()) return;
      const auto expected = pending.value();
      pending_.erase(pending);
      if (tile->generation != generation_ || tile->key != expected || tile->key.scale != scale_) {
        rememberFailedTile(expected);
        return;
      }
      failedTiles_.remove(tile->key);
      cache_.insert(tile->key, {tile->image, ++clock_});
      while (cache_.size() > 64) {
        const auto oldest = std::min_element(cache_.begin(), cache_.end(), [](const auto& left, const auto& right) { return left.used < right.used; });
        cache_.erase(oldest);
      }
      viewport()->update();
    } else if (const auto* error = std::get_if<pdf::Error>(&result)) {
      const auto pending = pending_.find(id);
      if (pending != pending_.end()) {
        const auto key = pending.value();
        pending_.erase(pending);
        if (error->code != pdf::Error::cancelled) {
          rememberFailedTile(key);
        }
      } else if (id != openId_) {
        return;
      }
      if (error->code != pdf::Error::stale && error->code != pdf::Error::cancelled) {
        emit statusChanged(error->message);
        viewport()->update();
      }
    }
  });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    emit pageChanged(currentPage() + 1, pages_.size()); viewport()->update();
  });
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}
void PdfView::clear() {
  openId_ = 0; generation_ = 0; pages_.clear(); tops_.clear(); cache_.clear(); pending_.clear();
  failedTiles_.clear();
  verticalScrollBar()->setRange(0, 0); horizontalScrollBar()->setRange(0, 0); viewport()->update();
  reader_.clear();
}
void PdfView::open(const QString& root, const QString& path) {
  clear(); fit_ = true; openId_ = reader_.open(root, path);
  emit statusChanged(openId_ ? tr("Opening PDF…") : tr("PDF reader is busy. Select the lesson again to retry."));
}
int PdfView::currentPage() const {
  if (tops_.isEmpty()) return 0;
  return std::max(0, static_cast<int>(std::upper_bound(tops_.begin(), tops_.end(), verticalScrollBar()->value()) - tops_.begin()) - 1);
}
void PdfView::rememberFailedTile(const pdf::TileKey& key) {
  failedTiles_.insert(key, ++clock_);
  while (failedTiles_.size() > 64) {
    const auto oldest = std::min_element(
        failedTiles_.begin(), failedTiles_.end(),
        [](quint64 left, quint64 right) { return left < right; });
    failedTiles_.erase(oldest);
  }
}
void PdfView::layoutPages(int previousScaleOverride) {
  if (pages_.isEmpty()) return;
  const int previousScale = previousScaleOverride > 0 ? previousScaleOverride : scale_;
  const int previousPage = currentPage();
  const bool hadPreviousLayout = !tops_.isEmpty() && previousPage >= 0 &&
      previousPage < pages_.size();
  double pageOffset = 0.0;
  if (hadPreviousLayout) {
    const auto previousHeight = pages_[previousPage].height() * previousScale / 16.0;
    if (previousHeight > 0.0) {
      pageOffset = std::clamp(
          (verticalScrollBar()->value() - tops_[previousPage]) / previousHeight,
          0.0, 1.0);
    }
  }
  const int page = std::min(previousPage, static_cast<int>(pages_.size()) - 1);
  if (fit_) scale_ = std::clamp(qFloor((viewport()->width() - 24) * 16.0 / pages_[page].width()), 4, 64);
  tops_.clear(); int top = 12; int widest = 0;
  for (const auto& size : pages_) {
    tops_.append(top); top += qCeil(size.height() * scale_ / 16.0) + 16;
    widest = std::max(widest, qCeil(size.width() * scale_ / 16.0));
  }
  const QSignalBlocker verticalSignals(verticalScrollBar());
  const QSignalBlocker horizontalSignals(horizontalScrollBar());
  verticalScrollBar()->setPageStep(viewport()->height()); verticalScrollBar()->setSingleStep(32);
  // A short final page must still be able to align with the viewport's top.
  verticalScrollBar()->setRange(0, std::max(tops_.last(), top - viewport()->height()));
  horizontalScrollBar()->setPageStep(viewport()->width());
  horizontalScrollBar()->setRange(0, std::max(0, widest + 24 - viewport()->width()));
  if (hadPreviousLayout) {
    const auto newHeight = pages_[page].height() * scale_ / 16.0;
    const auto target = tops_[page] + qRound(pageOffset * newHeight);
    verticalScrollBar()->setValue(std::clamp(
        target, verticalScrollBar()->minimum(), verticalScrollBar()->maximum()));
  }
  cache_.clear(); pending_.clear(); failedTiles_.clear(); viewport()->update();
  reader_.cancelTiles(generation_);
  emit pageChanged(currentPage() + 1, pages_.size());
}
void PdfView::fitWidth() { fit_ = true; layoutPages(); }
void PdfView::setZoom(double zoom) {
  if (!std::isfinite(zoom)) return;
  const int previousScale = scale_;
  fit_ = false; scale_ = std::clamp(qRound(zoom * 16), 4, 64); layoutPages(previousScale);
}
void PdfView::jumpToPage(int page) {
  if (page >= 1 && page <= tops_.size()) verticalScrollBar()->setValue(tops_[page - 1]);
}
void PdfView::resizeEvent(QResizeEvent* event) { QAbstractScrollArea::resizeEvent(event); layoutPages(); }
void PdfView::paintEvent(QPaintEvent*) {
  QPainter painter(viewport()); painter.fillRect(viewport()->rect(), palette().window());
  if (!generation_) return;
  const int scrollY = verticalScrollBar()->value();
  for (int page = currentPage(); page < pages_.size() && tops_[page] < scrollY + viewport()->height(); ++page) {
    const QSize size(qCeil(pages_[page].width() * scale_ / 16.0), qCeil(pages_[page].height() * scale_ / 16.0));
    const QPoint origin(std::max(12, (viewport()->width() - size.width()) / 2) - horizontalScrollBar()->value(), tops_[page] - scrollY);
    const QRect pageRect(origin, size); painter.fillRect(pageRect, Qt::white);
    const QRect visible = pageRect.intersected(viewport()->rect()).translated(-origin);
    if (visible.isEmpty()) continue;
    for (int y = visible.top() / 512; y <= visible.bottom() / 512; ++y) {
      for (int x = visible.left() / 512; x <= visible.right() / 512; ++x) {
        const pdf::TileKey key{page, scale_, x, y};
        auto cached = cache_.find(key);
        if (cached != cache_.end()) {
          cached->used = ++clock_; painter.drawImage(origin + QPoint(x * 512, y * 512), cached->image);
        } else if (!failedTiles_.contains(key) && !pending_.values().contains(key)) {
          const auto id = reader_.tile(generation_, key); if (id) pending_.insert(id, key);
        }
      }
    }
  }
}
