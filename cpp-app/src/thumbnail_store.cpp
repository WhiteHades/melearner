#include "thumbnail_store.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QSet>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace melearner {
namespace {

constexpr qsizetype kQueueLimit = 32;
constexpr qint64 kCacheLimit = 128LL * 1024 * 1024;
constexpr qint64 kDecodeDeadlineMs = 2000;
constexpr int kPacketLimit = 64;
constexpr int kThumbnailWidth = 320;
constexpr int kThumbnailHeight = 180;

struct Job {
    enum class Kind { cache, source } kind = Kind::cache;
    QString courseId;
    QString approvedRoot;
    library::Lesson lesson;
    std::uint64_t generation = 0;
};

struct DeliveryGate {
    std::mutex mutex;
    std::condition_variable available;
    qsizetype pending = 0;
};

bool reserveDelivery(const std::shared_ptr<DeliveryGate>& gate,
                    const std::shared_ptr<std::atomic<std::uint64_t>>& generation,
                    std::uint64_t expected) {
    std::unique_lock lock(gate->mutex);
    gate->available.wait(lock, [&] {
        return gate->pending < kQueueLimit || generation->load(std::memory_order_acquire) != expected;
    });
    if (generation->load(std::memory_order_acquire) != expected) return false;
    ++gate->pending;
    return true;
}

void releaseDelivery(const std::shared_ptr<DeliveryGate>& gate) {
    {
        std::lock_guard lock(gate->mutex);
        --gate->pending;
    }
    gate->available.notify_one();
}

QString keyFor(const QString& courseId, const QString& root) {
    return courseId + QChar(u'\n') + root;
}

QString cachePath(const QString& courseId) {
    const QByteArray hash = QCryptographicHash::hash(courseId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("course-thumbnails/") + QString::fromLatin1(hash) + QStringLiteral(".png"));
}

bool insideRoot(const QString& rootPath, const QString& filePath, QString* canonicalFile = nullptr) {
    const QString root = QFileInfo(rootPath).canonicalFilePath();
    const QString file = QFileInfo(filePath).canonicalFilePath();
    if (root.isEmpty() || file.isEmpty()) return false;
    // QFileInfo canonical paths use '/', including Windows drive paths.
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (file != root && !file.startsWith(prefix, Qt::CaseSensitive)) return false;
    if (canonicalFile) *canonicalFile = file;
    return true;
}

int approvedLocalOpen(AVFormatContext* context, AVIOContext** io, const char* url,
                      int flags, AVDictionary** options) {
    if (!context || !context->opaque || !url || !io) return AVERROR(EINVAL);
    const auto* root = static_cast<const QString*>(context->opaque);
    const QString path = QString::fromUtf8(url);
    if (path.contains(QStringLiteral("://")) || path.startsWith(QStringLiteral("data:")) ||
        !insideRoot(*root, path)) return AVERROR(EACCES);
    return avio_open2(io, url, flags, &context->interrupt_callback, options);
}

struct InterruptState {
    std::shared_ptr<std::atomic<std::uint64_t>> generation;
    std::uint64_t expected = 0;
    std::chrono::steady_clock::time_point deadline;
};

int interrupt(void* opaque) {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return !state || state->generation->load(std::memory_order_acquire) != state->expected ||
        std::chrono::steady_clock::now() >= state->deadline;
}

QImage decodeStill(const QString& source, const QString& approvedRoot,
                   const std::shared_ptr<std::atomic<std::uint64_t>>& generation,
                   std::uint64_t expected) {
    InterruptState interruptState{generation, expected,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDecodeDeadlineMs)};
    AVFormatContext* format = avformat_alloc_context();
    if (!format) return {};
    format->opaque = const_cast<QString*>(&approvedRoot);
    format->io_open = approvedLocalOpen;
    format->interrupt_callback = {interrupt, &interruptState};
    format->probesize = 2 * 1024 * 1024;
    format->max_analyze_duration = 1000000;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist",
        "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,flv,mpeg,mpegts,ogg,asf,rm,wav", 0);
    const QByteArray native = QFileInfo(source).filePath().toUtf8();
    if (avformat_open_input(&format, native.constData(), nullptr, &options) < 0) {
        av_dict_free(&options);
        if (format) avformat_free_context(format);
        return {};
    }
    av_dict_free(&options);

    QImage image;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            format->streams[i]->discard = AVDISCARD_ALL;
    }
    if (avformat_find_stream_info(format, nullptr) < 0 || interrupt(&interruptState)) {
        avformat_close_input(&format);
        return image;
    }
    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        avformat_close_input(&format);
        return image;
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return image;
    }
    codec->thread_count = 1;
    codec->max_pixels = 3840LL * 2160LL;
    if (avcodec_open2(codec, decoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return image;
    }

    const auto duration = stream->duration > 0 ? stream->duration :
        format->duration > 0 ? av_rescale_q(format->duration, AVRational{1, AV_TIME_BASE}, stream->time_base) : 0;
    if (duration > 0) {
        const auto fraction = QRandomGenerator::global()->generateDouble() * 0.75 + 0.1;
        const auto target = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        (void)av_seek_frame(format, streamIndex,
            target + static_cast<std::int64_t>(static_cast<double>(duration) * fraction), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool decoded = false;
    int packets = 0;
    while (frame && packet && packets < kPacketLimit && !interrupt(&interruptState)) {
        const int read = av_read_frame(format, packet);
        if (read < 0) break;
        ++packets;
        if (packet->stream_index == streamIndex && avcodec_send_packet(codec, packet) >= 0) {
            if (avcodec_receive_frame(codec, frame) >= 0) decoded = true;
        }
        av_packet_unref(packet);
        if (decoded) break;
    }

    if (decoded && !interrupt(&interruptState) && frame->width > 0 && frame->height > 0 &&
        frame->width <= 16384 && frame->height <= 16384) {
        const int visibleWidth = frame->width - static_cast<int>(frame->crop_left + frame->crop_right);
        const int visibleHeight = frame->height - static_cast<int>(frame->crop_top + frame->crop_bottom);
        if (visibleWidth <= 0 || visibleHeight <= 0) {
            decoded = false;
        } else if (static_cast<std::int64_t>(visibleWidth) * kThumbnailHeight >
                   static_cast<std::int64_t>(visibleHeight) * kThumbnailWidth) {
            const int croppedWidth = std::max(1, visibleHeight * kThumbnailWidth / kThumbnailHeight);
            const auto extra = static_cast<std::size_t>(visibleWidth - croppedWidth);
            frame->crop_left += extra / 2;
            frame->crop_right += extra - extra / 2;
        } else {
            const int croppedHeight = std::max(1, visibleWidth * kThumbnailHeight / kThumbnailWidth);
            const auto extra = static_cast<std::size_t>(visibleHeight - croppedHeight);
            frame->crop_top += extra / 2;
            frame->crop_bottom += extra - extra / 2;
        }
        if (decoded) (void)av_frame_apply_cropping(frame, 0);
        if (decoded) {
            QImage scaled(kThumbnailWidth, kThumbnailHeight, QImage::Format_RGB32);
            SwsContext* scaler = sws_getContext(frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), kThumbnailWidth, kThumbnailHeight, AV_PIX_FMT_BGRA,
                SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (scaler && !scaled.isNull()) {
                std::uint8_t* output[] = {scaled.bits(), nullptr, nullptr, nullptr};
                int strides[] = {static_cast<int>(scaled.bytesPerLine()), 0, 0, 0};
                if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, output, strides) > 0)
                    image = std::move(scaled);
            }
            if (scaler) sws_freeContext(scaler);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return interrupt(&interruptState) ? QImage{} : image;
}

QImage readCache(const QString& path, const QString& root) {
    QFileInfo cacheInfo(path);
    if (!cacheInfo.isFile() || cacheInfo.size() > 2 * 1024 * 1024) return {};
    QImageReader reader(path, "png");
    const QString cachedSource = reader.text(QStringLiteral("source"));
    bool sizeOk = false, modifiedOk = false;
    const qint64 size = reader.text(QStringLiteral("source-size")).toLongLong(&sizeOk);
    const qint64 modified = reader.text(QStringLiteral("source-mtime")).toLongLong(&modifiedOk);
    QString canonical;
    if (!sizeOk || !modifiedOk || !insideRoot(root, cachedSource, &canonical)) return {};
    const QFileInfo source(canonical);
    if (!source.isFile() || source.size() != size || source.lastModified().toMSecsSinceEpoch() != modified) return {};
    const QSize imageSize = reader.size();
    if (imageSize != QSize(kThumbnailWidth, kThumbnailHeight)) return {};
    reader.setScaledSize(QSize(kThumbnailWidth, kThumbnailHeight));
    return reader.read();
}

bool writeCache(const QString& path, const QImage& image, const QString& source,
                qint64 size, qint64 modified) {
    if (image.isNull()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&file, "png");
    writer.setText(QStringLiteral("source"), source);
    writer.setText(QStringLiteral("source-size"), QString::number(size));
    writer.setText(QStringLiteral("source-mtime"), QString::number(modified));
    if (!writer.write(image) || !file.commit()) return false;
    return true;
}

qint64 pruneCache(const QString& exceptPath) {
    const QDir directory(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("course-thumbnails")));
    auto files = directory.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const auto& file : files) total += file.size();
    for (const auto& file : files) {
        if (total <= kCacheLimit) break;
        if (file.absoluteFilePath() == exceptPath) continue;
        const qint64 size = file.size();
        if (QFile::remove(file.absoluteFilePath())) total -= size;
    }
    return total;
}

}  // namespace

