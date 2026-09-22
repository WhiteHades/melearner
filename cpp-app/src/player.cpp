#include "player.hpp"
#include "local_files.hpp"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace melearner {
namespace {

constexpr std::size_t kCommandCapacity = 128;
constexpr std::size_t kCoalescedCapacity = 32;
constexpr std::uint64_t kInternalRequestBit = 1ULL << 63U;

enum class CommandKind {
    load,
    addSubtitle,
    play,
    pause,
    stop,
    seekAbsolute,
    seekRelative,
    setVolume,
    setMuted,
    setRate,
    selectAudio,
    selectSubtitle,
    selectChapter,
    frameStep,
    screenshot,
};

struct Command {
    Player::RequestId id = 0;
    CommandKind kind = CommandKind::play;
    QString path;
    qint64 integer = 0;
    double number = 0.0;
    bool flag = false;
    int coalesceKey = 0;
};

[[nodiscard]] Command commandOf(CommandKind kind) {
    Command command;
    command.kind = kind;
    return command;
}

enum class ReplyAction {
    user,
    restorePause,
    restoreSeek,
};

struct PendingReply {
    ReplyAction action = ReplyAction::user;
    Player::RequestId userId = 0;
};

[[nodiscard]] QString mpvError(int error) {
    const auto* text = mpv_error_string(error);
    return QString::fromUtf8(text == nullptr ? "unknown mpv error" : text);
}

[[nodiscard]] QString utf8(const char* value) {
    return value == nullptr ? QString{} : QString::fromUtf8(value);
}

[[nodiscard]] const mpv_node* mapValue(const mpv_node& node, const char* key) {
    if (node.format != MPV_FORMAT_NODE_MAP || node.u.list == nullptr || key == nullptr) {
        return nullptr;
    }
    const auto* list = node.u.list;
    if (list->num <= 0 || list->values == nullptr || list->keys == nullptr) {
        return nullptr;
    }
    for (int index = 0; index < list->num; ++index) {
        if (list->keys[index] != nullptr && std::strcmp(list->keys[index], key) == 0) {
            return &list->values[index];
        }
    }
    return nullptr;
}

[[nodiscard]] QString nodeString(const mpv_node* value) {
    if (value == nullptr) {
        return {};
    }
    if (value->format == MPV_FORMAT_STRING) {
        return utf8(value->u.string);
    }
    return {};
}

[[nodiscard]] qint64 nodeInteger(const mpv_node* value, qint64 fallback = -1) {
    if (value == nullptr) {
        return fallback;
    }
    if (value->format == MPV_FORMAT_INT64) {
        return static_cast<qint64>(value->u.int64);
    }
    if (value->format == MPV_FORMAT_DOUBLE) {
        return static_cast<qint64>(value->u.double_);
    }
    if (value->format == MPV_FORMAT_FLAG) {
        return value->u.flag == 0 ? 0 : 1;
    }
    return fallback;
}

[[nodiscard]] double nodeNumber(const mpv_node* value, double fallback = 0.0) {
    if (value == nullptr) {
        return fallback;
    }
    if (value->format == MPV_FORMAT_DOUBLE) {
        return value->u.double_;
    }
    if (value->format == MPV_FORMAT_INT64) {
        return static_cast<double>(value->u.int64);
    }
    return fallback;
}

[[nodiscard]] bool nodeFlag(const mpv_node* value, bool fallback = false) {
    if (value == nullptr || value->format != MPV_FORMAT_FLAG) {
        return fallback;
    }
    return value->u.flag != 0;
}

}  // namespace

class Player::Impl final {
public:
    explicit Impl(Player& owner, Player::DecodeMode decoding) : owner_(&owner), decoding_(decoding) {}

    ~Impl() { shutdown(); }

    void setApprovedRoots(const QStringList& roots) {
        std::lock_guard lock(mutex_);
        roots_ = roots;
    }

    [[nodiscard]] QStringList approvedRoots() const {
        std::lock_guard lock(mutex_);
        return roots_;
    }

