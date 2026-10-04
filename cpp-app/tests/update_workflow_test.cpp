#include "update_checker.hpp"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

// Failure cases: unavailable server, redirects, malformed/oversized responses,
// untrusted download URLs, duplicate installers, older versions and prereleases.
class UpdateWorkflowTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { QCoreApplication::setApplicationVersion("0.1.1"); }
  void checksReleaseOverHttp_data() {
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<int>("updates");
    QTest::addColumn<int>("failures");
    for (const auto& name : {"newer", "equal", "older", "draft", "prerelease", "untrusted", "duplicate", "malformed", "oversized", "redirect"})
      QTest::newRow(name) << QString(name) << (QString(name) == "newer" ? 1 : 0)
        << (QStringList{"untrusted", "duplicate", "malformed", "oversized", "redirect"}.contains(name) ? 1 : 0);
  }
  void checksReleaseOverHttp() {
    QFETCH(QString, scenario); QFETCH(int, updates); QFETCH(int, failures);
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
#if defined(Q_OS_WIN)
    const QString asset = "melearner-0.1.2-setup.exe";
#elif defined(Q_OS_MACOS)
    const QString asset = "melearner-0.1.2-macos-arm64.dmg";
#else
    const QString asset = "melearner_0.1.2_amd64.AppImage";
#endif
    const QString tag = scenario == "equal" ? "v0.1.1" : scenario == "older" ? "v0.1.0" : "v0.1.2";
    const QString page = "https://github.com/WhiteHades/melearner/releases/tag/" + tag;
    QJsonObject installer{{"name", asset}, {"browser_download_url", "https://github.com/WhiteHades/melearner/releases/download/" + tag + "/" + asset}};
    if (scenario == "untrusted") installer["browser_download_url"] = "https://example.com/installer.AppImage";
    QJsonArray assets{installer}; if (scenario == "duplicate") assets.append(installer);
    QByteArray body = QJsonDocument(QJsonObject{{"tag_name", tag}, {"html_url", page}, {"draft", scenario == "draft"},
      {"prerelease", scenario == "prerelease"}, {"assets", assets}}).toJson(QJsonDocument::Compact);
    if (scenario == "malformed") body = "not json";
    if (scenario == "oversized") body = QByteArray(300 * 1024, 'x');
    QByteArray request;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      auto* socket = server.nextPendingConnection();
      connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
        request += socket->readAll();
        if (!request.endsWith("\r\n\r\n")) return;
        const auto header = scenario == "redirect" ? QByteArray("HTTP/1.1 302 Found\r\nLocation: https://example.com/\r\n")
          : QByteArray("HTTP/1.1 200 OK\r\n");
        socket->write(header + "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
    UpdateChecker checker(QUrl(QString("http://127.0.0.1:%1/latest").arg(server.serverPort())));
    QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
    QSignalSpy checked(&checker, &UpdateChecker::checked);
    QSignalSpy failed(&checker, &UpdateChecker::failed);
    checker.check(); checker.check(); // An in-flight check must not duplicate requests.
    QTRY_VERIFY_WITH_TIMEOUT(!checked.isEmpty() || !failed.isEmpty(), 5000);
    QCOMPARE(available.size(), updates); QCOMPARE(failed.size(), failures);
    QCOMPARE(checked.size(), failures ? 0 : 1);
    QVERIFY(request.contains("User-Agent: meLearner/0.1.1"));
    if (updates) { QCOMPARE(available.first()[0].toString(), "0.1.2"); QCOMPARE(available.first()[2].toUrl(), QUrl(page)); }
  }
  void rejectsNonGithubEndpoint() {
    UpdateChecker checker(QUrl("https://example.com/latest"));
    QSignalSpy failed(&checker, &UpdateChecker::failed); checker.check(); QCOMPARE(failed.size(), 1);
  }
  void unavailableServerReportsFailure() {
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto port = server.serverPort(); server.close();
    UpdateChecker checker(QUrl(QString("http://127.0.0.1:%1/latest").arg(port)));
    QSignalSpy failed(&checker, &UpdateChecker::failed); checker.check();
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
  }
};
QTEST_GUILESS_MAIN(UpdateWorkflowTest)
#include "update_workflow_test.moc"