class ThumbnailStore::Impl final {
public:
    explicit Impl(ThumbnailStore* owner) : owner_(owner), generation_(std::make_shared<std::atomic<std::uint64_t>>(1)),
        worker_([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            queue_.clear();
            std::lock_guard deliveryLock(deliveryGate_->mutex);
            generation_->fetch_add(1, std::memory_order_acq_rel);
        }
        deliveryGate_->available.notify_all();
        ready_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    void request(QString courseId, QString root) {
        enqueue({Job::Kind::cache, std::move(courseId), std::move(root), {}, currentGeneration()});
    }

    void provide(QString courseId, QString root, const library::Lesson& lesson) {
        enqueue({Job::Kind::source, std::move(courseId), std::move(root), lesson, currentGeneration()});
    }

    void cancel() {
        {
            std::lock_guard lock(mutex_);
            {
                std::lock_guard deliveryLock(deliveryGate_->mutex);
                generation_->fetch_add(1, std::memory_order_acq_rel);
            }
            queue_.clear();
            queued_.clear();
            needed_.clear();
        }
        deliveryGate_->available.notify_all();
    }

private:
    std::uint64_t currentGeneration() const { return generation_->load(std::memory_order_acquire); }

    void enqueue(Job job) {
        const QString key = keyFor(job.courseId, job.approvedRoot);
        bool overflow = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            if (queued_.contains(key)) return;
            if (queue_.size() >= kQueueLimit) {
                overflow = true;
            } else {
                queued_.insert(key);
                if (job.kind == Job::Kind::source) needed_.remove(key);
                queue_.push_back(std::move(job));
            }
        }
        if (overflow) {
            postOverflowReady(job.courseId, job.generation);
            return;
        }
        ready_.notify_one();
    }