    void start() {
        std::lock_guard lock(mutex_);
        if (started_ || stopping_) {
            return;
        }
        started_ = true;
        worker_ = std::thread([this] { run(); });
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (!started_) {
                return;
            }
        }
        notifyAboutToShutdown();
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        commandAvailable_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] bool isReady() const { return ready_.load(std::memory_order_acquire); }

    [[nodiscard]] Player::RequestId enqueue(Command command) {
        Player::RequestId superseded = 0;
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !started_) {
                postCommandFailure(0, "not_started", "The Player is not running.");
                return 0;
            }
            std::lock_guard reservationLock(reservations_->mutex);
            command.id = nextRequestId_++;
            if (command.coalesceKey != 0) {
                const auto existing = coalesced_.find(command.coalesceKey);
                if (existing == coalesced_.end() && coalesced_.size() >= kCoalescedCapacity) {
                    postCommandFailure(command.id, "busy", "Too many replaceable Player commands are pending.");
                    return 0;
                }
                if (existing != coalesced_.end()) {
                    if (reservations_->ids.size() >= kCommandCapacity) {
                        postCommandFailure(command.id, "busy", "The Player command queue is full.");
                        return 0;
                    }
                    superseded = existing->second.id;
                    existing->second = command;
                    reservations_->ids.insert(command.id);
                } else {
                    if (reservations_->ids.size() >= kCommandCapacity) {
                        postCommandFailure(command.id, "busy", "The Player command queue is full.");
                        return 0;
                    }
                    coalesced_.emplace(command.coalesceKey, command);
                    reservations_->ids.insert(command.id);
                }
            } else {
                if (reservations_->ids.size() >= kCommandCapacity) {
                    postCommandFailure(command.id, "busy", "The Player command queue is full.");
                    return 0;
                }
                queued_.push_back(command);
                reservations_->ids.insert(command.id);
            }
        }

        if (superseded != 0) {
            postCommandFailure(superseded, "superseded", "A newer value replaced this pending Player command.");
        }
        commandAvailable_.notify_one();
        if (auto* handle = handle_.load(std::memory_order_acquire); handle != nullptr) {
            mpv_wakeup(handle);
        }
        return command.id;
    }

    [[nodiscard]] bool createRenderContext(
        Player::OpenGLProcAddress getProcAddress,
        void* getProcAddressContext,
        Player::RenderUpdateCallback updateCallback,
        void* updateCallbackContext) {
        auto* handle = handle_.load(std::memory_order_acquire);
        if (handle == nullptr || !ready_.load(std::memory_order_acquire) || getProcAddress == nullptr) {
            return false;
        }
        if (renderContext_.load(std::memory_order_acquire) != nullptr) {
            return true;
        }

        mpv_opengl_init_params glParams{};
        glParams.get_proc_address = getProcAddress;
        glParams.get_proc_address_ctx = getProcAddressContext;
        const char* apiType = MPV_RENDER_API_TYPE_OPENGL;
        int advancedControl = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(apiType)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glParams},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advancedControl},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context* context = nullptr;
        const auto error = mpv_render_context_create(&context, handle, params);
        if (error < 0 || context == nullptr) {
            postFatalError("renderer_init", mpvError(error));
            return false;
        }
        mpv_render_context_set_update_callback(context, updateCallback, updateCallbackContext);
        renderContext_.store(context, std::memory_order_release);
        return true;
    }

    void destroyRenderContext() {
        auto* context = renderContext_.exchange(nullptr, std::memory_order_acq_rel);
        if (context == nullptr) {
            return;
        }
        mpv_render_context_set_update_callback(context, nullptr, nullptr);
        mpv_render_context_free(context);
    }

    [[nodiscard]] bool renderFrame(int framebufferObject, int width, int height,
                                   int internalFormat, bool flipY) {
        auto* context = renderContext_.load(std::memory_order_acquire);
        if (context == nullptr || width <= 0 || height <= 0) {
            return false;
        }
        int flip = flipY ? 1 : 0;
        mpv_opengl_fbo fbo{};
        fbo.fbo = framebufferObject;
        fbo.w = width;
        fbo.h = height;
        fbo.internal_format = internalFormat;
        int blockForTargetTime = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        (void)mpv_render_context_update(context);
        return mpv_render_context_render(context, params) >= 0;
    }

    [[nodiscard]] bool hasRenderContext() const {
        return renderContext_.load(std::memory_order_acquire) != nullptr;
    }

