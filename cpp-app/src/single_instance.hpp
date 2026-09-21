#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QObject>

class SingleInstance final : public QObject {
  Q_OBJECT
public:
  enum class Result { Owner, Forwarded, Error };
  explicit SingleInstance(const QString& dataDirectory, QObject* parent = nullptr);
  Result acquire();
  QString error() const { return error_; }
signals:
  void activationRequested();
private:
  QLockFile lock_;
  QLocalServer server_;
  QString socketName_;
  QString error_;
};
