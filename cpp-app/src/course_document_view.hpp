#pragma once

#include <QString>
#include <QWidget>

class QWebEngineView;

namespace melearner {

// A disposable, offline-only web surface for HTML and Markdown course files.
// The WebEngine process/profile is created only when a document is opened.
class CourseDocumentView final : public QWidget {
  Q_OBJECT
public:
  explicit CourseDocumentView(QWidget *parent = nullptr);
  ~CourseDocumentView() override;

  void open(const QString &courseRoot, const QString &filePath);
  void clear();
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
