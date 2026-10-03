#include "course_document_view.hpp"

#include "local_files.hpp"
#include "theme.hpp"

#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QPointer>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QThreadPool>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include <md4c-html.h>
#include <md4c.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace melearner {
namespace {

constexpr auto kScheme = "melearner-course";
constexpr qsizetype kMaxResourceBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaxAggregateBytes = 128 * 1024 * 1024;
constexpr int kMaxRequests = 512;
std::atomic<int> pendingAssets{0};
QThreadPool &assetPool() {
  static QThreadPool pool;
  static const bool configured = [] {
    pool.setMaxThreadCount(2);
    return true;
  }();
  Q_UNUSED(configured);
  return pool;
}
struct ResourceRead {
  QByteArray bytes;
  bool valid = false;
};

// Configure before QApplication without creating WebEngine objects or processes.
const bool kSchemeRegistered = [] {
  // WebEngine's Quick surface shares a window with the OpenGL video canvas.
  // Mixing the native Metal or Direct3D default with that canvas cannot render.
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
  QWebEngineUrlScheme scheme{QByteArray(kScheme)};
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
  scheme.setFlags(QWebEngineUrlScheme::SecureScheme |
                  QWebEngineUrlScheme::CorsEnabled |
                  QWebEngineUrlScheme::FetchApiAllowed);
  QWebEngineUrlScheme::registerScheme(scheme);
  return true;
}();

QString freshToken() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces) +
         QString::number(QRandomGenerator::system()->generate64(), 16);
}

QString errorText(const local_files::Error &error) { return error.message; }

struct PreparedDocument {
  local_files::ValidatedRoot root;
  QString relativePath;
  QByteArray contents;
  QString error;
};

QByteArray markdownHtml(const QByteArray &markdown, const QString &css);

void configurePrivateWebEngineRuntime() {
  const QDir appDir(QCoreApplication::applicationDirPath());
  const QString process = QDir::cleanPath(appDir.filePath(
      QStringLiteral("../libexec/melearner/QtWebEngineProcess")));
  const QString resources = QDir::cleanPath(appDir.filePath(
      QStringLiteral("../share/melearner/qtwebengine/resources")));
  const QString locales = QDir::cleanPath(appDir.filePath(
      QStringLiteral("../share/melearner/qtwebengine/locales")));
  if (!QFileInfo(process).isExecutable() || !QDir(resources).exists() ||
      !QDir(locales).exists())
    return;
  qputenv("QTWEBENGINEPROCESS_PATH", QFile::encodeName(process));
  qputenv("QTWEBENGINE_RESOURCES_PATH", QFile::encodeName(resources));
  qputenv("QTWEBENGINE_LOCALES_PATH", QFile::encodeName(locales));
}

PreparedDocument prepareDocument(QString courseRoot, QString filePath,
                                 QString markdownCss) {
  PreparedDocument prepared;
  const auto root = local_files::LocalFiles::validateRoot(courseRoot);
  if (!root) {
    prepared.error = errorText(root.error());
    return prepared;
  }
  const auto entry = local_files::LocalFiles::validateFile(*root, filePath);
  if (!entry) {
    prepared.error = errorText(entry.error());
    return prepared;
  }
  const QString suffix = QFileInfo(entry->path).suffix().toCaseFolded();
  const bool markdown =
      suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown");
  if (!markdown && suffix != QStringLiteral("html") &&
      suffix != QStringLiteral("htm")) {
    prepared.error = QStringLiteral(
        "Only HTML and Markdown course documents can be shown here.");
    return prepared;
  }
  auto source = local_files::LocalFiles::openRead(*root, entry->path);
  if (!source) {
    prepared.error = errorText(source.error());
    return prepared;
  }
  QFile &file = **source;
  if (file.size() < 0 || file.size() > kMaxResourceBytes) {
    prepared.error =
        QStringLiteral("The course document exceeds the 32 MiB display limit.");
    return prepared;
  }
  QByteArray contents = file.read(kMaxResourceBytes + 1);
  if (file.error() != QFileDevice::NoError ||
      contents.size() > kMaxResourceBytes || contents.size() != file.size()) {
    prepared.error =
        QStringLiteral("The course document could not be read completely.");
    return prepared;
  }
  if (markdown) {
    contents = markdownHtml(contents, markdownCss);
    if (contents.isEmpty()) {
      prepared.error =
          QStringLiteral("The Markdown document could not be rendered.");
      return prepared;
    }
  }
  if (contents.size() > kMaxResourceBytes) {
    prepared.error = QStringLiteral(
        "The rendered course document exceeds the 32 MiB display limit.");
    return prepared;
  }
  prepared.root = *root;
  prepared.relativePath = QDir(root->path).relativeFilePath(entry->path);
  prepared.contents = std::move(contents);
  return prepared;
}

