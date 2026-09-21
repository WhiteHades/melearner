#include "single_instance.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
#include <QTimer>

SingleInstance::SingleInstance(const QString& directory, QObject* parent)
    : QObject(parent), lock_(QDir(directory).filePath("instance.lock")),
      socketName_("melearner-" + QString::fromLatin1(QCryptographicHash::hash(
          QDir(directory).absolutePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(32))) {
  lock_.setStaleLockTime(0);
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&server_, &QLocalServer::newConnection, this, [this] {
    while (auto* socket = server_.nextPendingConnection()) {
      socket->setReadBufferSize(16 * 1024 + 1);
      auto receive = [this, socket] {
        // Activation has one fixed message. No paths or executable commands enter here.
        const auto message = socket->peek(16 * 1024 + 1);
        if (message == "activate\n") {
          socket->readAll();
          emit activationRequested();
          socket->disconnectFromServer();
        } else if (message.contains('\n') || message.size() > 16 * 1024) {
          socket->abort();
        }
      };
      connect(socket, &QLocalSocket::readyRead, this, receive);
      connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
      QTimer::singleShot(2000, socket, [socket] { socket->abort(); socket->deleteLater(); });
      receive();
    }
  });
}

SingleInstance::Result SingleInstance::acquire() {
  if (!lock_.tryLock(0)) {
    if (lock_.error() != QLockFile::LockFailedError) {
      error_ = tr("Cannot lock the current C++ data directory. Check its permissions.");
      return Result::Error;
    }
    QLocalSocket socket;
    socket.connectToServer(socketName_);
    if (socket.waitForConnected(2000) && socket.write("activate\n") == 9 &&
        socket.waitForBytesWritten(2000)) {
      socket.disconnectFromServer();
      return Result::Forwarded;
    }
    error_ = tr("Another melearner instance owns this Library but did not respond. Try again after it finishes starting.");
    return Result::Error;
  }
  // Only the lock owner may remove a stale socket left by a terminated process.
  QLocalServer::removeServer(socketName_);
  if (server_.listen(socketName_)) return Result::Owner;
  error_ = tr("Cannot start local activation: %1").arg(server_.errorString());
  lock_.unlock();
  return Result::Error;
}