    void postReady(const QString& id, QImage image, std::uint64_t generation) {
        if (!reserveDelivery(deliveryGate_, generation_, generation)) return;
        auto* owner = owner_;
        auto state = generation_;
        auto gate = deliveryGate_;
        if (!QMetaObject::invokeMethod(owner, [owner, id, image = std::move(image), state, generation, gate]() mutable {
            if (state->load(std::memory_order_acquire) == generation) emit owner->ready(id, std::move(image));
            releaseDelivery(gate);
        }, Qt::QueuedConnection)) releaseDelivery(deliveryGate_);
    }

    void postSourceNeeded(const QString& id, std::uint64_t generation) {
        if (!reserveDelivery(deliveryGate_, generation_, generation)) return;
        auto* owner = owner_;
        auto state = generation_;
        auto gate = deliveryGate_;
        if (!QMetaObject::invokeMethod(owner, [owner, id, state, generation, gate] {
            if (state->load(std::memory_order_acquire) == generation) emit owner->sourceNeeded(id);
            releaseDelivery(gate);
        }, Qt::QueuedConnection)) releaseDelivery(deliveryGate_);
    }

    void postOverflowReady(const QString& id, std::uint64_t generation) {
        auto* owner = owner_;
        auto state = generation_;
        const auto deliver = [owner, id, state, generation] {
            if (state->load(std::memory_order_acquire) == generation) emit owner->ready(id, {});
        };
        if (QThread::currentThread() == owner->thread()) deliver();
        else (void)QMetaObject::invokeMethod(owner, deliver, Qt::QueuedConnection);
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
                queued_.remove(keyFor(job.courseId, job.approvedRoot));
            }
            if (job.generation != currentGeneration()) continue;
            const QString path = cachePath(job.courseId);
            if (job.kind == Job::Kind::cache) {
                const QImage cached = readCache(path, job.approvedRoot);
                if (!cached.isNull()) {
                    postReady(job.courseId, cached, job.generation);
                    continue;
                }
                const QString key = keyFor(job.courseId, job.approvedRoot);
                bool emitRequest = false;
                {
                    std::lock_guard lock(mutex_);
                    emitRequest = !stopping_ && job.generation == currentGeneration() &&
                        needed_.size() < kQueueLimit && !needed_.contains(key);
                    if (emitRequest) needed_.insert(key);
                }
                if (emitRequest) postSourceNeeded(job.courseId, job.generation);
                continue;
            }