class CourseHandler final : public QWebEngineUrlSchemeHandler {
public:
  CourseHandler(local_files::ValidatedRoot root, QString token,
                QString entryRelative, QByteArray entryContents,
                std::function<void(const QString &)> denied,
                QObject *parent)
      : QWebEngineUrlSchemeHandler(parent), root_(std::move(root)),
        token_(std::move(token)), entryRelative_(std::move(entryRelative)),
        entryContents_(std::move(entryContents)), denied_(std::move(denied)) {}

  void requestStarted(QWebEngineUrlRequestJob *job) override {
    if (++requestCount_ > kMaxRequests) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    const QUrl url = job->requestUrl();
    if (url.scheme() != QLatin1String(kScheme) || url.host() != token_ ||
        url.userInfo().size() != 0 || url.port(-1) != -1 ||
        job->requestMethod() != QByteArrayLiteral("GET")) {
      job->fail(QWebEngineUrlRequestJob::UrlNotFound);
      return;
    }

    QString relative = url.path(QUrl::FullyDecoded);
    if (relative.startsWith(QChar('/')))
      relative.remove(0, 1);
    if (relative.isEmpty() || relative.startsWith(QChar('/')) ||
        relative.contains(QChar('\\')) || relative.contains(QChar(0)) ||
        relative.toUtf8().size() > local_files::LocalFiles::kMaxPathBytes) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    const QStringList components =
        relative.split(QChar('/'), Qt::KeepEmptyParts);
    if (std::any_of(components.cbegin(), components.cend(),
                    [](const QString &part) {
                      return part.isEmpty() || part == QStringLiteral(".");
                    })) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }

    const QString path = QDir::cleanPath(QDir(root_.path).filePath(relative));
    const QString rootPrefix = root_.path == QStringLiteral("/")
                                   ? root_.path
                                   : root_.path + QDir::separator();
    if (path != root_.path && !path.startsWith(rootPrefix)) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    // Bound queued reads across profiles, including documents just left.
    if (pendingAssets.fetch_add(1) >= 16) {
      pendingAssets.fetch_sub(1);
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    auto *watcher = new QFutureWatcher<ResourceRead>(job);
    const QPointer<QWebEngineUrlRequestJob> pendingJob(job);
    connect(watcher, &QFutureWatcher<ResourceRead>::finished, this,
            [this, watcher, pendingJob, relative, path] {
              const auto read = watcher->result();
              watcher->deleteLater();
              if (!pendingJob)
                return;
              if (!read.valid) {
                denied_(path);
                pendingJob->fail(QWebEngineUrlRequestJob::UrlNotFound);
                return;
              }
              reply(pendingJob, relative, path, read.bytes);
            });
    const auto root = root_;
    const bool entry = relative == entryRelative_;
    const auto prepared = entry ? entryContents_ : QByteArray{};
    watcher->setFuture(
        QtConcurrent::run(&assetPool(), [root, path, entry, prepared] {
          const auto release = qScopeGuard([] { pendingAssets.fetch_sub(1); });
          auto opened = local_files::LocalFiles::openRead(root, path);
          if (!opened)
            return ResourceRead{};
          if (entry)
            return ResourceRead{prepared, true};
          QFile &file = **opened;
          if (file.size() < 0 || file.size() > kMaxResourceBytes)
            return ResourceRead{};
          auto bytes = file.read(kMaxResourceBytes + 1);
          const bool valid = file.error() == QFileDevice::NoError &&
                             bytes.size() <= kMaxResourceBytes &&
                             bytes.size() == file.size();
          return ResourceRead{std::move(bytes), valid};
        }));
  }

private:
  void reply(QWebEngineUrlRequestJob *job, const QString &relative,
             const QString &path, const QByteArray &bytes) {
    if (bytes.size() > kMaxResourceBytes) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    if (bytes.size() > kMaxAggregateBytes - aggregateBytes_) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    aggregateBytes_ += bytes.size();
    auto *buffer = new QBuffer(job);
    buffer->setData(bytes);
    if (!buffer->open(QIODevice::ReadOnly)) {
      job->fail(QWebEngineUrlRequestJob::RequestFailed);
      return;
    }
    const QString mime =
        relative == entryRelative_
            ? QStringLiteral("text/html")
            : QMimeDatabase()
                  .mimeTypeForFile(path, QMimeDatabase::MatchExtension)
                  .name();
    job->setAdditionalResponseHeaders(
        {{QByteArrayLiteral("Content-Security-Policy"),
          QByteArrayLiteral(
              "default-src 'self' data: blob:; script-src 'self' "
              "'unsafe-inline' 'unsafe-eval'; style-src 'self' "
              "'unsafe-inline'; img-src 'self' data: blob:; connect-src "
              "'self'; object-src 'none'; frame-src 'self'; worker-src 'none'; "
              "form-action 'none'; base-uri 'self'")}});
    job->reply(mime.toLatin1(), buffer);
  }

private:
  local_files::ValidatedRoot root_;
  QString token_;
  QString entryRelative_;
  QByteArray entryContents_;
  std::function<void(const QString &)> denied_;
  int requestCount_ = 0;
  qsizetype aggregateBytes_ = 0;
};

class OfflineInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
  OfflineInterceptor(QString token, QObject *parent)
      : QWebEngineUrlRequestInterceptor(parent), token_(std::move(token)) {}

  void interceptRequest(QWebEngineUrlRequestInfo &info) override {
    const QUrl url = info.requestUrl();
    const QUrl firstParty = info.firstPartyUrl();
    const bool get = info.requestMethod() == QByteArrayLiteral("GET");
    const auto resource = info.resourceType();
    const bool subresource =
        resource != QWebEngineUrlRequestInfo::ResourceTypeMainFrame &&
        resource != QWebEngineUrlRequestInfo::ResourceTypeSubFrame;
    const bool course =
        url.scheme() == QLatin1String(kScheme) && url.host() == token_;
    const bool data =
        url.scheme() == QStringLiteral("data") && subresource && get;
    const bool blob =
        url.scheme() == QStringLiteral("blob") &&
        firstParty.scheme() == QLatin1String(kScheme) &&
        firstParty.host() == token_ &&
        url.toString().startsWith(QStringLiteral("blob:melearner-course://") +
                                  token_ + QChar('/'));
    if (!course && !data && !blob)
      info.block(true);
  }

private:
  QString token_;
};

class CoursePage final : public QWebEnginePage {
public:
  CoursePage(QWebEngineProfile *profile, QString token, QObject *parent)
      : QWebEnginePage(profile, parent), token_(std::move(token)) {}

protected:
  bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override {
    return url.scheme() == QLatin1String(kScheme) && url.host() == token_ &&
           url.userInfo().isEmpty() && url.port(-1) == -1;
  }

  QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
  void javaScriptAlert(const QUrl &, const QString &) override {}
  bool javaScriptConfirm(const QUrl &, const QString &) override {
    return false;
  }
  bool javaScriptPrompt(const QUrl &, const QString &, const QString &,
                        QString *) override {
    return false;
  }

private:
  QString token_;
};

struct MarkdownOutput {
  QByteArray bytes;
  bool oversized = false;
};

void appendHtml(const MD_CHAR *data, MD_SIZE size, void *context) {
  auto &output = *static_cast<MarkdownOutput *>(context);
  const auto count = static_cast<qsizetype>(size);
  if (count > kMaxResourceBytes - output.bytes.size()) {
    output.oversized = true;
    return;
  }
  output.bytes.append(data, count);
}