private:
    struct ReservationState {
        mutable std::mutex mutex;
        std::unordered_set<Player::RequestId> ids;
    };

    struct RestoreState {
        bool active = false;
        QString path;
        qint64 positionMs = 0;
        Player::RequestId loadRequestId = 0;
    };

    [[nodiscard]] bool stopping() const {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    void notifyAboutToShutdown() {
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            if (!shutdownNotified_) {
                shutdownNotified_ = true;
                notify = true;
            }
        }
        if (notify) {
            emit owner_->aboutToShutdown();
        }
    }

    void requestShutdownOnOwnerThread() {
        bool request = false;
        {
            std::lock_guard lock(mutex_);
            if (!stopping_ && !shutdownQueued_) {
                shutdownQueued_ = true;
                request = true;
            }
        }
        if (request) {
            QMetaObject::invokeMethod(owner_, [owner = owner_] {
                owner->shutdown();
            }, Qt::QueuedConnection);
        }
    }

    void run() {
        if (!initializeMpv()) {
            rejectPending("not_initialized", "libmpv could not be initialized.");
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            ready_.store(false, std::memory_order_release);
            emit owner_->stopped();
            return;
        }

        ready_.store(true, std::memory_order_release);
        emit owner_->initialized();

        while (!stopping()) {
            Command command;
            bool haveCommand = false;
            {
                std::unique_lock lock(mutex_);
                if (queued_.empty() && coalesced_.empty() && !stopping_) {
                    commandAvailable_.wait_for(lock, std::chrono::milliseconds(8));
                }
                if (!queued_.empty()) {
                    command = std::move(queued_.front());
                    queued_.pop_front();
                    haveCommand = true;
                } else if (!coalesced_.empty()) {
                    auto item = coalesced_.begin();
                    command = std::move(item->second);
                    coalesced_.erase(item);
                    haveCommand = true;
                }
            }

            if (haveCommand) {
                processCommand(command);
            }
            drainEvents();
            if (!haveCommand && !stopping()) {
                if (auto* event = mpv_wait_event(handle_.load(std::memory_order_acquire), 0.02);
                    event != nullptr && event->event_id != MPV_EVENT_NONE) {
                    processEvent(*event);
                }
            }
        }

        rejectPending("cancelled", "The Player was shut down before this command ran.");
        for (const auto& [requestId, reply] : pendingReplies_) {
            if (reply.action == ReplyAction::user) {
                postCommandFailure(reply.userId, "cancelled",
                                   "The Player was shut down before this command completed.");
            }
        }
        pendingReplies_.clear();
        {
            std::lock_guard lock(reservations_->mutex);
            reservations_->ids.clear();
        }
        ready_.store(false, std::memory_order_release);
        if (auto* handle = handle_.exchange(nullptr, std::memory_order_acq_rel); handle != nullptr) {
            mpv_set_wakeup_callback(handle, nullptr, nullptr);
            mpv_terminate_destroy(handle);
        }
        emit owner_->stopped();
    }

    [[nodiscard]] bool initializeMpv() {
        auto* handle = mpv_create();
        if (handle == nullptr) {
            postFatalError("create", "libmpv returned no client handle.");
            return false;
        }
        handle_.store(handle, std::memory_order_release);

        const std::pair<const char*, const char*> options[] = {
            {"config", "no"},
            {"terminal", "no"},
            {"load-scripts", "no"},
            {"ytdl", "no"},
            {"access-references", "no"},
            {"load-unsafe-playlists", "no"},
            {"input-default-bindings", "no"},
            {"input-vo-keyboard", "no"},
            {"osc", "no"},
            {"hwdec", decoding_ == Player::DecodeMode::Software ? "no" : "auto"},
            {"vo", "libmpv"},
            {"idle", "yes"},
            {"pause", "yes"},
            {"keep-open", "no"},
            {"video-sync", "display-resample"},
            {"video-timing-offset", "0"},
        };
        for (const auto& [name, value] : options) {
            if (const auto error = mpv_set_option_string(handle, name, value); error < 0) {
                postFatalError("option", QStringLiteral("%1: %2").arg(name, mpvError(error)));
                mpv_terminate_destroy(handle_.exchange(nullptr, std::memory_order_acq_rel));
                return false;
            }
        }
        if (const auto error = mpv_initialize(handle); error < 0) {
            postFatalError("initialize", mpvError(error));
            mpv_terminate_destroy(handle_.exchange(nullptr, std::memory_order_acq_rel));
            return false;
        }
        const struct Observation {
            std::uint64_t id;
            const char* name;
            mpv_format format;
        } observations[] = {
            {1001, "time-pos", MPV_FORMAT_DOUBLE},
            {1002, "duration", MPV_FORMAT_DOUBLE},
            {1003, "pause", MPV_FORMAT_FLAG},
            {1004, "volume", MPV_FORMAT_DOUBLE},
            {1005, "mute", MPV_FORMAT_FLAG},
            {1006, "speed", MPV_FORMAT_DOUBLE},
            {1007, "track-list", MPV_FORMAT_NODE},
            {1008, "chapter-list", MPV_FORMAT_NODE},
            {1009, "hwdec-current", MPV_FORMAT_STRING},
        };
        for (const auto& observation : observations) {
            if (const auto error = mpv_observe_property(handle, observation.id,
                                                        observation.name, observation.format);
                error < 0) {
                postFatalError("observe", QStringLiteral("%1: %2")
                                         .arg(observation.name, mpvError(error)));
                mpv_terminate_destroy(handle_.exchange(nullptr, std::memory_order_acq_rel));
                return false;
            }
        }
        return true;
    }

    void drainEvents() {
        auto* handle = handle_.load(std::memory_order_acquire);
        if (handle == nullptr) {
            return;
        }
        for (;;) {
            auto* event = mpv_wait_event(handle, 0.0);
            if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
                return;
            }
            processEvent(*event);
        }
    }

    void processCommand(const Command& command) {
        auto* handle = handle_.load(std::memory_order_acquire);
        if (handle == nullptr) {
            postCommandFailure(command.id, "not_initialized", "libmpv is not initialized.");
            return;
        }

        switch (command.kind) {
        case CommandKind::load: {
            if (loadCommandInFlight_) {
                if (deferredLoad_.has_value()) {
                    const auto superseded = deferredLoad_->id;
                    postCommandFailure(superseded, "superseded",
                                       "A newer media load replaced this pending load.");
                }
                deferredLoad_ = command;
                return;
            }
            startLoad(command);
            return;
        }
        case CommandKind::addSubtitle: {
            const auto canonical = canonicalMediaPath(command.path);
            if (!canonical.has_value()) {
                postCommandFailure(command.id, "invalid_path",
                                   "The subtitle file must be a regular file inside an approved root.");
                return;
            }
            issueCommand(command.id, {QByteArrayLiteral("sub-add"), canonical->toUtf8(),
                                      QByteArrayLiteral("select")});
            return;
        }
        case CommandKind::play:
            issueFlagProperty(command.id, "pause", false);
            return;
        case CommandKind::pause:
            issueFlagProperty(command.id, "pause", true);
            return;
        case CommandKind::stop:
            issueCommand(command.id, {QByteArrayLiteral("stop")});
            return;
        case CommandKind::seekAbsolute:
            issueCommand(command.id, {QByteArrayLiteral("seek"), QByteArray::number(
                                          static_cast<double>(command.integer) / 1000.0, 'f', 3),
                                      QByteArrayLiteral("absolute+exact")});
            return;
        case CommandKind::seekRelative:
            issueCommand(command.id, {QByteArrayLiteral("seek"), QByteArray::number(
                                          static_cast<double>(command.integer) / 1000.0, 'f', 3),
                                      QByteArrayLiteral("relative+exact")});
            return;
        case CommandKind::setVolume:
            issueDoubleProperty(command.id, "volume", std::clamp(command.number, 0.0, 100.0));
            return;
        case CommandKind::setMuted:
            issueFlagProperty(command.id, "mute", command.flag);
            return;
        case CommandKind::setRate:
            issueDoubleProperty(command.id, "speed", std::clamp(command.number, 0.1, 8.0));
            return;
        case CommandKind::selectAudio:
            issueIntegerProperty(command.id, "aid", command.integer);
            return;
        case CommandKind::selectSubtitle:
            if (command.integer < 0) {
                issueStringProperty(command.id, "sid", QByteArrayLiteral("no"));
            } else {
                issueIntegerProperty(command.id, "sid", command.integer);
            }
            return;
        case CommandKind::selectChapter:
            issueIntegerProperty(command.id, "chapter", command.integer);
            return;
        case CommandKind::frameStep:
            issueCommand(command.id, {QByteArrayLiteral("frame-step")});
            return;
        case CommandKind::screenshot: {
            const auto canonical = canonicalOutputPath(command.path);
            if (!canonical.has_value()) {
                postCommandFailure(command.id, "invalid_path",
                                   "The screenshot destination must be inside an approved root.");
                return;
            }
            issueCommand(command.id, {QByteArrayLiteral("screenshot-to-file"), canonical->toUtf8(),
                                      QByteArrayLiteral("video")});
            return;
        }
        }
    }

    void startLoad(const Command& command) {
        const auto canonical = canonicalMediaPath(command.path);
        if (!canonical.has_value()) {
            postCommandFailure(command.id, "invalid_path",
                               "The media file must be a regular file inside an approved root.");
            startDeferredLoad();
            return;
        }
        ++positionGeneration_;
        suppressPositions_.store(true, std::memory_order_release);
        restore_ = RestoreState{true, *canonical, std::max<qint64>(0, command.integer), command.id};
        loadCommandInFlight_ = true;
        currentPath_ = *canonical;
        emit owner_->fileLoading(*canonical, command.id);
        issueCommand(command.id, {QByteArrayLiteral("loadfile"), canonical->toUtf8(),
                                  QByteArrayLiteral("replace")});
        if (!pendingReplies_.contains(command.id)) {
            restore_ = {};
            loadCommandInFlight_ = false;
            suppressPositions_.store(false, std::memory_order_release);
            startDeferredLoad();
        }
    }

    void startDeferredLoad() {
        if (loadCommandInFlight_ || !deferredLoad_.has_value()) {
            return;
        }
        const auto command = std::move(*deferredLoad_);
        deferredLoad_.reset();
        startLoad(command);
    }

    void issueCommand(Player::RequestId requestId, std::initializer_list<QByteArray> arguments,
                      ReplyAction action = ReplyAction::user,
                      Player::RequestId userId = 0) {
        std::vector<QByteArray> bytes(arguments.begin(), arguments.end());
        std::vector<const char*> pointers;
        pointers.reserve(bytes.size() + 1U);
        for (const auto& value : bytes) {
            pointers.push_back(value.constData());
        }
        pointers.push_back(nullptr);
        const auto error = mpv_command_async(handle_.load(std::memory_order_acquire), requestId,
                                             pointers.data());
        if (error < 0) {
            if (action == ReplyAction::user) {
                postCommandFailure(requestId, "mpv_command", mpvError(error));
            } else {
                restoreFailed(mpvError(error));
            }
            return;
        }
        pendingReplies_.emplace(requestId, PendingReply{action, userId == 0 ? requestId : userId});
    }

    void issueFlagProperty(Player::RequestId requestId, const char* name, bool value,
                           ReplyAction action = ReplyAction::user) {
        int flag = value ? 1 : 0;
        issueProperty(requestId, name, MPV_FORMAT_FLAG, &flag, action);
    }

    void issueDoubleProperty(Player::RequestId requestId, const char* name, double value,
                             ReplyAction action = ReplyAction::user) {
        issueProperty(requestId, name, MPV_FORMAT_DOUBLE, &value, action);
    }

    void issueIntegerProperty(Player::RequestId requestId, const char* name, qint64 value,
                              ReplyAction action = ReplyAction::user) {
        int64_t integer = static_cast<int64_t>(value);
        issueProperty(requestId, name, MPV_FORMAT_INT64, &integer, action);
    }

    void issueStringProperty(Player::RequestId requestId, const char* name, const QByteArray& value,
                             ReplyAction action = ReplyAction::user) {
        auto* string = const_cast<char*>(value.constData());
        issueProperty(requestId, name, MPV_FORMAT_STRING, &string, action);
    }

    void issueProperty(Player::RequestId requestId, const char* name, mpv_format format,
                       void* value, ReplyAction action) {
        const auto error = mpv_set_property_async(handle_.load(std::memory_order_acquire), requestId,
                                                  name, format, value);
        if (error < 0) {
            if (action == ReplyAction::user) {
                postCommandFailure(requestId, "mpv_property", mpvError(error));
            } else {
                restoreFailed(mpvError(error));
            }
            return;
        }
        pendingReplies_.emplace(requestId, PendingReply{action, requestId});
    }

    void processEvent(const mpv_event& event) {
        switch (event.event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            processPropertyChange(static_cast<const mpv_event_property*>(event.data));
            return;
        case MPV_EVENT_COMMAND_REPLY:
        case MPV_EVENT_SET_PROPERTY_REPLY:
            processReply(event);
            return;
        case MPV_EVENT_FILE_LOADED:
            beginRestore();
            return;
        case MPV_EVENT_END_FILE: {
            const auto* end = static_cast<const mpv_event_end_file*>(event.data);
            const auto endedPath = currentPath_;
            const bool failed = end != nullptr && end->reason == MPV_END_FILE_REASON_ERROR;
            const bool naturalEnd = end != nullptr && end->reason == MPV_END_FILE_REASON_EOF;
            if (restore_.active) {
                if (failed) {
                    restoreFailed(mpvError(end->error));
                } else {
                    restore_ = {};
                    loadCommandInFlight_ = false;
                    suppressPositions_.store(false, std::memory_order_release);
                    startDeferredLoad();
                }
            }
            if (naturalEnd || failed) {
                emit owner_->playbackEnded(endedPath, failed);
            }
            if (failed && end != nullptr) {
                postFatalError("playback", mpvError(end->error));
            }
            return;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto* message = static_cast<const mpv_event_log_message*>(event.data);
            if (message != nullptr && message->log_level <= MPV_LOG_LEVEL_FATAL) {
                postFatalError("mpv_log", utf8(message->text));
            }
            return;
        }
        case MPV_EVENT_SHUTDOWN:
            requestShutdownOnOwnerThread();
            return;
        default:
            return;
        }
    }

    void processPropertyChange(const mpv_event_property* property) {
        if (property == nullptr || property->name == nullptr) {
            return;
        }
        const auto name = QByteArray(property->name);
        if (name == "time-pos" && property->format == MPV_FORMAT_DOUBLE && property->data != nullptr) {
            if (suppressPositions_.load(std::memory_order_acquire)) {
                return;
            }
            const auto seconds = *static_cast<const double*>(property->data);
            publishPosition(static_cast<qint64>(std::max(0.0, seconds) * 1000.0), durationMs_);
        } else if (name == "duration" && property->format == MPV_FORMAT_DOUBLE && property->data != nullptr) {
            const auto seconds = *static_cast<const double*>(property->data);
            durationMs_ = static_cast<qint64>(std::max(0.0, seconds) * 1000.0);
            if (!suppressPositions_.load(std::memory_order_acquire)) {
                publishPosition(lastPositionMs_, durationMs_);
            }
        } else if (name == "pause" && property->format == MPV_FORMAT_FLAG && property->data != nullptr) {
            emit owner_->pausedChanged(*static_cast<const int*>(property->data) != 0);
        } else if (name == "volume" && property->format == MPV_FORMAT_DOUBLE && property->data != nullptr) {
            emit owner_->volumeChanged(*static_cast<const double*>(property->data));
        } else if (name == "mute" && property->format == MPV_FORMAT_FLAG && property->data != nullptr) {
            emit owner_->mutedChanged(*static_cast<const int*>(property->data) != 0);
        } else if (name == "speed" && property->format == MPV_FORMAT_DOUBLE && property->data != nullptr) {
            emit owner_->rateChanged(*static_cast<const double*>(property->data));
        } else if (name == "track-list") {
            emit owner_->tracksChanged(parseTracks(property));
        } else if (name == "chapter-list") {
            emit owner_->chaptersChanged(parseChapters(property));
        } else if (name == "hwdec-current") {
            const auto* decoder = property->format == MPV_FORMAT_STRING && property->data
                ? *static_cast<char* const*>(property->data) : nullptr;
            emit owner_->decoderChanged(decoder ? QString::fromUtf8(decoder) : QString());
        }
    }

    void processReply(const mpv_event& event) {
        const auto iterator = pendingReplies_.find(event.reply_userdata);
        if (iterator == pendingReplies_.end()) {
            return;
        }
        const auto reply = iterator->second;
        pendingReplies_.erase(iterator);
        if (event.error < 0) {
            const auto message = mpvError(event.error);
            if (reply.action == ReplyAction::user) {
                postCommandFailure(reply.userId, "mpv", message);
            } else {
                restoreFailed(message);
            }
            return;
        }
        if (reply.action == ReplyAction::user) {
            postCommandFinished(reply.userId);
        } else if (reply.action == ReplyAction::restorePause) {
            if (restore_.positionMs > 0) {
                const auto id = nextInternalRequestId();
                issueCommand(id, {QByteArrayLiteral("seek"), QByteArray::number(
                                      static_cast<double>(restore_.positionMs) / 1000.0, 'f', 3),
                                  QByteArrayLiteral("absolute+exact")},
                             ReplyAction::restoreSeek);
            } else {
                finishRestore();
            }
        } else if (reply.action == ReplyAction::restoreSeek) {
            finishRestore();
        }
    }

    void beginRestore() {
        if (!restore_.active) {
            suppressPositions_.store(false, std::memory_order_release);
            emit owner_->fileLoaded(currentPath_, durationMs_, lastPositionMs_, positionGeneration_);
            return;
        }
        const auto id = nextInternalRequestId();
        issueFlagProperty(id, "pause", true, ReplyAction::restorePause);
    }

    void restoreFailed(const QString& message) {
        if (!restore_.active) {
            return;
        }
        postFatalError("restore_position", message);
        restore_ = {};
        loadCommandInFlight_ = false;
        suppressPositions_.store(false, std::memory_order_release);
        startDeferredLoad();
    }

    void finishRestore() {
        const auto path = restore_.path;
        const auto position = restore_.positionMs;
        const auto generation = restore_.loadRequestId;
        restore_ = {};
        loadCommandInFlight_ = false;
        lastPositionMs_ = position;
        suppressPositions_.store(false, std::memory_order_release);
        // The property notification may follow its command reply. Publish the
        // confirmed pause before consumers enable the Play button.
        emit owner_->pausedChanged(true);
        emit owner_->fileLoaded(path, durationMs_, position, generation);
        publishPosition(position, durationMs_);
        startDeferredLoad();
    }

    [[nodiscard]] Player::RequestId nextInternalRequestId() {
        return kInternalRequestBit | nextInternalId_++;
    }

    [[nodiscard]] std::optional<QString> canonicalMediaPath(const QString& input) const {
        for (const auto& path : approvedRoots()) {
            const auto root = local_files::LocalFiles::validateRoot(path);
            if (!root) continue;
            const auto file = local_files::LocalFiles::validateFile(*root, input);
            if (file) return file->path;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<QString> canonicalOutputPath(const QString& input) const {
        for (const auto& path : approvedRoots()) {
            const auto root = local_files::LocalFiles::validateRoot(path);
            if (!root) continue;
            const auto file = local_files::LocalFiles::validateScreenshotDestination(*root, input);
            if (file) return file->path;
        }
        return std::nullopt;
    }

    [[nodiscard]] QVector<PlayerTrack> parseTracks(const mpv_event_property* property) const {
        QVector<PlayerTrack> tracks;
        if (property == nullptr || property->format != MPV_FORMAT_NODE || property->data == nullptr) {
            return tracks;
        }
        const auto* node = static_cast<const mpv_node*>(property->data);
        if (node->format != MPV_FORMAT_NODE_ARRAY || node->u.list == nullptr || node->u.list->values == nullptr) {
            return tracks;
        }
        const auto count = std::clamp(node->u.list->num, 0, 4096);
        tracks.reserve(count);
        for (int index = 0; index < count; ++index) {
            const auto& item = node->u.list->values[index];
            PlayerTrack track;
            track.id = static_cast<int>(nodeInteger(mapValue(item, "id")));
            track.type = nodeString(mapValue(item, "type"));
            track.title = nodeString(mapValue(item, "title"));
            track.language = nodeString(mapValue(item, "lang"));
            track.codec = nodeString(mapValue(item, "codec"));
            track.selected = nodeFlag(mapValue(item, "selected"));
            track.external = nodeFlag(mapValue(item, "external"));
            tracks.push_back(std::move(track));
        }
        return tracks;
    }

    [[nodiscard]] QVector<PlayerChapter> parseChapters(const mpv_event_property* property) const {
        QVector<PlayerChapter> chapters;
        if (property == nullptr || property->format != MPV_FORMAT_NODE || property->data == nullptr) {
            return chapters;
        }
        const auto* node = static_cast<const mpv_node*>(property->data);
        if (node->format != MPV_FORMAT_NODE_ARRAY || node->u.list == nullptr || node->u.list->values == nullptr) {
            return chapters;
        }
        const auto count = std::clamp(node->u.list->num, 0, 4096);
        chapters.reserve(count);
        for (int index = 0; index < count; ++index) {
            const auto& item = node->u.list->values[index];
            PlayerChapter chapter;
            chapter.index = index;
            chapter.title = nodeString(mapValue(item, "title"));
            chapter.timeMs = static_cast<qint64>(std::max(0.0, nodeNumber(mapValue(item, "time"))) * 1000.0);
            chapters.push_back(std::move(chapter));
        }
        return chapters;
    }

    void publishPosition(qint64 positionMs, qint64 durationMs) {
        lastPositionMs_ = positionMs;
        pendingPositionMs_.store(positionMs, std::memory_order_release);
        pendingDurationMs_.store(durationMs, std::memory_order_release);
        pendingPositionGeneration_.store(positionGeneration_, std::memory_order_release);
        pendingPositionRevision_.fetch_add(1, std::memory_order_acq_rel);
        bool expected = false;
        if (!positionDeliveryPosted_.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel)) {
            return;
        }
        QMetaObject::invokeMethod(owner_, [this] {
            for (;;) {
                const auto revision = pendingPositionRevision_.load(std::memory_order_acquire);
                const auto generation = pendingPositionGeneration_.load(std::memory_order_acquire);
                const auto position = pendingPositionMs_.load(std::memory_order_acquire);
                const auto duration = pendingDurationMs_.load(std::memory_order_acquire);
                if (!suppressPositions_.load(std::memory_order_acquire)
                    && generation == positionGeneration_.load(std::memory_order_acquire)) {
                    owner_->positionChanged(position, duration);
                }
                positionDeliveryPosted_.store(false, std::memory_order_release);
                if (revision == pendingPositionRevision_.load(std::memory_order_acquire)) {
                    return;
                }
                bool expected = false;
                if (!positionDeliveryPosted_.compare_exchange_strong(expected, true,
                                                                      std::memory_order_acq_rel)) {
                    return;
                }
            }
        }, Qt::QueuedConnection);
    }

    void rejectPending(const QString& code, const QString& message) {
        std::vector<Player::RequestId> ids;
        {
            std::lock_guard lock(mutex_);
            ids.reserve(queued_.size() + coalesced_.size() + (deferredLoad_.has_value() ? 1U : 0U));
            for (const auto& command : queued_) {
                ids.push_back(command.id);
            }
            for (const auto& [unused, command] : coalesced_) {
                ids.push_back(command.id);
            }
            if (deferredLoad_.has_value()) {
                ids.push_back(deferredLoad_->id);
                deferredLoad_.reset();
            }
            queued_.clear();
            coalesced_.clear();
            std::lock_guard reservationLock(reservations_->mutex);
            for (const auto id : ids) {
                reservations_->ids.erase(id);
            }
        }
        for (const auto id : ids) {
            postCommandFailure(id, code, message);
        }
    }

    static void releaseReservation(const std::shared_ptr<ReservationState>& reservations,
                                   Player::RequestId requestId) {
        if (requestId == 0 || (requestId & kInternalRequestBit) != 0) {
            return;
        }
        std::lock_guard lock(reservations->mutex);
        reservations->ids.erase(requestId);
    }

    void postCommandFailure(Player::RequestId requestId, const QString& code, const QString& message) {
        const auto reservations = reservations_;
        QMetaObject::invokeMethod(owner_, [owner = owner_, reservations, requestId, code, message] {
            emit owner->commandFailed(requestId, code, message);
            releaseReservation(reservations, requestId);
        }, Qt::QueuedConnection);
    }

    void postCommandFinished(Player::RequestId requestId) {
        const auto reservations = reservations_;
        QMetaObject::invokeMethod(owner_, [owner = owner_, reservations, requestId] {
            emit owner->commandFinished(requestId);
            releaseReservation(reservations, requestId);
        }, Qt::QueuedConnection);
    }

    void postFatalError(const QString& code, const QString& message) const {
        QMetaObject::invokeMethod(owner_, [owner = owner_, code, message] {
            emit owner->fatalError(code, message);
        }, Qt::QueuedConnection);
    }

    Player* owner_;
    const Player::DecodeMode decoding_;
    mutable std::mutex mutex_;
    std::condition_variable commandAvailable_;
    QStringList roots_;
    std::deque<Command> queued_;
    std::map<int, Command> coalesced_;
    std::shared_ptr<ReservationState> reservations_ = std::make_shared<ReservationState>();
    std::optional<Command> deferredLoad_;
    bool loadCommandInFlight_ = false;
    bool started_ = false;
    bool stopping_ = false;
    bool shutdownNotified_ = false;
    bool shutdownQueued_ = false;
    std::thread worker_;

    std::atomic<bool> ready_{false};
    std::atomic<mpv_handle*> handle_{nullptr};
    std::atomic<mpv_render_context*> renderContext_{nullptr};
    std::atomic<quint64> positionGeneration_{0};
    std::atomic<bool> suppressPositions_{true};
    std::atomic<qint64> pendingPositionMs_{0};
    std::atomic<qint64> pendingDurationMs_{0};
    std::atomic<quint64> pendingPositionGeneration_{0};
    std::atomic<quint64> pendingPositionRevision_{0};
    std::atomic<bool> positionDeliveryPosted_{false};
    std::uint64_t nextRequestId_ = 1;
    std::uint64_t nextInternalId_ = 1;
    std::unordered_map<Player::RequestId, PendingReply> pendingReplies_;
    RestoreState restore_;
    QString currentPath_;
    qint64 durationMs_ = 0;
    qint64 lastPositionMs_ = 0;
};