            QString source;
            const bool valid = job.lesson.type == QStringLiteral("video") &&
                insideRoot(job.approvedRoot, job.lesson.path, &source);
            const QFileInfo info(source);
            QImage image;
            if (valid && info.isFile() && job.generation == currentGeneration()) {
                const qint64 sourceSize = info.size();
                const qint64 sourceModified = info.lastModified().toMSecsSinceEpoch();
                image = decodeStill(source, job.approvedRoot, generation_, job.generation);
                if (!image.isNull() && job.generation == currentGeneration()) {
                    const QFileInfo current(source);
                    if (!current.isFile() || current.size() != sourceSize ||
                        current.lastModified().toMSecsSinceEpoch() != sourceModified) {
                        image = {};
                    } else {
                        if (!cacheBytesKnown_) {
                            cacheBytes_ = pruneCache(path);
                            cacheBytesKnown_ = true;
                        }
                        const QFileInfo previous(path);
                        const qint64 previousSize = previous.isFile() ? previous.size() : 0;
                        if (!writeCache(path, image, source, sourceSize, sourceModified)) {
                            image = {};
                        } else {
                            const QFileInfo written(path);
                            cacheBytes_ += (written.isFile() ? written.size() : 0) - previousSize;
                            ++writesSinceReconcile_;
                            if (cacheBytes_ > kCacheLimit || writesSinceReconcile_ >= 64) {
                                cacheBytes_ = pruneCache(path);
                                writesSinceReconcile_ = 0;
                            }
                        }
                    }
                }
            }
            postReady(job.courseId, std::move(image), job.generation);
        }
    }

    ThumbnailStore* owner_;
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
    std::shared_ptr<DeliveryGate> deliveryGate_ = std::make_shared<DeliveryGate>();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> queue_;
    QSet<QString> queued_;
    QSet<QString> needed_;
    qint64 cacheBytes_ = 0;
    int writesSinceReconcile_ = 0;
    bool cacheBytesKnown_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

ThumbnailStore::ThumbnailStore(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
ThumbnailStore::~ThumbnailStore() = default;
void ThumbnailStore::request(QString courseId, QString approvedRoot) {
    impl_->request(std::move(courseId), std::move(approvedRoot));
}
void ThumbnailStore::provideSource(QString courseId, QString approvedRoot, const library::Lesson& lesson) {
    impl_->provide(std::move(courseId), std::move(approvedRoot), lesson);
}
void ThumbnailStore::cancelPending() { impl_->cancel(); }

}  // namespace melearner