QString markdownCss(const QWidget *theme) {
  const auto appFont = QApplication::font();
  const qreal fontPx = appFont.pixelSize() > 0
                           ? appFont.pixelSize()
                           : appFont.pointSizeF() * 4.0 / 3.0;
  return QStringLiteral("html{background:%1;color-scheme:dark}body{max-width:"
                        "60rem;margin:2rem auto;padding:0 1.25rem;"
                        "background:%1;color:%2;font-family:'%3';font-size:%"
                        "4px;line-height:1.65}a{color:%5}"
                        "img{max-width:100%;height:auto}table{border-collapse:"
                        "collapse;display:block;overflow-x:auto}"
                        "th,td{border:1px solid %6;padding:.45rem "
                        ".7rem}pre{overflow:auto;padding:1rem;"
                        "background:%7;border-radius:.5rem}code{font-family:ui-"
                        "monospace,monospace}"
                        "blockquote{border-left:3px solid "
                        "%6;margin-left:0;padding-left:1rem;color:%8}"
                        "::-webkit-scrollbar{width:4px;height:4px}::-webkit-"
                        "scrollbar-thumb{background:%6;border-radius:4px}")
      .arg(
          roleColor(theme, shadcn::Role::Background).name(QColor::HexRgb),
          roleColor(theme, shadcn::Role::Foreground).name(QColor::HexRgb),
          appFont.family().replace(QChar('\''), QStringLiteral("\\'")),
          QString::number(fontPx, 'f', 2),
          roleColor(theme, shadcn::Role::Primary).name(QColor::HexRgb),
          roleColor(theme, shadcn::Role::Border).name(QColor::HexRgb),
          roleColor(theme, shadcn::Role::Card).name(QColor::HexRgb),
          roleColor(theme, shadcn::Role::MutedForeground).name(QColor::HexRgb));
}

QByteArray markdownHtml(const QByteArray &markdown, const QString &css) {
  MarkdownOutput output;
  const int result =
      md_html(markdown.constData(), static_cast<MD_SIZE>(markdown.size()),
              appendHtml, &output, MD_DIALECT_GITHUB, 0);
  if (result != 0 || output.oversized)
    return {};
  return QByteArrayLiteral(
             "<!doctype html><html><head><meta charset=\"utf-8\"><meta "
             "name=\"viewport\" content=\"width=device-width, "
             "initial-scale=1\"><style>") +
         css.toUtf8() + QByteArrayLiteral("</style></head><body>") +
         output.bytes + QByteArrayLiteral("</body></html>");
}

} // namespace

class CourseDocumentView::Private {
public:
  QWebEngineView *view = nullptr;
  QWebEngineProfile *profile = nullptr;
  CourseHandler *handler = nullptr;
  OfflineInterceptor *interceptor = nullptr;
  QString token;
  QVBoxLayout *layout = nullptr;
  QThreadPool pool;
  QFutureWatcher<PreparedDocument> *watcher = nullptr;
  quint64 generation = 0;

  Private() { pool.setMaxThreadCount(1); }
};

CourseDocumentView::CourseDocumentView(QWidget *parent)
    : QWidget(parent), d_(new Private) {
  Q_UNUSED(kSchemeRegistered);
  setObjectName(QStringLiteral("courseDocumentView"));
  d_->layout = new QVBoxLayout(this);
  d_->layout->setContentsMargins(0, 0, 0, 0);
}

CourseDocumentView::~CourseDocumentView() {
  clear();
  delete d_;
}

void CourseDocumentView::clear() {
  ++d_->generation;
  d_->pool.clear();
  if (d_->watcher != nullptr) {
    d_->watcher->disconnect(this);
    delete d_->watcher;
    d_->watcher = nullptr;
  }
  if (d_->view != nullptr) {
    d_->layout->removeWidget(d_->view);
    delete d_->view;
    d_->view = nullptr;
  }
  if (d_->profile != nullptr) {
    delete d_->profile;
    d_->profile = nullptr;
  }
  d_->handler = nullptr;
  d_->interceptor = nullptr;
  d_->token.clear();
}