Player::Player(QObject* parent, DecodeMode decoding) : QObject(parent), impl_(std::make_unique<Impl>(*this, decoding)) {
    qRegisterMetaType<PlayerTrack>();
    qRegisterMetaType<PlayerChapter>();
    qRegisterMetaType<QVector<PlayerTrack>>();
    qRegisterMetaType<QVector<PlayerChapter>>();
}

Player::~Player() {
    shutdown();
}

void Player::setApprovedRoots(const QStringList& roots) { impl_->setApprovedRoots(roots); }

QStringList Player::approvedRoots() const { return impl_->approvedRoots(); }

void Player::start() { impl_->start(); }

void Player::shutdown() { impl_->shutdown(); }

bool Player::isReady() const { return impl_->isReady(); }

Player::RequestId Player::loadFile(const QString& path, qint64 savedPositionMs) {
    return impl_->enqueue(Command{0, CommandKind::load, path, savedPositionMs});
}

Player::RequestId Player::addSubtitleFile(const QString& path) {
    return impl_->enqueue(Command{0, CommandKind::addSubtitle, path});
}

Player::RequestId Player::play() { return impl_->enqueue(commandOf(CommandKind::play)); }

Player::RequestId Player::pause() { return impl_->enqueue(commandOf(CommandKind::pause)); }

