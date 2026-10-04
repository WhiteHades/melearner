#pragma once

#include <QString>
#include <QWidget>

class QWebEngineView;

namespace melearner {

// Offline-only HTML/Markdown. One same-course private profile stays warm briefly
// between readings; document pages and their access tokens are never reused.
class CourseDocumentView final : public QWidget {
  Q_OBJECT
public:
  explicit CourseDocumentView(QWidget *parent = nullptr);
  ~CourseDocumentView() override;

  void open(const QString &courseRoot, const QString &filePath);
  void clear();
  void suspend(const QString& courseRoot = {});
  void scrollBy(int pixels);
  void jumpTo(bool last);
  void focusReader();

signals:
  void loaded(bool success);
  void error(const QString &message);
  void resourceDenied(const QString &path);

private:
  class Private;
  Private *d_;
};

} // namespace melearner
