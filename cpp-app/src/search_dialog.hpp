#pragma once
#include "library.hpp"
#include <shadcn/overlays.hpp>
#include <QMap>
#include <QTimer>

class QLabel;
namespace shadcn { class ListView; }
class PagedListModel;

class SearchDialog final : public shadcn::Dialog {
  Q_OBJECT
public:
  explicit SearchDialog(melearner::library::Library& library, QWidget* parent = nullptr);
signals:
  void selected(melearner::library::SearchRow row);
protected:
  bool eventFilter(QObject* object, QEvent* event) override;
private:
  // Results stay a model-backed list view rather than a shadcn Command list.
  // Search pages through an arbitrarily large result set, and the component
  // library's own rule for data views is to avoid one widget per row.
  shadcn::Input* query_;
  shadcn::ListView* results_;
  QLabel* status_;
  shadcn::Button* open_ = nullptr;
  PagedListModel* model_;
  void openSelected();
  QTimer debounce_;
  QString submittedQuery_;
  quint64 generation_ = 0;
  struct Request { quint64 generation; int offset; };
  QMap<quint64, Request> requests_;
};
