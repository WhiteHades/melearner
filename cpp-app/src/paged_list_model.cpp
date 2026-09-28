#include "paged_list_model.hpp"
#include "library.hpp"
#include "study_icons.hpp"
#include "theme.hpp"
#include <QApplication>
#include <QPainter>
#include <QSize>
#include <QStyle>
#include <QTreeView>
#include <algorithm>

namespace {

/// The side a row icon is rendered at. It matches the delegate's icon box, so the
/// pixmap is not scaled on the way to the screen.
constexpr int kRowIconSide = 30;

struct RowIconSet {
  QPixmap present;
  QPixmap missing;
  QPixmap chevron;
};

RowIconSet& rowIcons() {
  static RowIconSet icons;
  return icons;
}

/// Scale whichever size the font carries. The shadcn install sets a pixel size,
/// so a point-size scale is silently ignored and every heading in the list
/// renders at the body size.
void scaleFont(QFont& font, double factor) {
  if (font.pixelSize() > 0) font.setPixelSize(std::max(1, qRound(font.pixelSize() * factor)));
  else font.setPointSizeF(std::max(1.0, font.pointSizeF() * factor));
}

}  // namespace

/// The icons a course row carries, rebuilt when the theme moves.
///
/// The themed row delegate draws a leading pixmap as given, so a caller supplies
/// data rather than widgets. That makes the colour of a row's icon the model's to
/// know, and the model has to hear about a theme change to redraw it. The
/// alternative is a delegate that knows what a course icon is, which is the
/// application leaking into the drawing.
void refreshRowIcons() {
  const auto& theme = melearner::themeFor(nullptr);
  rowIcons().present = melearner::studyIcon(melearner::StudyIcon::Courses,
    melearner::roleColor(theme, shadcn::Role::Primary)).pixmap(kRowIconSide);
  // A course whose folder is missing is drawn in the muted colour, because it is
  // in the Library and cannot be opened.
  rowIcons().missing = melearner::studyIcon(melearner::StudyIcon::Courses,
    melearner::roleColor(theme, shadcn::Role::MutedForeground)).pixmap(kRowIconSide);
  rowIcons().chevron = melearner::studyIcon(melearner::StudyIcon::ChevronRight,
    melearner::roleColor(theme, shadcn::Role::Foreground)).pixmap(kRowIconSide);
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
  // The themed row delegate reads the description and the progress roles, so a row
  // is described by data rather than by a newline the delegate has to split. A
  // title containing a newline is no longer a row that silently gains a line.
  if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole && role != Qt::ToolTipRole
      && role != Qt::UserRole && role != melearner::shadcnRowDescription
      && role != melearner::shadcnRowProgress && role != melearner::shadcnRowLeading
      && role != melearner::shadcnRowTrailing) return {};
  const auto item = row(index.row());
  if (!item) return role == Qt::UserRole ? QVariant() : QVariant(tr("Loading…"));
  if (role == Qt::UserRole) return item->id;
  const auto course = item->value.canConvert<melearner::library::Course>()
    ? std::optional<melearner::library::Course>(item->value.value<melearner::library::Course>())
    : std::nullopt;
  if (role == Qt::AccessibleTextRole) {
    return item->title + ", " + item->description + (item->available ? "" : tr(", missing"));
  }
  if (role == Qt::ToolTipRole) {
    return QString("<qt>%1<br>%2</qt>").arg(item->title.toHtmlEscaped(), item->description.toHtmlEscaped());
  }
  if (role == melearner::shadcnRowDescription) return item->description;
  if (role == melearner::shadcnRowProgress) {
    if (!course || course->missing || course->lessonCount <= 0) return {};
    return std::min(1.0, static_cast<double>(course->completedLessons) / course->lessonCount);
  }
  if (role == melearner::shadcnRowLeading) {
    // A course whose folder is missing is drawn in the muted colour, because it is
    // in the Library and cannot be opened.
    if (!course) return {};
    return course->missing ? rowIcons().missing : rowIcons().present;
  }
  if (role == melearner::shadcnRowTrailing) {
    if (!course) return {};
    return rowIcons().chevron;
  }
  return item->title;
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
void PagedListModel::refreshRowIcons() {
  // Only the rows that are resident change, and only their icon roles. The
  // viewport repaints what is on screen, which is what the reader can see.
  for (auto page = pages_.begin(); page != pages_.end(); ++page) {
    if (page->rows.isEmpty()) continue;
    const auto changed = index(page.key());
    emit dataChanged(changed, index(page.key() + page->rows.size() - 1),
                     {melearner::shadcnRowLeading, melearner::shadcnRowTrailing});
  }
}
int PagedListModel::cachedRows() const {
  int count = 0;
  for (const auto& page : pages_) count += page.rows.size();
  return count;
}
