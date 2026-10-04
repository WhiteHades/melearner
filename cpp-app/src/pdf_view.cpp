#include "pdf_view.hpp"
#include <QAccessible>
#include <QAccessibleWidget>
#include <QAccessibleTextInterface>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace pdf = melearner::pdf;
class PdfViewAccessible final : public QAccessibleWidget, public QAccessibleTextInterface {
public:
  explicit PdfViewAccessible(PdfView* view) : QAccessibleWidget(view, QAccessible::Document), view_(view) {}
  void* interface_cast(QAccessible::InterfaceType type) override {
    if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface*>(this);
    return QAccessibleWidget::interface_cast(type);
  }
  QString text(QAccessible::Text type) const override {
    if (type == QAccessible::Name) {
      if (view_->pages_.isEmpty()) return QObject::tr("PDF pages");
      return QObject::tr("PDF page %1 of %2").arg(view_->accessiblePage_ + 1).arg(view_->pages_.size());
    }
    return QAccessibleWidget::text(type);
  }
  int selectionCount() const override { return selectionStart_ >= 0 && selectionEnd_ > selectionStart_ ? 1 : 0; }
  void selection(int index, int* start, int* end) const override {
    if (index != 0 || !selectionCount()) {
      if (start) *start = -1;
      if (end) *end = -1;
      return;
    }
    if (start) *start = selectionStart_;
    if (end) *end = selectionEnd_;
  }
  void addSelection(int start, int end) override { setSelection(0, start, end); }
  void removeSelection(int index) override {
    if (index != 0 || !selectionCount()) return;
    selectionStart_ = selectionEnd_ = -1;
    QAccessibleTextSelectionEvent event(view_, -1, -1);
    QAccessible::updateAccessibility(&event);
  }
  void setSelection(int index, int start, int end) override {
    const int count = characterCount();
    if (index != 0 || count == 0 || start < 0 || end < start || end > count) return;
    selectionStart_ = start;
    selectionEnd_ = end;
    QAccessibleTextSelectionEvent event(view_, start, end);
    QAccessible::updateAccessibility(&event);
  }
  int cursorPosition() const override { return cursorPosition_; }
  void setCursorPosition(int position) override {
    if (position < 0 || position > characterCount()) return;
    cursorPosition_ = position;
    QAccessibleTextCursorEvent event(view_, position);
    QAccessible::updateAccessibility(&event);
  }
  void resetForPageChange() {
    const bool hadSelection = selectionCount() != 0;
    const bool hadCursor = cursorPosition_ != 0;
    selectionStart_ = selectionEnd_ = -1;
    cursorPosition_ = 0;
    if (hadSelection) {
      QAccessibleTextSelectionEvent event(view_, -1, -1);
      QAccessible::updateAccessibility(&event);
    }
    if (hadCursor) {
      QAccessibleTextCursorEvent event(view_, 0);
      QAccessible::updateAccessibility(&event);
    }
  }
  QString text(int start, int end) const override {
    const int count = characterCount();
    if (start < 0) start = 0;
    if (end < 0 || end > count) end = count;
    if (start >= end || start >= count) return {};
    return view_->pageText_.mid(start, end - start);
  }
  int characterCount() const override { return view_->pageText_.size(); }
  QRect characterRect(int offset) const override {
    if (offset < 0 || view_->accessiblePage_ < 0 || view_->accessiblePage_ >= view_->pages_.size() ||
        offset >= view_->pageText_.size()) return {};
    const auto* cachedBounds = view_->pageCharacterBounds_.object(offset);
    if (!cachedBounds) {
      view_->requestCharacterBounds(offset);
      return {};
    }
    const auto bounds = *cachedBounds;
    if (!bounds.isValid() || bounds.isEmpty()) return {};
    const qreal factor = view_->scale_ / 16.0;
    const qreal pageWidth = view_->pages_[view_->accessiblePage_].width() * factor;
    const int x = std::max(12, (view_->viewport()->width() - qCeil(pageWidth)) / 2) - view_->horizontalScrollBar()->value();
    const int y = view_->tops_[view_->accessiblePage_] - view_->verticalScrollBar()->value();
    const auto localRect = QRectF(bounds.x() * factor + x, bounds.y() * factor + y,
                                  bounds.width() * factor, bounds.height() * factor).toAlignedRect();
    return localRect.translated(view_->viewport()->mapToGlobal(QPoint()));
  }
  int offsetAtPoint(const QPoint& point) const override {
    for (const int index : view_->pageCharacterBounds_.keys()) {
      if (characterRect(index).contains(point)) return index;
    }
    return -1;
  }
  void scrollToSubstring(int start, int end) override {
    if (start < 0 || end < start || end > characterCount() || view_->accessiblePage_ < 0) return;
    view_->jumpToPage(view_->accessiblePage_ + 1);
  }
  QString attributes(int offset, int* start, int* end) const override {
    if (offset < 0 || offset > characterCount()) {
      if (start) *start = -1;
      if (end) *end = -1;
      return {};
    }
    if (start) *start = 0;
    if (end) *end = characterCount();
    return {};
  }
private:
  PdfView* view_;
  int cursorPosition_ = -1;
  int selectionStart_ = -1;
  int selectionEnd_ = -1;
};

