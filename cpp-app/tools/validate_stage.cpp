#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>

#include <iostream>
#include <cstdlib>
#include <filesystem>

namespace {

[[noreturn]] void fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    std::exit(1);
}

QString path(const QString& root, const QString& relative) {
    return QDir(root).filePath(relative);
}

QFileInfo requireFile(const QString& root, const QString& relative, const QString& label) {
    const QFileInfo info(path(root, relative));
    if (info.isSymLink() || !info.isFile() || info.size() < 1) {
        fail(QStringLiteral("required staged file is missing or empty: %1 (%2)").arg(label, info.filePath()));
    }
    return info;
}

bool beneath(const QString& root, const QString& candidate) {
    const auto cleanRoot = QDir::cleanPath(root);
    const auto cleanCandidate = QDir::cleanPath(candidate);
    return cleanCandidate == cleanRoot || cleanCandidate.startsWith(cleanRoot + QDir::separator());
}

bool sha256Text(const QString& value) {
    if (value.size() != 64) return false;
    for (const auto character : value) {
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9'))
              || (character >= QLatin1Char('a') && character <= QLatin1Char('f')))) return false;
    }
    return true;
}

QSet<QString> validateInventory(const QString& stage, const QString& usrRoot) {
    const auto inventoryInfo = requireFile(stage, QStringLiteral("usr/share/doc/melearner/runtime-binaries.json"), QStringLiteral("runtime binary inventory"));
    QFile inventoryFile(inventoryInfo.filePath());
    if (inventoryInfo.size() > 32 * 1024 * 1024 || !inventoryFile.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("cannot read runtime binary inventory"));
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(inventoryFile.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("runtime binary inventory must be a JSON object"));
    const auto inventory = document.object();
    if (inventory.value(QStringLiteral("schemaVersion")).toDouble(-1) != 1
        || inventory.value(QStringLiteral("version")).toString() != QStringLiteral("0.1.9")) {
        fail(QStringLiteral("runtime binary inventory schema/version mismatch"));
    }
    const auto files = inventory.value(QStringLiteral("files"));
    if (!files.isArray() || files.toArray().isEmpty()) fail(QStringLiteral("runtime binary inventory files must be a nonempty array"));
    const QStringList sourceKinds{QStringLiteral("application-build"), QStringLiteral("mpv-build"), QStringLiteral("library-provider"), QStringLiteral("qt-plugin-provider"), QStringLiteral("webengine-helper")};
    QSet<QString> paths;
    for (const auto& value : files.toArray()) {
        if (!value.isObject()) fail(QStringLiteral("runtime binary inventory entry must be an object"));
        const auto entry = value.toObject();
        const auto relative = entry.value(QStringLiteral("path")).toString();
        if (!relative.startsWith(QStringLiteral("usr/")) || relative.contains(QLatin1Char('\\'))
            || relative.contains(QChar::Null) || QDir::cleanPath(relative) != relative
            || paths.contains(relative)) fail(QStringLiteral("unsafe or duplicate runtime inventory path: %1").arg(relative));
        const auto info = requireFile(stage, relative, QStringLiteral("inventoried binary"));
        if (!beneath(usrRoot, info.canonicalFilePath())
            || info.canonicalFilePath() != QDir::cleanPath(info.absoluteFilePath())) {
            fail(QStringLiteral("runtime inventory path is not a canonical file within usr: %1").arg(relative));
        }
        const auto sourceName = entry.value(QStringLiteral("sourceName")).toString();
        if (sourceName.isEmpty() || sourceName == QStringLiteral(".") || sourceName == QStringLiteral("..")
            || sourceName.contains(QLatin1Char('/')) || sourceName.contains(QLatin1Char('\\')) || sourceName.contains(QChar::Null)
            || !sourceKinds.contains(entry.value(QStringLiteral("sourceKind")).toString())
            || !sha256Text(entry.value(QStringLiteral("sourceSha256")).toString())
            || !sha256Text(entry.value(QStringLiteral("sha256")).toString())) {
            fail(QStringLiteral("invalid runtime inventory input identity: %1").arg(relative));
        }
        const auto size = entry.value(QStringLiteral("size"));
        if (!size.isDouble() || size.toDouble(-1) != static_cast<double>(info.size())) {
            fail(QStringLiteral("runtime inventory size mismatch: %1").arg(relative));
        }
        QFile binaryFile(info.filePath());
        if (!binaryFile.open(QIODevice::ReadOnly) || binaryFile.read(4) != QByteArray("\x7f" "ELF", 4)
            || !binaryFile.seek(0)) fail(QStringLiteral("runtime inventory file is not a readable ELF: %1").arg(relative));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&binaryFile) || QString::fromLatin1(hash.result().toHex()) != entry.value(QStringLiteral("sha256")).toString()) {
            fail(QStringLiteral("runtime inventory hash mismatch: %1").arg(relative));
        }
        paths.insert(relative);
    }
    return paths;
}

