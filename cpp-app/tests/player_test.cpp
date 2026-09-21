#include "player.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class PlayerTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsPathsOutsideApprovedRoots();
    void reservesTerminalResultsUntilGuiDelivery();
    void exposesTracksAndChapters();
    void playsTheCheckedInH264Fixture();
};

void PlayerTest::rejectsPathsOutsideApprovedRoots() {
    QTemporaryDir approved;
    QTemporaryDir outside;
    QVERIFY(approved.isValid());
    QVERIFY(outside.isValid());

    QFile media(outside.filePath(QStringLiteral("not-approved.mp4")));
    QVERIFY(media.open(QIODevice::WriteOnly));
    QVERIFY(media.write("not a media file") > 0);
    media.close();

    melearner::Player player;
    player.setApprovedRoots({approved.path()});
    QSignalSpy initialized(&player, &melearner::Player::initialized);
    QSignalSpy failed(&player, &melearner::Player::commandFailed);
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(initialized.count() == 1, 10000);

    const auto request = player.loadFile(media.fileName());
    QVERIFY(request != 0);
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 2000);
    QCOMPARE(failed.at(0).at(0).value<quint64>(), request);
    QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("invalid_path"));
    player.shutdown();
}

void PlayerTest::reservesTerminalResultsUntilGuiDelivery() {
    melearner::Player player;
    QSignalSpy initialized(&player, &melearner::Player::initialized);
    QSignalSpy finished(&player, &melearner::Player::commandFinished);
    QSignalSpy failed(&player, &melearner::Player::commandFailed);
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(initialized.count() == 1, 10000);

    int accepted = 0;
    for (int index = 0; index < 128; ++index) {
        accepted += player.frameStep() != 0 ? 1 : 0;
    }
    QCOMPARE(accepted, 128);
    QCOMPARE(player.frameStep(), quint64(0));

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() >= 128, 5000);
    player.shutdown();
}

void PlayerTest::playsTheCheckedInH264Fixture() {
    const auto mediaPath = QDir::cleanPath(
        QDir::current().absoluteFilePath(QStringLiteral("../../fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4")));
    if (!QFileInfo::exists(mediaPath)) {
        QSKIP("The checked-in media corpus is not available in this build tree.");
    }

    melearner::Player player;
    player.setApprovedRoots({QFileInfo(mediaPath).dir().absolutePath()});
    QSignalSpy initialized(&player, &melearner::Player::initialized);
    QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
    QSignalSpy positions(&player, &melearner::Player::positionChanged);
    QSignalSpy ended(&player, &melearner::Player::playbackEnded);
    QSignalSpy commands(&player, &melearner::Player::commandFinished);
    QSignalSpy fatal(&player, &melearner::Player::fatalError);
    bool paused = false;
    bool loadedPaused = false;
    connect(&player, &melearner::Player::pausedChanged, &player, [&](bool value) { paused = value; });
    connect(&player, &melearner::Player::fileLoaded, &player, [&] { loadedPaused = paused; });
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(initialized.count() == 1, 10000);
    QVERIFY(player.setVolume(0) != 0);

    constexpr qint64 savedPositionMs = 750;
    const auto request = player.loadFile(mediaPath, savedPositionMs);
    QVERIFY(request != 0);
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1 || fatal.count() > 0, 10000);
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.at(0).at(1).toString()));
    QCOMPARE(loaded.at(0).at(0).toString(), QFileInfo(mediaPath).canonicalFilePath());
    QCOMPARE(loaded.at(0).at(3).value<quint64>(), request);
    QVERIFY(loaded.at(0).at(1).toLongLong() > 0);
    QCOMPARE(loaded.at(0).at(2).toLongLong(), savedPositionMs);
    QTRY_VERIFY(loadedPaused);

    const auto seek = player.seek(1000);
    QVERIFY(seek != 0);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        bool completed = false;
        for (const auto& row : commands) {
            if (row.size() == 1 && row.at(0).value<quint64>() == seek) {
                completed = true;
                break;
            }
        }
        if (!completed) {
            return false;
        }
        for (const auto& row : positions) {
            if (row.size() >= 1 && row.at(0).toLongLong() >= 850
                && row.at(0).toLongLong() <= 1150) {
                return true;
            }
        }
        return false;
    }(), 3000);
    positions.clear();
    QVERIFY(player.play() != 0);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : positions) {
            if (!row.isEmpty() && row.at(0).toLongLong() > 1100) {
                return true;
            }
        }
        return false;
    }(), 3000);
    QVERIFY(player.pause() != 0);
    const auto stop = player.stop();
    QVERIFY(stop != 0);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : commands) {
            if (row.size() == 1 && row.at(0).value<quint64>() == stop) {
                return true;
            }
        }
        return false;
    }(), 3000);
    QCOMPARE(ended.count(), 0);
    player.shutdown();
}

void PlayerTest::exposesTracksAndChapters() {
    const auto mediaRoot = QDir::cleanPath(
        QDir::current().absoluteFilePath(QStringLiteral("../../fixtures/parity/media")));
    const auto mediaPath = QDir(mediaRoot).filePath(QStringLiteral("02 Multi audio chapters.mkv"));
    if (!QFileInfo::exists(mediaPath)) {
        QSKIP("The checked-in multi-track media corpus is not available in this build tree.");
    }

    melearner::Player player;
    player.setApprovedRoots({mediaRoot});
    QSignalSpy initialized(&player, &melearner::Player::initialized);
    QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
    QSignalSpy tracks(&player, &melearner::Player::tracksChanged);
    QSignalSpy chapters(&player, &melearner::Player::chaptersChanged);
    QSignalSpy commands(&player, &melearner::Player::commandFinished);
    QSignalSpy fatal(&player, &melearner::Player::fatalError);
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(initialized.count() == 1, 10000);
    QVERIFY(player.setVolume(0) != 0);
    QVERIFY(player.loadFile(mediaPath) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1 || fatal.count() > 0, 10000);
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.at(0).at(1).toString()));
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : tracks) {
            if (row.size() == 1 && row.at(0).canConvert<QVector<melearner::PlayerTrack>>()
                && row.at(0).value<QVector<melearner::PlayerTrack>>().size() >= 2) {
                return true;
            }
        }
        return false;
    }(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : chapters) {
            if (row.size() == 1 && row.at(0).canConvert<QVector<melearner::PlayerChapter>>()
                && !row.at(0).value<QVector<melearner::PlayerChapter>>().isEmpty()) {
                return true;
            }
        }
        return false;
    }(), 3000);
    const auto subtitleOff = player.selectSubtitleTrack(-1);
    QVERIFY(subtitleOff != 0);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : commands) {
            if (row.size() == 1 && row.at(0).value<quint64>() == subtitleOff) {
                return true;
            }
        }
        return false;
    }(), 3000);
    const auto chapter = player.selectChapter(0);
    QVERIFY(chapter != 0);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto& row : commands) {
            if (row.size() == 1 && row.at(0).value<quint64>() == chapter) {
                return true;
            }
        }
        return false;
    }(), 3000);
    player.shutdown();
}

}  // namespace

QTEST_MAIN(PlayerTest)
#include "player_test.moc"