namespace {
QAccessibleInterface* pdfViewAccessibleFactory(const QString&, QObject* object) {
  if (auto* view = qobject_cast<PdfView*>(object)) return new PdfViewAccessible(view);
  return nullptr;
}
}

PdfView::PdfView(QWidget* parent) : shadcn::ScrollArea(parent) {
  static const bool accessibleFactoryInstalled = [] { QAccessible::installFactory(pdfViewAccessibleFactory); return true; }();
  Q_UNUSED(accessibleFactoryInstalled);
  setObjectName("pdfPages"); setAccessibleName(tr("PDF pages"));
  setFocusPolicy(Qt::StrongFocus); setFrameShape(QFrame::NoFrame);
  connect(&reader_, &pdf::PdfReader::finished, this, [this](quint64 id, const pdf::Result& result) {
    // Each reply frees a reader credit, including a superseded render. Retry
    // visible tiles that could not be queued while the reader was full.
    if (generation_) viewport()->update();
    if (const auto* info = std::get_if<pdf::Info>(&result)) {
      if (id != openId_) return;
      generation_ = info->generation; pages_ = info->pages; layoutPages();
      viewport()->update();
    } else if (const auto* tile = std::get_if<pdf::Tile>(&result)) {
      const auto pending = pending_.find(id);
      if (pending == pending_.end()) return;
      pendingKeys_.remove(pending.value());
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
    } else if (const auto* text = std::get_if<pdf::PageText>(&result)) {
      if (id != pageTextRequest_ || text->generation != generation_ || text->page != accessiblePage_) return;
      pageTextRequest_ = 0;
      pageTextPending_ = false;
      pageTextLoaded_ = true;
      const auto previous = pageText_;
      pageText_ = text->text;
      QAccessibleTextUpdateEvent textEvent(this, 0, previous, pageText_);
      QAccessible::updateAccessibility(&textEvent);
      QAccessibleEvent nameEvent(this, QAccessible::NameChanged);
      QAccessible::updateAccessibility(&nameEvent);
    } else if (const auto* bounds = std::get_if<pdf::CharacterBounds>(&result)) {
      if (id != characterBoundsRequest_ || bounds->generation != generation_ || bounds->page != accessiblePage_ ||
          bounds->offset != characterBoundsRequestOffset_) return;
      characterBoundsRequest_ = 0;
      characterBoundsRequestOffset_ = -1;
      pageCharacterBounds_.insert(bounds->offset, new QRectF(bounds->bounds));
      QAccessibleEvent locationEvent(this, QAccessible::LocationChanged);
      QAccessible::updateAccessibility(&locationEvent);
    } else if (const auto* error = std::get_if<pdf::Error>(&result)) {
      if (id == pageTextRequest_) {
        pageTextRequest_ = 0;
        pageTextPending_ = false;
        pageTextLoaded_ = true;
      } else {
        if (id == characterBoundsRequest_) {
          characterBoundsRequest_ = 0;
          characterBoundsRequestOffset_ = -1;
        }
        const auto pending = pending_.find(id);
        if (pending != pending_.end()) {
          pendingKeys_.remove(pending.value());
          const auto key = pending.value();
          pending_.erase(pending);
          if (error->code != pdf::Error::cancelled) rememberFailedTile(key);
        } else if (id != openId_) {
          return;
        }
        if (error->code != pdf::Error::stale && error->code != pdf::Error::cancelled) {
          emit errorOccurred(error->message);
          viewport()->update();
        }
      }
    }
    if (accessiblePage_ >= 0 && generation_ && !pageTextPending_ && !pageTextLoaded_) requestAccessiblePageText();
  });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    updateAccessiblePage(currentPage());
    emit pageChanged(currentPage() + 1, pages_.size()); viewport()->update();
  });
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}
void PdfView::clear() {
  updateAccessiblePage(-1);
  openId_ = 0; generation_ = 0; pages_.clear(); tops_.clear(); cache_.clear(); pending_.clear(); pendingKeys_.clear();
  failedTiles_.clear();
  verticalScrollBar()->setRange(0, 0); horizontalScrollBar()->setRange(0, 0); viewport()->update();
  reader_.clear();
}
void PdfView::updateAccessiblePage(int page) {
  if (page == accessiblePage_) return;
  const auto previous = pageText_;
  reader_.cancelPageText(generation_);
  accessiblePage_ = page;
  pageTextRequest_ = 0;
  pageTextPending_ = false;
  pageTextLoaded_ = false;
  pageText_.clear();
  pageCharacterBounds_.clear();
  characterBoundsRequest_ = 0;
  characterBoundsRequestOffset_ = -1;
  if (auto* accessible = dynamic_cast<PdfViewAccessible*>(QAccessible::queryAccessibleInterface(this)))
    accessible->resetForPageChange();
  QAccessibleEvent nameEvent(this, QAccessible::NameChanged);
  QAccessible::updateAccessibility(&nameEvent);
  if (!previous.isEmpty()) {
    QAccessibleTextUpdateEvent textEvent(this, 0, previous, QString());
    QAccessible::updateAccessibility(&textEvent);
  }
  if (page >= 0 && generation_) requestAccessiblePageText();
}
void PdfView::requestAccessiblePageText() {
  if (pageTextPending_ || pageTextLoaded_ || accessiblePage_ < 0 || !generation_) return;
  pageTextRequest_ = reader_.pageText(generation_, accessiblePage_);
  pageTextPending_ = pageTextRequest_ != 0;
}
void PdfView::requestCharacterBounds(int offset) {
  if (characterBoundsRequest_ || !pageTextLoaded_ || accessiblePage_ < 0 || offset < 0 || offset >= pageText_.size() ||
      pageCharacterBounds_.contains(offset)) return;
  characterBoundsRequestOffset_ = offset;
  characterBoundsRequest_ = reader_.characterBounds(generation_, accessiblePage_, offset);
  if (!characterBoundsRequest_) characterBoundsRequestOffset_ = -1;
}
void PdfView::open(const QString& root, const QString& path) {
  clear(); fit_ = true; openId_ = reader_.open(root, path);
  if (!openId_) emit errorOccurred(tr("PDF reader is busy. Select the lesson again to retry."));
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
  // A tile is keyed by its page and its scale, so a resize that leaves the scale
  // alone leaves every cached tile valid. Dropping them on every resize event
  // re-rendered every visible tile for each pixel of a window drag; the reader
  // only re-renders when the scale or the document actually changed.
  if (scale_ != previousScale) {
    cache_.clear(); pending_.clear(); pendingKeys_.clear(); failedTiles_.clear();
    reader_.cancelTiles(generation_);
  }
  viewport()->update();
  updateAccessiblePage(currentPage());
  requestAccessiblePageText();
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
void PdfView::resizeEvent(QResizeEvent* event) { shadcn::ScrollArea::resizeEvent(event); layoutPages(); }
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
        } else if (!failedTiles_.contains(key) && !pendingKeys_.contains(key)) {
          const auto id = reader_.tile(generation_, key);
          if (id) { pending_.insert(id, key); pendingKeys_.insert(key); }
        }
      }
    }
  }
}