void validate(const QString& stage, bool appImage) {
    const auto usr = path(stage, QStringLiteral("usr"));
    const QFileInfo usrInfo(usr);
    if (!usrInfo.isDir() || usrInfo.isSymLink()) {
        fail(QStringLiteral("C++ stage must contain a real usr directory: %1").arg(usr));
    }
    const auto usrRoot = usrInfo.canonicalFilePath();
    const auto binary = requireFile(stage, QStringLiteral("usr/bin/melearner"), QStringLiteral("melearner executable"));
    if (!binary.isExecutable()) {
        fail(QStringLiteral("staged melearner is not executable: %1").arg(binary.filePath()));
    }
    const auto webengineHelper = requireFile(stage, QStringLiteral("usr/libexec/QtWebEngineProcess"), QStringLiteral("QtWebEngineProcess helper"));
    requireFile(stage, QStringLiteral("usr/share/doc/melearner/qtwebengine/LICENSE.chromium"), QStringLiteral("Qt WebEngine provider notice"));
    if (!webengineHelper.isExecutable()) fail(QStringLiteral("staged QtWebEngineProcess is not executable: %1").arg(webengineHelper.filePath()));
    for (const auto& resource : {QStringLiteral("qtwebengine_resources.pak"), QStringLiteral("qtwebengine_resources_100p.pak"), QStringLiteral("qtwebengine_resources_200p.pak"), QStringLiteral("v8_context_snapshot.bin")}) {
        requireFile(stage, QStringLiteral("usr/share/qt6/resources/") + resource, QStringLiteral("Qt WebEngine resource"));
    }
    const auto localeDir = path(stage, QStringLiteral("usr/share/qt6/translations/qtwebengine_locales"));
    const auto locales = QDir(localeDir).entryInfoList({QStringLiteral("*.pak")}, QDir::Files | QDir::NoSymLinks);
    if (locales.isEmpty()) fail(QStringLiteral("staged Qt WebEngine locales are missing or empty: %1").arg(localeDir));
    const auto binPath = QFileInfo(binary.filePath()).absolutePath();
    const auto binEntries = QDir(binPath).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto& candidate : binEntries) {
        if (candidate.fileName() != QStringLiteral("melearner") && candidate.isFile()
            && candidate.isExecutable()) {
            fail(QStringLiteral("unexpected staged executable: %1").arg(candidate.filePath()));
        }
    }

    const auto desktopPath = appImage
        ? QStringLiteral("usr/share/applications/io.github.whitehades.melearner.appimage.desktop")
        : QStringLiteral("usr/share/applications/io.github.whitehades.melearner.desktop");
    const auto desktop = requireFile(stage, desktopPath, QStringLiteral("desktop launcher"));
    QFile desktopFile(desktop.filePath());
    if (!desktopFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fail(QStringLiteral("cannot read staged desktop launcher: %1").arg(desktop.filePath()));
    }
    const auto desktopText = QString::fromUtf8(desktopFile.readAll());
    QStringList execLines;
    for (const auto& line : desktopText.split(QLatin1Char('\n'))) {
        const auto normalized = line.endsWith(QLatin1Char('\r')) ? line.left(line.size() - 1) : line;
        if (normalized.startsWith(QStringLiteral("Exec="))) execLines.append(normalized);
    }
    const auto expectedExec = appImage ? QStringLiteral("Exec=melearner") : QStringLiteral("Exec=/usr/bin/melearner");
    if (execLines != QStringList{expectedExec}) {
        fail(QStringLiteral("desktop launcher must contain exactly %1").arg(expectedExec));
    }
    const auto desktopLower = desktopText.toLower();
    const QStringList oldRuntimeTokens{QStringLiteral("tauri"), QStringLiteral("native-app"), QStringLiteral("node"), QStringLiteral("zig"), QStringLiteral("rust"), QStringLiteral("webview"), QStringLiteral("qml"), QStringLiteral("electron"), QStringLiteral("chromium")};
    for (const auto& token : oldRuntimeTokens) {
        if (desktopLower.contains(token)) fail(QStringLiteral("desktop launcher references an old or browser runtime"));
    }

    const auto metadataInfo = requireFile(stage, QStringLiteral("usr/share/doc/melearner/runtime-stage.json"), QStringLiteral("runtime stage metadata"));
    QFile metadataFile(metadataInfo.filePath());
    if (!metadataFile.open(QIODevice::ReadOnly)) fail(QStringLiteral("runtime-stage.json is not valid JSON: %1").arg(metadataFile.errorString()));
    QJsonParseError metadataError;
    const auto metadataDoc = QJsonDocument::fromJson(metadataFile.readAll(), &metadataError);
    if (metadataError.error != QJsonParseError::NoError) fail(QStringLiteral("runtime-stage.json is not valid JSON: %1").arg(metadataError.errorString()));
    if (!metadataDoc.isObject()) fail(QStringLiteral("runtime-stage.json must be a JSON object"));
    const auto metadata = metadataDoc.object();
    if (metadata.value(QStringLiteral("schemaVersion")).toDouble(-1) != 1 || !metadata.value(QStringLiteral("schemaVersion")).isDouble()) fail(QStringLiteral("runtime-stage.json schemaVersion must be 1"));
    if (metadata.value(QStringLiteral("version")).toString() != QStringLiteral("0.1.9")) fail(QStringLiteral("runtime-stage.json version must be 0.1.9"));
    if (metadata.value(QStringLiteral("architecture")).toString() != QStringLiteral("x86_64")) fail(QStringLiteral("runtime-stage.json architecture must be x86_64"));
    if (!metadata.value(QStringLiteral("releaseQualified")).isBool() || metadata.value(QStringLiteral("releaseQualified")).toBool()) fail(QStringLiteral("runtime-stage.json must keep releaseQualified false"));
    if (metadata.value(QStringLiteral("binaryInventory")).toString() != QStringLiteral("runtime-binaries.json")) fail(QStringLiteral("runtime-stage.json must name runtime-binaries.json"));
    auto inventoriedPaths = validateInventory(stage, usrRoot);
    if (!inventoriedPaths.contains(QStringLiteral("usr/libexec/QtWebEngineProcess"))) fail(QStringLiteral("runtime inventory omits QtWebEngineProcess"));
    QJsonArray expectedWebEngineResources{
        QStringLiteral("usr/share/qt6/resources/qtwebengine_resources.pak"),
        QStringLiteral("usr/share/qt6/resources/qtwebengine_resources_100p.pak"),
        QStringLiteral("usr/share/qt6/resources/qtwebengine_resources_200p.pak"),
        QStringLiteral("usr/share/qt6/resources/v8_context_snapshot.bin")};
    const auto icuProvider = metadata.value(QStringLiteral("qtWebEngineIcuProvider")).toString();
    if (icuProvider == QStringLiteral("bundled-data")) {
        expectedWebEngineResources.insert(3, QStringLiteral("usr/share/qt6/resources/icudtl.dat"));
        requireFile(stage, QStringLiteral("usr/share/qt6/resources/icudtl.dat"), QStringLiteral("Qt WebEngine ICU data"));
    } else if (icuProvider != QStringLiteral("private-libraries")) {
        fail(QStringLiteral("runtime-stage.json has an invalid Qt WebEngine ICU provider"));
    }
    if (!metadata.value(QStringLiteral("qtWebEngineResources")).isArray()
        || metadata.value(QStringLiteral("qtWebEngineResources")).toArray() != expectedWebEngineResources) {
        fail(QStringLiteral("runtime-stage.json Qt WebEngine resource list is incorrect"));
    }
    const QStringList expectedLegal{QStringLiteral("LICENSE"), QStringLiteral("THIRD_PARTY_NOTICES"), QStringLiteral("melearner.spdx.json"), QStringLiteral("runtime-lock.json"), QStringLiteral("reference-profiles-v1.json")};
    QStringList actualLegal;
    if (metadata.value(QStringLiteral("legalInputs")).isArray()) {
        for (const auto& value : metadata.value(QStringLiteral("legalInputs")).toArray()) actualLegal.append(value.toString());
    }
    if (!metadata.value(QStringLiteral("legalInputs")).isArray() || actualLegal != expectedLegal) fail(QStringLiteral("runtime-stage.json legalInputs do not match the required legal inputs"));
    for (const auto& key : {QStringLiteral("privateLibraries"), QStringLiteral("systemRuntimeBoundary"), QStringLiteral("qtPluginGroups")}) {
        if (!metadata.value(key).isArray()) fail(QStringLiteral("runtime-stage.json %1 must be an array").arg(key));
    }

    const QList<QPair<QString, QString>> legalFiles{
        {QStringLiteral("LICENSE"), QStringLiteral("usr/share/licenses/melearner/LICENSE")},
        {QStringLiteral("THIRD_PARTY_NOTICES"), QStringLiteral("usr/share/doc/melearner/THIRD_PARTY_NOTICES")},
        {QStringLiteral("melearner.spdx.json"), QStringLiteral("usr/share/doc/melearner/melearner.spdx.json")},
        {QStringLiteral("runtime-lock.json"), QStringLiteral("usr/share/doc/melearner/runtime-lock.json")},
        {QStringLiteral("reference-profiles-v1.json"), QStringLiteral("usr/share/doc/melearner/reference-profiles-v1.json")},
    };
    for (const auto& entry : legalFiles) {
        const auto info = requireFile(stage, entry.second, entry.first);
        if (entry.first.endsWith(QStringLiteral(".json"))) {
            QFile jsonFile(info.filePath());
            if (!jsonFile.open(QIODevice::ReadOnly)) fail(QStringLiteral("staged legal JSON is invalid (%1): %2").arg(entry.first, jsonFile.errorString()));
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(jsonFile.readAll(), &error);
            if (error.error != QJsonParseError::NoError) fail(QStringLiteral("staged legal JSON is invalid (%1): %2").arg(entry.first, error.errorString()));
            if (!document.isObject()) fail(QStringLiteral("staged legal JSON must be an object: %1").arg(entry.first));
        }
    }

    const QStringList forbiddenParts{QStringLiteral("native-app"), QStringLiteral("src-tauri"), QStringLiteral("node_modules"), QStringLiteral("qml"), QStringLiteral("webview"), QStringLiteral("electron"), QStringLiteral("chromium"), QStringLiteral("zig"), QStringLiteral("rust")};
    const QStringList forbiddenFiles{QStringLiteral("node"), QStringLiteral("nodejs"), QStringLiteral("chromium"), QStringLiteral("chrome"), QStringLiteral("electron"), QStringLiteral("mpv"), QStringLiteral("ffmpeg"), QStringLiteral("ffprobe")};
    QDirIterator iterator(usr, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto entryPath = iterator.next();
        const QFileInfo info(entryPath);
        if (info.isFile() && !info.isSymLink()) {
            QFile file(entryPath);
            if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("cannot read staged file: %1").arg(entryPath));
            if (file.read(4) == QByteArray("\x7f" "ELF", 4)) {
                const auto relative = QDir(stage).relativeFilePath(entryPath);
                if (!inventoriedPaths.remove(relative)) fail(QStringLiteral("staged ELF is absent from runtime inventory: %1").arg(relative));
            }
        }
        if (info.isSymLink()) {
            std::error_code error;
            const auto link = std::filesystem::read_symlink(std::filesystem::path(entryPath.toStdString()), error);
            if (error) fail(QStringLiteral("cannot read staged symlink: %1").arg(entryPath));
            if (link.is_absolute()) fail(QStringLiteral("absolute staged symlink is not portable: %1").arg(entryPath));
            const auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(entryPath.toStdString()).parent_path() / link, error);
            if (error || !beneath(usrRoot, QString::fromStdString(resolved.string()))) fail(QStringLiteral("staged symlink escapes usr/: %1").arg(entryPath));
        }
        QStringList relativeParts = QDir(usr).relativeFilePath(entryPath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (auto& part : relativeParts) part = part.toLower();
        for (const auto& forbidden : forbiddenParts) {
            if (relativeParts.contains(forbidden)) fail(QStringLiteral("old or browser runtime asset is staged: %1").arg(entryPath));
        }
        if (info.isFile() && forbiddenFiles.contains(info.fileName().toLower())) fail(QStringLiteral("old or browser runtime executable is staged: %1").arg(entryPath));
        if (relativeParts.size() >= 3 && relativeParts.mid(0, 3) == QStringList{QStringLiteral("share"), QStringLiteral("melearner"), QStringLiteral("resources")} && info.isFile()) {
            const auto suffix = info.suffix().toLower();
            if (QStringList{QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("html"), QStringLiteral("htm")}.contains(suffix)) fail(QStringLiteral("browser application asset is staged: %1").arg(entryPath));
        }
    }
    if (!inventoriedPaths.isEmpty()) fail(QStringLiteral("runtime inventory contains unvisited binaries"));

    const auto privateDir = path(stage, QStringLiteral("usr/lib/melearner"));
    const QFileInfo privateInfo(privateDir);
    if (!privateInfo.isDir() || privateInfo.isSymLink()) fail(QStringLiteral("private runtime directory is missing: %1").arg(privateDir));
    const auto privateLibraries = metadata.value(QStringLiteral("privateLibraries")).toArray();
    for (const auto& value : privateLibraries) {
        const auto name = value.toString();
        if (!value.isString() || name.contains(QLatin1Char('/')) || !QFileInfo::exists(path(privateDir, name))) fail(QStringLiteral("declared private runtime library is missing: %1").arg(value.toVariant().toString()));
    }
    const auto runtimeEntries = QDir(privateDir).entryInfoList(QDir::Files | QDir::System | QDir::Hidden);
    bool hasMpv = false;
    bool hasQtPdf = false;
    bool hasQtWebEngine = false;
    bool hasIcuUc = false;
    bool hasIcuI18n = false;
    bool hasIcuData = false;
    for (const auto& entry : runtimeEntries) {
        hasMpv |= entry.fileName().startsWith(QStringLiteral("libmpv.so"));
        hasQtPdf |= entry.fileName().startsWith(QStringLiteral("libQt6Pdf.so"));
        hasQtWebEngine |= entry.fileName().startsWith(QStringLiteral("libQt6WebEngineCore.so"));
        hasIcuUc |= entry.fileName().startsWith(QStringLiteral("libicuuc.so"));
        hasIcuI18n |= entry.fileName().startsWith(QStringLiteral("libicui18n.so"));
        hasIcuData |= entry.fileName().startsWith(QStringLiteral("libicudata.so"));
    }
    if (!hasMpv) fail(QStringLiteral("private runtime closure does not contain libmpv"));
    if (!hasQtPdf) fail(QStringLiteral("private runtime closure does not contain Qt6Pdf"));
    if (!hasQtWebEngine) fail(QStringLiteral("private runtime closure does not contain Qt6WebEngineCore"));
    if (icuProvider == QStringLiteral("private-libraries") && !(hasIcuUc && hasIcuI18n && hasIcuData)) fail(QStringLiteral("private runtime closure does not contain all required ICU libraries"));
}

} // namespace

int main(int argc, char** argv) {
    const bool appImage = argc == 3 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("--appimage");
    if (argc != 2 && !appImage) {
        std::cerr << "usage: validate_stage <stage-directory> [--appimage]\n";
        return 2;
    }
    const auto stageInfo = QFileInfo(QString::fromLocal8Bit(argv[1]));
    if (!stageInfo.isDir()) fail(QStringLiteral("stage path is not a directory: %1").arg(stageInfo.filePath()));
    validate(stageInfo.absoluteFilePath(), appImage);
    return 0;
}
