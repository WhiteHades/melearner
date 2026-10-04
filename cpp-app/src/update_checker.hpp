#pragma once

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class UpdateChecker final : public QObject {
  Q_OBJECT

public:
  explicit UpdateChecker(QObject* parent = nullptr);
  UpdateChecker(const QUrl& endpoint, QObject* parent = nullptr);

public slots:
  void check();

signals:
  void updateAvailable(const QString& version, const QUrl& installer, const QUrl& releasePage);
  void checked(bool newer);
  void failed(const QString& message);

private:
  void finish(QNetworkReply* reply);

  QUrl endpoint_;
  QNetworkAccessManager* network_;
  QNetworkReply* reply_ = nullptr;
  QTimer* timeout_;
  QByteArray response_;
};
