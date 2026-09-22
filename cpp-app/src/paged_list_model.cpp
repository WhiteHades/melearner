#include "paged_list_model.hpp"
#include <QSize>
#include <algorithm>

QSize StudyItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
  auto size = QStyledItemDelegate::sizeHint(option, index);
  size.setHeight(std::max(compact_ ? 52 : 68, option.fontMetrics.lineSpacing() * 2 + (compact_ ? 16 : 20))); return size;
}

PagedListModel::PagedListModel(int pageSize, QObject* parent)
    : QAbstractListModel(parent), pageSize_(pageSize) { Q_ASSERT(pageSize > 0 && pageSize <= 256); }
int PagedListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : total_; }
void PagedListModel::request(int offset) const {
  if (pending_.contains(offset)) return;
  if (pending_.size() >= 4) { deferred_ = offset; return; }
  pending_.insert(offset);
  auto* self = const_cast<PagedListModel*>(this);
  QMetaObject::invokeMethod(self, [self, offset, generation = generation_] {
    if (self->generation_ == generation && self->pending_.contains(offset)) emit self->pageRequested(offset);
  }, Qt::QueuedConnection);
}
std::optional<StudyRow> PagedListModel::row(int index) const {
  if (index < 0 || index >= total_) return {};
  const int offset = index / pageSize_ * pageSize_;
  auto page = pages_.find(offset);
  if (page == pages_.end()) { request(offset); return {}; }
  page->used = ++clock_;
  const int local = index - offset;
  if (local >= page->rows.size()) return {};
  return page->rows[local];
}
QVariant PagedListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= total_) return {};
  if (role == Qt::SizeHintRole) return QSize(180, 68);
  if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole && role != Qt::ToolTipRole && role != Qt::UserRole) return {};
  const auto item = row(index.row());
  if (!item) return role == Qt::UserRole ? QVariant() : QVariant(tr("Loading…"));
  if (role == Qt::UserRole) return item->id;
  if (role == Qt::AccessibleTextRole) return item->title + ", " + item->description + (item->available ? "" : tr(", missing"));
  if (role == Qt::ToolTipRole) return QString("<qt>%1<br>%2</qt>").arg(item->title.toHtmlEscaped(), item->description.toHtmlEscaped());
  return item->title + '\n' + item->description;
}
Qt::ItemFlags PagedListModel::flags(const QModelIndex& index) const {
  return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}
void PagedListModel::reset() {
  beginResetModel();
  ++generation_; total_ = 0; pages_.clear(); pending_.clear(); deferred_.reset();
  endResetModel();
  pending_.insert(0);
  emit pageRequested(0);
}
bool PagedListModel::setPage(int offset, int total, const QList<StudyRow>& rows) {
  if (offset < 0 || offset % pageSize_ || total < 0 || rows.size() > pageSize_ ||
      offset > total || rows.size() != std::min(pageSize_, total - offset)) return false;
  pending_.remove(offset);
  const bool resize = total != total_;
  if (resize) beginResetModel();
  total_ = total;
  pages_.insert(offset, {rows, ++clock_});
  while (pages_.size() > 4) {
    auto oldest = std::min_element(pages_.begin(), pages_.end(), [](const Page& a, const Page& b) { return a.used < b.used; });
    pages_.erase(oldest);
  }
  if (resize) endResetModel();
  else if (!rows.isEmpty()) emit dataChanged(index(offset), index(offset + rows.size() - 1));
  if (deferred_ && pending_.size() < 4) {
    const int deferred = *deferred_; deferred_.reset();
    if (deferred < total_ && !pages_.contains(deferred)) request(deferred);
  }
  return true;
}
void PagedListModel::failedPage(int offset) { pending_.remove(offset); }
bool PagedListModel::updateRow(const StudyRow& row) {
  for (auto page = pages_.begin(); page != pages_.end(); ++page) {
    for (int local = 0; local < page->rows.size(); ++local) {
      if (page->rows[local].id != row.id) continue;
      page->rows[local] = row;
      const auto changed = index(page.key() + local); emit dataChanged(changed, changed); return true;
    }
  }
  return false;
}
int PagedListModel::cachedRows() const {
  int count = 0;
  for (const auto& page : pages_) count += page.rows.size();
  return count;
}
