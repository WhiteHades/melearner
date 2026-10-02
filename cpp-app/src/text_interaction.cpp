#include "text_interaction.hpp"

#include <shadcn/overlays.hpp>
#include <shadcn/navigation.hpp>
#include <shadcn/rows.hpp>
#include <shadcn/widgets.hpp>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QEvent>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPointer>
#include <QTextEdit>
#include <QKeyEvent>
#include <QWidget>

#include <algorithm>

namespace melearner {
namespace {

class TextSelectionDialog final : public shadcn::Dialog {
 public:
  TextSelectionDialog(const QString& text, QWidget* parent) : shadcn::Dialog(parent) {
    setObjectName(QStringLiteral("textSelection"));
    setTitle(tr("Select text"));
    setDescription(tr("Select and copy this text."));
    setContentWidth(560);

    auto* prose = new shadcn::Prose;
    prose->setObjectName(QStringLiteral("selectedText"));
    prose->setPlainText(text);
    prose->setReaderMode(true);
    prose->setMinimumHeight(72);
    content().addWidget(prose);

    auto* close = new shadcn::Button(tr("Close"));
    close->setVariant(shadcn::Variant::Outline);
    content().addWidget(close, 0, Qt::AlignRight);
    content().setContentsMargins(16, 0, 16, 16);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
  }
};

QAbstractItemView* itemViewFor(QObject* object) {
  auto* widget = qobject_cast<QWidget*>(object);
  while (widget) {
    if (auto* view = qobject_cast<QAbstractItemView*>(widget)) return view;
    widget = widget->parentWidget();
  }
  return nullptr;
}

QString escapedTsvField(const QString& text) {
  if (!text.contains(QLatin1Char('\t')) && !text.contains(QLatin1Char('\n')) &&
      !text.contains(QLatin1Char('\r')) && !text.contains(QLatin1Char('"'))) {
    return text;
  }
  auto escaped = text;
  escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString selectedIndexesAsTsv(const QAbstractItemView& view) {
  auto indexes = view.selectionModel() ? view.selectionModel()->selectedIndexes()
                                       : QModelIndexList{};
  if (indexes.isEmpty()) return {};

  const auto rowPath = [](QModelIndex index) {
    QList<int> path;
    while (index.isValid()) { path.prepend(index.row()); index = index.parent(); }
    return path;
  };
  std::stable_sort(indexes.begin(), indexes.end(), [&](const QModelIndex& left, const QModelIndex& right) {
    const auto leftPath = rowPath(left), rightPath = rowPath(right);
    if (leftPath != rightPath) return leftPath < rightPath;
    return left.column() < right.column();
  });
  QStringList rows, columns;
  QList<int> previousPath;
  for (const auto& index : indexes) {
    const auto path = rowPath(index);
    if (!columns.isEmpty() && path != previousPath) {
      rows.append(columns.join(QLatin1Char('\t'))); columns.clear();
    }
    previousPath = path;
    columns.append(escapedTsvField(index.data(Qt::DisplayRole).toString()));
  }
  if (!columns.isEmpty()) rows.append(columns.join(QLatin1Char('\t')));
  return rows.join(QLatin1Char('\n'));
}

bool isEditingWidget(QWidget* widget) {
  return qobject_cast<QLineEdit*>(widget) || qobject_cast<QTextEdit*>(widget) ||
         qobject_cast<QPlainTextEdit*>(widget);
}

QString informationalText(QWidget* widget) {
  if (const auto text = widget->property("melearnerCopyText").toString(); !text.isEmpty()) return text;
  if (auto* label = qobject_cast<QLabel*>(widget)) return label->text();
  if (auto* button = qobject_cast<QAbstractButton*>(widget))
    return button->text().isEmpty() ? button->accessibleName() : button->text();
  if (auto* group = qobject_cast<QGroupBox*>(widget)) return group->title();
  if (auto* combo = qobject_cast<QComboBox*>(widget); combo && !combo->isEditable()) {
    return combo->currentText();
  }
  if (auto* progress = qobject_cast<QProgressBar*>(widget)) return progress->text();
  return {};
}

void showSelectionDialog(QWidget* owner, const QString& text) {
  if (text.isEmpty()) return;
  auto* dialog = new TextSelectionDialog(text, owner->window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->open();
}

void showCopyMenu(QWidget* owner, const QPoint& position, const QString& text,
                  const QString& selectionText = {}) {
  if (!owner || text.isEmpty()) return;
  shadcn::DropdownMenu menu;
  auto* copy = menu.addAction(QObject::tr("Copy text"));
  auto* select = menu.addAction(QObject::tr("Select text"));
  const QPointer<QWidget> guardedOwner(owner);
  const auto chosen = menu.exec(position);
  if (!guardedOwner) return;
  if (chosen == copy) QApplication::clipboard()->setText(text);
  else if (chosen == select) showSelectionDialog(owner, selectionText.isEmpty() ? text : selectionText);
}

class TextInteractionFilter final : public QObject {
 public:
  explicit TextInteractionFilter(QObject* parent) : QObject(parent) {}

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    switch (event->type()) {
      case QEvent::Polish:
      case QEvent::Show:
        if (auto* label = qobject_cast<QLabel*>(watched); label && !label->buddy()) {
          label->setTextInteractionFlags(label->textInteractionFlags() |
              Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        }
        return false;
      case QEvent::KeyPress:
        return copyViewSelection(watched, static_cast<QKeyEvent*>(event));
      case QEvent::ContextMenu:
        return contextMenu(watched, static_cast<QContextMenuEvent*>(event));
      default:
        return false;
    }
  }

 private:
  bool copyViewSelection(QObject* watched, QKeyEvent* key) const {
    if (key->key() != Qt::Key_C || !(key->modifiers() & Qt::ControlModifier) ||
        (key->modifiers() & (Qt::AltModifier | Qt::MetaModifier))) {
      return false;
    }
    auto* view = itemViewFor(watched);
    if (!view || isEditingWidget(QApplication::focusWidget())) return false;
    const auto text = selectedIndexesAsTsv(*view);
    if (text.isEmpty()) return false;
    QApplication::clipboard()->setText(text);
    return true;
  }

  bool contextMenu(QObject* watched, QContextMenuEvent* event) const {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;

    if (auto* header = qobject_cast<QHeaderView*>(widget->parentWidget()); header && header->viewport() == widget) {
      const int section = header->logicalIndexAt(event->pos());
      if (section < 0 || !header->model()) return false;
      const auto text = header->model()->headerData(section, header->orientation(), Qt::DisplayRole).toString();
      if (text.isEmpty()) return false;
      showCopyMenu(widget, event->globalPos(), text); return true;
    }

    if (auto* view = itemViewFor(widget); view && view->viewport() == widget) {
      if (widget->contextMenuPolicy() != Qt::DefaultContextMenu) return false;
      const auto index = view->indexAt(event->pos());
      if (!index.isValid()) return false;
      const auto value = index.data(Qt::DisplayRole);
      if (!value.isValid() || value.toString().isEmpty()) return false;
      const auto details = view->model()->columnCount(index.parent()) == 1
          ? index.data(Qt::AccessibleTextRole).toString() : QString{};
      showCopyMenu(widget, event->globalPos(), value.toString(), details);
      return true;
    }

    if (widget->contextMenuPolicy() != Qt::DefaultContextMenu || !widget->actions().isEmpty() ||
        isEditingWidget(widget)) {
      return false;
    }
    if (const auto* label = qobject_cast<QLabel*>(widget); label && label->hasSelectedText()) return false;
    const auto text = informationalText(widget);
    if (text.isEmpty()) return false;
    showCopyMenu(widget, event->globalPos(), text);
    return true;
  }
};

}  // namespace

void installTextInteraction(QApplication& application) {
  constexpr auto installedProperty = "melearnerTextInteractionInstalled";
  if (application.property(installedProperty).toBool()) return;
  application.setProperty(installedProperty, true);
  auto* filter = new TextInteractionFilter(&application);
  application.installEventFilter(filter);
}

}  // namespace melearner
