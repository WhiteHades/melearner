#include "search_dialog.hpp"
#include "paged_list_model.hpp"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QVBoxLayout>
#include <limits>

namespace lib = melearner::library;

SearchDialog::SearchDialog(lib::Library& library, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Search your Library")); resize(620, 480);
  auto* layout = new QVBoxLayout(this);
  query_ = new QLineEdit; query_->setObjectName("searchQuery");
  query_->setPlaceholderText(tr("Search courses, sections, and lessons"));
  query_->setAccessibleName(tr("Search your Library")); query_->setMinimumHeight(40);
  query_->setMaxLength(512); query_->installEventFilter(this); layout->addWidget(query_);
  model_ = new PagedListModel(100, this);
  results_ = new QListView; results_->setObjectName("searchResults");
  results_->setAccessibleName(tr("Search results")); results_->setModel(model_);
  results_->setItemDelegate(new StudyItemDelegate(results_));
  results_->setUniformItemSizes(true); results_->setTextElideMode(Qt::ElideRight);
  results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); layout->addWidget(results_, 1);
  status_ = new QLabel(tr("Type a name to search.")); status_->setWordWrap(true); layout->addWidget(status_);
  status_->setTextFormat(Qt::PlainText);
  debounce_.setSingleShot(true); debounce_.setInterval(100);
  connect(query_, &QLineEdit::textChanged, this, [this] {
    ++generation_; requests_.clear(); submittedQuery_.clear(); model_->reset(); debounce_.start();
    status_->setText(query_->text().trimmed().isEmpty() ? tr("Type a name to search.") : tr("Searching…"));
  });
  connect(&debounce_, &QTimer::timeout, this, [this] {
    submittedQuery_ = query_->text().trimmed();
    status_->setText(submittedQuery_.isEmpty() ? tr("Type a name to search.") : tr("Searching…"));
    model_->reset();
  });
  connect(model_, &PagedListModel::pageRequested, this, [this, &library](int offset) {
    if (submittedQuery_.isEmpty()) { model_->setPage(0, 0, {}); return; }
    const auto id = library.search(submittedQuery_, offset);
    if (id) requests_.insert(id, {generation_, offset});
    else { model_->failedPage(offset); status_->setText(tr("Library is busy. Try again shortly.")); }
  });
  connect(&library, &lib::Library::searchReady, this, [this](auto id, const lib::SearchPage& page) {
    const auto found = requests_.find(id); if (found == requests_.end()) return;
    const auto request = *found; requests_.erase(found);
    if (request.generation != generation_ || page.query != submittedQuery_) return;
    if (page.total > std::numeric_limits<int>::max()) { status_->setText(tr("Too many results. Refine your search.")); return; }
    QList<StudyRow> rows;
    for (const auto& result : page.rows) {
      const auto context = result.kind == "course" ? tr("Course") : result.courseName + " · " + result.sectionName;
      rows.append({result.id, result.name, result.missing ? context + tr(" · Folder missing") : context,
        false, true, QVariant::fromValue(result)});
    }
    model_->setPage(static_cast<int>(page.offset), static_cast<int>(page.total), rows);
    status_->setText(page.total ? tr("%1 results").arg(page.total) : tr("No matching courses or lessons."));
  });
  connect(&library, &lib::Library::failed, this, [this](auto id, const lib::Error& error) {
    if (!requests_.contains(id)) return;
    model_->failedPage(requests_.take(id).offset); status_->setText(error.message);
  });
  connect(results_, &QListView::activated, this, [this](const QModelIndex& index) {
    if (const auto row = model_->row(index.row())) {
      emit selected(row->value.value<lib::SearchRow>()); accept();
    }
  });
  connect(query_, &QLineEdit::returnPressed, this, [this] {
    const auto index = results_->currentIndex().isValid() ? results_->currentIndex() : model_->index(0);
    if (const auto row = model_->row(index.row())) { emit selected(row->value.value<lib::SearchRow>()); accept(); }
  });
  query_->setFocus();
}

bool SearchDialog::eventFilter(QObject* object, QEvent* event) {
  if (object == query_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if ((key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) && model_->rowCount()) {
      const int current = results_->currentIndex().row();
      const int target = qBound(0, current + (key->key() == Qt::Key_Down ? 1 : -1), model_->rowCount() - 1);
      results_->setCurrentIndex(model_->index(target)); results_->scrollTo(results_->currentIndex());
      return true;
    }
  }
  return QDialog::eventFilter(object, event);
}
