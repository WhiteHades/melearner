#pragma once
#include "library.hpp"
#include <QDialog>
#include <QMap>
#include <QTimer>

class QLabel;
class QLineEdit;
class QListView;
class PagedListModel;

class SearchDialog final : public QDialog {
  Q_OBJECT
public:
  explicit SearchDialog(melearner::library::Library& library, QWidget* parent = nullptr);
signals:
  void selected(melearner::library::SearchRow row);
protected:
  bool eventFilter(QObject* object, QEvent* event) override;
private:
  QLineEdit* query_;
  QListView* results_;
  QLabel* status_;
  PagedListModel* model_;
  QTimer debounce_;
  QString submittedQuery_;
  quint64 generation_ = 0;
  struct Request { quint64 generation; int offset; };
  QMap<quint64, Request> requests_;
};