void CourseDocumentView::open(const QString &courseRoot,
                              const QString &filePath) {
  clear();
  const quint64 generation = d_->generation;
  const QString css = markdownCss(this);
  d_->watcher = new QFutureWatcher<PreparedDocument>(this);
  auto *watcher = d_->watcher;
  connect(
      watcher, &QFutureWatcher<PreparedDocument>::finished, this,
      [this, watcher, generation] {
        const PreparedDocument prepared = watcher->result();
        if (d_->watcher == watcher)
          d_->watcher = nullptr;
        watcher->deleteLater();
        if (generation != d_->generation)
          return;
        if (!prepared.error.isEmpty()) {
          emit error(prepared.error);
          emit loaded(false);
          return;
        }

        configurePrivateWebEngineRuntime();
        d_->token = freshToken();
        const QString relative = prepared.relativePath;
        d_->profile =
            new QWebEngineProfile(this); // unnamed profile is off-the-record
        d_->profile->setHttpCacheType(QWebEngineProfile::NoCache);
        d_->profile->setPersistentCookiesPolicy(
            QWebEngineProfile::NoPersistentCookies);
        d_->handler = new CourseHandler(prepared.root, d_->token, relative,
                                        prepared.contents,
                                        [this](const QString &path) { emit resourceDenied(path); },
                                        d_->profile);
        d_->profile->installUrlSchemeHandler(QByteArray(kScheme), d_->handler);
        d_->interceptor = new OfflineInterceptor(d_->token, d_->profile);
        d_->profile->setUrlRequestInterceptor(d_->interceptor);
        connect(
            d_->profile, &QWebEngineProfile::downloadRequested, d_->profile,
            [](QWebEngineDownloadRequest *download) { download->cancel(); });
        d_->view = new QWebEngineView(this);
        d_->view->setObjectName(QStringLiteral("documentBrowser"));
        auto *page = new CoursePage(d_->profile, d_->token, d_->view);
        d_->view->setPage(page);
        auto *settings = page->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                               false);
        settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard,
                               false);
        settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
                               true);
        settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled,
                               false);
        settings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly,
                               true);
        settings->setAttribute(
            QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        settings->setAttribute(
            QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        settings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
        settings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled,
                               false);
        settings->setAttribute(QWebEngineSettings::NavigateOnDropEnabled,
                               false);
        settings->setAttribute(QWebEngineSettings::BackForwardCacheEnabled,
                               false);
        settings->setUnknownUrlSchemePolicy(
            QWebEngineSettings::DisallowUnknownUrlSchemes);
        page->setBackgroundColor(Qt::white);
        connect(page, &QWebEnginePage::permissionRequested, page,
                [](QWebEnginePermission permission) { permission.deny(); });
        QWebEngineScript disableRtc;
        disableRtc.setName(QStringLiteral("course-disable-realtime-network"));
        disableRtc.setInjectionPoint(QWebEngineScript::DocumentCreation);
        disableRtc.setWorldId(QWebEngineScript::MainWorld);
        disableRtc.setRunsOnSubFrames(true);
        disableRtc.setSourceCode(QStringLiteral(
            "(()=>{for(const k of "
            "['RTCPeerConnection','webkitRTCPeerConnection','WebTransport'])"
            "try{Object.defineProperty(globalThis,k,{value:undefined,writable:"
            "false,configurable:false})}catch(e){}})();"));
        page->scripts().insert(disableRtc);
        connect(d_->view, &QWebEngineView::loadFinished, this, [this](bool ok) {
          if (!ok)
            emit error(QStringLiteral("The course document failed to load."));
          emit loaded(ok);
        });
        d_->layout->addWidget(d_->view);

        QUrl url;
        url.setScheme(QLatin1String(kScheme));
        url.setHost(d_->token);
        url.setPath(QLatin1Char('/') + relative);
        d_->view->setUrl(url);
      });
  d_->watcher->setFuture(
      QtConcurrent::run(&d_->pool, [courseRoot, filePath, css] {
        return prepareDocument(courseRoot, filePath, css);
      }));
}

void CourseDocumentView::scrollBy(int pixels) {
  if (d_->view != nullptr)
    d_->view->page()->runJavaScript(
        QStringLiteral("window.scrollBy(0, %1)").arg(pixels));
}

void CourseDocumentView::jumpTo(bool last) {
  if (d_->view != nullptr) {
    d_->view->page()->runJavaScript(
        last ? QStringLiteral(
                   "window.scrollTo(0, document.documentElement.scrollHeight)")
             : QStringLiteral("window.scrollTo(0, 0)"));
  }
}

void CourseDocumentView::focusReader() {
  if (d_->view != nullptr)
    d_->view->setFocus(Qt::OtherFocusReason);
}

} // namespace melearner
