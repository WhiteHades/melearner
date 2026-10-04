#include "update_checker.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QtGlobal>
#include <QSet>

namespace {

constexpr qsizetype maxResponseBytes = 256 * 1024;
const QUrl officialEndpoint(QStringLiteral("https://api.github.com/repos/WhiteHades/melearner/releases/latest"));

struct Version {
  qint64 major;
  qint64 minor;
  qint64 patch;
  QString text;
};

bool parseVersion(QString value, Version* result) {
  if (value.startsWith(QLatin1Char('v'))) value.remove(0, 1);
  static const QRegularExpression pattern(QStringLiteral(
    R"(^((?:0|[1-9][0-9]*))\.((?:0|[1-9][0-9]*))\.((?:0|[1-9][0-9]*))$)"));
  const auto match = pattern.match(value);
  if (!match.hasMatch()) return false;
  bool majorOk = false, minorOk = false, patchOk = false;
  const auto major = match.captured(1).toLongLong(&majorOk);
  const auto minor = match.captured(2).toLongLong(&minorOk);
  const auto patch = match.captured(3).toLongLong(&patchOk);
  if (!majorOk || !minorOk || !patchOk) return false;
  *result = {major, minor, patch, value};
  return true;
}

bool newerThan(const Version& candidate, const Version& current) {
  if (candidate.major != current.major) return candidate.major > current.major;
  if (candidate.minor != current.minor) return candidate.minor > current.minor;
  return candidate.patch > current.patch;
}

bool isLoopbackEndpoint(const QUrl& url) {
  if (url.scheme() != QStringLiteral("http") || url.port(-1) < 1 ||
      !url.userInfo().isEmpty() || !url.query().isEmpty() || !url.fragment().isEmpty()) return false;
  const auto host = url.host().toLower();
  if (host == QStringLiteral("localhost")) return true;
  QHostAddress address;
  return address.setAddress(host) && address.isLoopback();
}

bool validEndpoint(const QUrl& url) {
  if (url == officialEndpoint) return true;
  return isLoopbackEndpoint(url);
}

bool validGithubUrl(const QUrl& url, const QString& path) {
  return url.scheme() == QStringLiteral("https") && url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0 &&
    (url.port(-1) == -1 || url.port() == 443) && url.userInfo().isEmpty() &&
    url.query().isEmpty() && url.fragment().isEmpty() && url.path(QUrl::FullyDecoded) == path;
}

bool isPlatformInstaller(const QString& name) {
#if defined(Q_OS_WIN)
  return name.endsWith(QStringLiteral("setup.exe"), Qt::CaseInsensitive);
#elif defined(Q_OS_MACOS)
  return name.endsWith(QStringLiteral("arm64.dmg"), Qt::CaseInsensitive);
#elif defined(Q_OS_LINUX)
  return name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive);
#else
  Q_UNUSED(name);
  return false;
#endif
}

QString platformDescription() {
#if defined(Q_OS_WIN)
  return QStringLiteral("Windows setup.exe");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("macOS arm64 .dmg");
#elif defined(Q_OS_LINUX)
  return QStringLiteral("Linux .AppImage");
#else
  return QStringLiteral("this platform's installer");
#endif
}

} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
  : UpdateChecker(officialEndpoint, parent) {}

UpdateChecker::UpdateChecker(const QUrl& endpoint, QObject* parent)
  : QObject(parent), endpoint_(endpoint), network_(new QNetworkAccessManager(this)), timeout_(new QTimer(this)) {
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this] {
    if (!reply_) return;
    auto* timedOutReply = reply_;
    reply_ = nullptr;
    timedOutReply->abort();
    timedOutReply->deleteLater();
    response_.clear();
    emit failed(tr("The update check timed out."));
  });
}

