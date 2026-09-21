#include "pdf_reader.hpp"
#include "local_files.hpp"
#include <QFile>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QtMath>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace melearner::pdf {
class PdfReader::Worker {
public:
  struct Request { quint64 id = 0; quint64 generation = 0; QString root; QString path; TileKey key; };
  explicit Worker(PdfReader* owner) : owner_(owner) { thread_ = std::thread([this] { run(); }); }
  ~Worker() { close(); }
  quint64 submit(Request request) {
    std::lock_guard lock(mutex_);
    if (closing_ || credits_->load() >= 64) return 0;
    request.id = ++nextId_; ++*credits_; queue_.push_back(request); condition_.notify_one();
    return request.id;
  }
  void cancelQueued() {
    std::deque<Request> cancelled;
    {
      std::lock_guard lock(mutex_);
      if (closing_) return;
      cancelled.swap(queue_);
    }
    publishCancelled(std::move(cancelled), QStringLiteral("PDF request superseded."));
  }
  void cancelTiles(quint64 generation) {
    if (generation == 0) return;
    std::deque<Request> cancelled;
    {
      std::lock_guard lock(mutex_);
      if (closing_) return;
      for (auto iterator = queue_.begin(); iterator != queue_.end();) {
        if (iterator->path.isEmpty() && iterator->generation == generation) {
          cancelled.push_back(std::move(*iterator));
          iterator = queue_.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
    publishCancelled(std::move(cancelled), QStringLiteral("PDF tile request superseded."));
  }
  void reset() {
    {
      std::lock_guard lock(mutex_);
      if (closing_) return;
      resetRequested_ = true;
    }
    condition_.notify_one();
  }
  void close() {
    std::deque<Request> cancelled;
    {
      std::lock_guard lock(mutex_);
      if (!closing_) {
        closing_ = true;
        cancelled.swap(queue_);
      }
    }
    publishCancelled(std::move(cancelled), QStringLiteral("PDF reader closed."), Qt::QueuedConnection);
    condition_.notify_one();
    // QPdfDocument::render() has no cancellation API. The worker can cancel
    // queued requests, but shutdown waits for one in-flight render to return.
    if (thread_.joinable()) thread_.join();
  }
private:
  PdfReader* owner_;
  std::shared_ptr<std::atomic<int>> credits_ = std::make_shared<std::atomic<int>>(0);
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Request> queue_;
  bool closing_ = false;
  bool resetRequested_ = false;
  quint64 nextId_ = 0;
  std::thread thread_;
  void publishCancelled(
      std::deque<Request> requests,
      const QString& message,
      Qt::ConnectionType connection = Qt::AutoConnection) {
    for (const auto& request : requests) {
      publish(request.id, Error{Error::cancelled, message}, connection);
    }
  }
  void publish(quint64 id, Result result, Qt::ConnectionType connection = Qt::QueuedConnection) {
    QMetaObject::invokeMethod(owner_, [owner = owner_, credits = credits_, id, result = std::move(result)]() mutable {
      --*credits; emit owner->finished(id, std::move(result));
    }, connection);
  }
  void run() {
    std::unique_ptr<QFile> file;
    QPdfDocument document;
    quint64 generation = 0;
    QVector<QSizeF> pages;
    for (;;) {
      Request request;
      bool reset = false;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return closing_ || resetRequested_ || !queue_.empty(); });
        if (closing_) return;
        if (resetRequested_) {
          resetRequested_ = false;
          reset = true;
        } else {
          request = std::move(queue_.front()); queue_.pop_front();
        }
      }
      if (reset) {
        document.close(); file.reset(); pages.clear(); generation = 0;
        continue;
      }
      try {
        if (!request.path.isEmpty()) {
          document.close(); file.reset(); pages.clear(); generation = 0;
          auto root = local_files::LocalFiles::validateRoot(request.root);
          if (!root) { publish(request.id, Error{Error::file, root.error().message}); continue; }
          auto opened = local_files::LocalFiles::openRead(*root, request.path);
          if (!opened) { publish(request.id, Error{Error::file, opened.error().message}); continue; }
          file = std::move(*opened);
          if (file->size() > 2LL * 1024 * 1024 * 1024) {
            publish(request.id, Error{Error::oversized, "PDF exceeds the 2 GiB file limit."}); file.reset(); continue;
          }
          document.load(file.get());
          if (document.status() != QPdfDocument::Status::Ready || document.pageCount() < 1 || document.pageCount() > 5000) {
            const auto message = document.error() == QPdfDocument::Error::IncorrectPassword
              ? QStringLiteral("This PDF needs a password. Open it in your default PDF app.")
              : QStringLiteral("Cannot read this PDF, or it exceeds the 5,000-page limit.");
            const auto code = document.status() == QPdfDocument::Status::Ready
              ? Error::oversized : Error::malformed;
            publish(request.id, Error{code, message}); document.close(); file.reset(); continue;
          }
          bool valid = true;
          for (int index = 0; index < document.pageCount(); ++index) {
            const auto size = document.pagePointSize(index);
            if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() <= 0 || size.height() <= 0 ||
                size.width() > 32768 || size.height() > 32768) { valid = false; break; }
            pages.append(size);
          }
          if (!valid) { publish(request.id, Error{Error::malformed, "PDF has invalid page dimensions."}); document.close(); file.reset(); pages.clear(); continue; }
          generation = request.id; publish(request.id, Info{generation, pages});
        } else {
          const auto& key = request.key;
          if (!generation || request.generation != generation) { publish(request.id, Error{Error::stale, "PDF selection changed."}); continue; }
          if (key.page < 0 || key.page >= pages.size() || key.scale < 4 || key.scale > 64 || key.x < 0 || key.y < 0 || key.x > 255 || key.y > 255) {
            publish(request.id, Error{Error::invalid, "Invalid PDF tile."}); continue;
          }
          const QSize full(qCeil(pages[key.page].width() * key.scale / 16.0), qCeil(pages[key.page].height() * key.scale / 16.0));
          const QRect clip = QRect(key.x * 512, key.y * 512, 512, 512).intersected(QRect(QPoint(), full));
          if (clip.isEmpty()) { publish(request.id, Error{Error::invalid, "PDF tile is outside the page."}); continue; }
          QPdfDocumentRenderOptions options; options.setScaledSize(full); options.setScaledClipRect(clip);
          auto image = document.render(key.page, clip.size(), options);
          if (image.isNull()) publish(request.id, Error{Error::render, "Could not render this PDF page."});
          else publish(request.id, Tile{generation, key, std::move(image)});
        }
      } catch (const std::exception& error) {
        publish(request.id, Error{Error::render, QString::fromUtf8(error.what())});
      } catch (...) { publish(request.id, Error{Error::render, "Unexpected PDF reader error."}); }
    }
  }
};
PdfReader::PdfReader(QObject* parent) : QObject(parent), worker_(std::make_unique<Worker>(this)) {}
PdfReader::~PdfReader() = default;
quint64 PdfReader::open(QString root, QString path) {
  if (root.isEmpty() || path.isEmpty()) return 0;
  worker_->cancelQueued();
  Worker::Request request; request.root = std::move(root); request.path = std::move(path); return worker_->submit(std::move(request));
}
quint64 PdfReader::tile(quint64 generation, TileKey key) {
  Worker::Request request; request.generation = generation; request.key = key; return worker_->submit(std::move(request));
}
void PdfReader::cancelTiles(quint64 generation) { worker_->cancelTiles(generation); }
void PdfReader::clear() {
  worker_->cancelQueued();
  worker_->reset();
}
void PdfReader::close() { worker_->close(); }
}
