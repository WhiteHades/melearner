#pragma once
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QMap>
#include <QSet>
#include <optional>

struct StudyRow {
  QString id;
  QString title;
  QString description;
  bool completed = false;
  bool available = true;
  QVariant value;
};

class StudyItemDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void setCompact(bool compact) { compact_ = compact; }
private:
  bool compact_ = false;
};

// Shared by the Course and Lesson views. Only four visible/recent pages stay resident.
class PagedListModel final : public QAbstractListModel {
  Q_OBJECT
public:
  explicit PagedListModel(int pageSize, QObject* parent = nullptr);
  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  void reset();
  bool setPage(int offset, int total, const QList<StudyRow>& rows);
  bool updateRow(const StudyRow& row);
  void failedPage(int offset);
  std::optional<StudyRow> row(int index) const;
  int cachedRows() const;
signals:
  void pageRequested(int offset);
private:
  struct Page { QList<StudyRow> rows; quint64 used; };
  int pageSize_;
  int total_ = 0;
  quint64 generation_ = 0;
  mutable quint64 clock_ = 0;
  mutable QMap<int, Page> pages_;
  mutable QSet<int> pending_;
  mutable std::optional<int> deferred_;
  void request(int offset) const;
};