void UpdateChecker::check() {
  if (reply_) return;
  if (!validEndpoint(endpoint_)) {
    emit failed(tr("The update check endpoint is not allowed."));
    return;
  }

  response_.clear();
  QNetworkRequest request(endpoint_);
  request.setRawHeader("User-Agent", QStringLiteral("meLearner/%1").arg(QCoreApplication::applicationVersion()).toUtf8());
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  reply_ = network_->get(request);
  reply_->setReadBufferSize(maxResponseBytes + 1);
  connect(reply_, &QNetworkReply::readyRead, this, [this] {
    if (!reply_) return;
    response_ += reply_->read(maxResponseBytes + 1 - response_.size());
    if (response_.size() > maxResponseBytes) {
      auto* oversizedReply = reply_;
      reply_ = nullptr;
      timeout_->stop();
      oversizedReply->abort();
      oversizedReply->deleteLater();
      response_.clear();
      emit failed(tr("The update response is too large."));
    }
  });
  connect(reply_, &QNetworkReply::finished, this, [this, activeReply = reply_] {
    if (reply_ != activeReply) return;
    finish(activeReply);
  });
  timeout_->start(10000);
}

void UpdateChecker::finish(QNetworkReply* reply) {
  timeout_->stop();
  response_ += reply->read(maxResponseBytes + 1 - response_.size());
  reply_ = nullptr;
  reply->deleteLater();
  if (response_.size() > maxResponseBytes) {
    response_.clear();
    emit failed(tr("The update response is too large."));
    return;
  }
  if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid() ||
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    response_.clear();
    emit failed(tr("The update check could not reach GitHub."));
    return;
  }

  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(response_, &parseError);
  response_.clear();
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    emit failed(tr("GitHub returned an invalid update response."));
    return;
  }
  const auto release = document.object();
  if (!release.value(QStringLiteral("draft")).isBool() || !release.value(QStringLiteral("prerelease")).isBool() ||
      !release.value(QStringLiteral("tag_name")).isString() || !release.value(QStringLiteral("html_url")).isString() ||
      !release.value(QStringLiteral("assets")).isArray()) {
    emit failed(tr("GitHub returned an incomplete update response."));
    return;
  }
  if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()) {
    emit checked(false);
    return;
  }

  Version candidate{}, current{};
  if (!parseVersion(release.value(QStringLiteral("tag_name")).toString(), &candidate) ||
      !parseVersion(QCoreApplication::applicationVersion(), &current)) {
    emit failed(tr("The update version is invalid."));
    return;
  }
  if (!newerThan(candidate, current)) {
    emit checked(false);
    return;
  }

  const auto page = QUrl(release.value(QStringLiteral("html_url")).toString());
  const auto expectedPage = QStringLiteral("/WhiteHades/melearner/releases/tag/%1").arg(release.value(QStringLiteral("tag_name")).toString());
  if (!validGithubUrl(page, expectedPage)) {
    emit failed(tr("GitHub returned an invalid release page."));
    return;
  }

  QUrl installer;
  int matches = 0;
  QSet<QString> assetNames;
  const auto assets = release.value(QStringLiteral("assets")).toArray();
  for (const auto& value : assets) {
    if (!value.isObject()) {
      emit failed(tr("GitHub returned an invalid release asset."));
      return;
    }
    const auto asset = value.toObject();
    if (!asset.value(QStringLiteral("name")).isString() || !asset.value(QStringLiteral("browser_download_url")).isString()) {
      emit failed(tr("GitHub returned an invalid release asset."));
      return;
    }
    const auto name = asset.value(QStringLiteral("name")).toString();
    if (assetNames.contains(name)) {
      emit failed(tr("GitHub returned duplicate release assets."));
      return;
    }
    assetNames.insert(name);
    if (!isPlatformInstaller(name)) continue;
    ++matches;
    const auto url = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
    const auto expectedAsset = QStringLiteral("/WhiteHades/melearner/releases/download/%1/%2")
      .arg(release.value(QStringLiteral("tag_name")).toString(), name);
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || !validGithubUrl(url, expectedAsset)) {
      emit failed(tr("GitHub returned an invalid installer URL."));
      return;
    }
    installer = url;
  }
  if (matches == 0) {
    emit failed(tr("No %1 was published for this release.").arg(platformDescription()));
    return;
  }
  if (matches != 1) {
    emit failed(tr("GitHub published duplicate installers for this platform."));
    return;
  }
  emit updateAvailable(candidate.text, installer, page);
  emit checked(true);
}