Player::RequestId Player::stop() { return impl_->enqueue(commandOf(CommandKind::stop)); }

Player::RequestId Player::seek(qint64 positionMs) {
    return impl_->enqueue(Command{0, CommandKind::seekAbsolute, {}, std::max<qint64>(0, positionMs), 0.0, false, 1});
}

Player::RequestId Player::seekRelative(qint64 deltaMs) {
    return impl_->enqueue(Command{0, CommandKind::seekRelative, {}, deltaMs, 0.0, false, 1});
}

Player::RequestId Player::setVolume(double volume) {
    return impl_->enqueue(Command{0, CommandKind::setVolume, {}, 0, volume, false, 2});
}

Player::RequestId Player::setMuted(bool muted) {
    return impl_->enqueue(Command{0, CommandKind::setMuted, {}, 0, 0.0, muted, 3});
}

Player::RequestId Player::setRate(double rate) {
    return impl_->enqueue(Command{0, CommandKind::setRate, {}, 0, rate, false, 4});
}

Player::RequestId Player::selectAudioTrack(int trackId) {
    return impl_->enqueue(Command{0, CommandKind::selectAudio, {}, trackId});
}

Player::RequestId Player::selectSubtitleTrack(int trackId) {
    return impl_->enqueue(Command{0, CommandKind::selectSubtitle, {}, trackId});
}

Player::RequestId Player::selectChapter(int chapterIndex) {
    return impl_->enqueue(Command{0, CommandKind::selectChapter, {}, chapterIndex});
}

Player::RequestId Player::frameStep() { return impl_->enqueue(commandOf(CommandKind::frameStep)); }

Player::RequestId Player::screenshot(const QString& outputPath) {
    return impl_->enqueue(Command{0, CommandKind::screenshot, outputPath});
}

bool Player::createRenderContext(OpenGLProcAddress getProcAddress, void* getProcAddressContext,
                                 RenderUpdateCallback updateCallback, void* updateCallbackContext) {
    return impl_->createRenderContext(getProcAddress, getProcAddressContext, updateCallback,
                                      updateCallbackContext);
}

void Player::destroyRenderContext() { impl_->destroyRenderContext(); }

bool Player::renderFrame(int framebufferObject, int width, int height, int internalFormat, bool flipY) {
    return impl_->renderFrame(framebufferObject, width, height, internalFormat, flipY);
}

bool Player::hasRenderContext() const { return impl_->hasRenderContext(); }

}  // namespace melearner
