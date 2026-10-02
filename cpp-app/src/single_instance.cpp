#include "single_instance.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
#include <QTimer>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <unistd.h>

namespace {
bool sameUser(const QLocalSocket& socket) {
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  return socket.socketDescriptor() >= 0 &&
         getsockopt(static_cast<int>(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                    &credentials, &size) == 0 && size == sizeof(credentials) &&
         credentials.uid == geteuid();
}
}
#endif

SingleInstance::SingleInstance(const QString& directory, QObject* parent)
    : QObject(parent), lock_(QDir(directory).filePath("instance.lock")),
      socketName_("melearner-" + QString::fromLatin1(QCryptographicHash::hash(
          QDir(directory).absolutePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(32))) {
  lock_.setStaleLockTime(0);
#ifdef Q_OS_LINUX
  // Abstract sockets avoid filesystem path limits and disappear with the process.
  // Their namespace has no permission bits, so both peers verify kernel credentials.
  socketName_.prepend(QString::number(geteuid()) + QLatin1Char('-'));
  server_.setSocketOptions(QLocalServer::AbstractNamespaceOption);
#else
  server_.setSocketOptions(QLocalServer::UserAccessOption);
#endif
  connect(&server_, &QLocalServer::newConnection, this, [this] {
    while (auto* socket = server_.nextPendingConnection()) {
#ifdef Q_OS_LINUX
      if (!sameUser(*socket)) {
        socket->abort();
        socket->deleteLater();
        continue;
      }
#endif
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
#ifdef Q_OS_LINUX
    socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    socket.connectToServer(socketName_);
    if (socket.waitForConnected(2000) &&
#ifdef Q_OS_LINUX
        sameUser(socket) &&
#endif
        socket.write("activate\n") == 9 &&
        (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(2000))) {
      socket.disconnectFromServer();
      return Result::Forwarded;
    }
    error_ = tr("Another melearner instance owns this Library but did not respond. Try again after it finishes starting.");
    return Result::Error;
  }
#ifndef Q_OS_LINUX
  // Only the lock owner may remove a stale filesystem socket.
  QLocalServer::removeServer(socketName_);
#endif
  if (server_.listen(socketName_)) return Result::Owner;
  error_ = tr("Cannot start local activation: %1").arg(server_.errorString());
  lock_.unlock();
  return Result::Error;
}
