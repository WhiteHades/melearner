#include "single_instance.hpp"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class SingleInstanceTest final : public QObject {
  Q_OBJECT
private slots:
  void oneOwnerAndActivation() {
    QTemporaryDir data;
    QVERIFY(data.isValid());
    SingleInstance first(data.path());
    QCOMPARE(first.acquire(), SingleInstance::Result::Owner);
    QSignalSpy activated(&first, &SingleInstance::activationRequested);
    SingleInstance second(data.path());
    QCOMPARE(second.acquire(), SingleInstance::Result::Forwarded);
    QTRY_COMPARE(activated.size(), 1);
  }
  void ownershipReleased() {
    QTemporaryDir data;
    { SingleInstance first(data.path());
      QCOMPARE(first.acquire(), SingleInstance::Result::Owner); }
    SingleInstance next(data.path());
    QCOMPARE(next.acquire(), SingleInstance::Result::Owner);
  }
};
QTEST_GUILESS_MAIN(SingleInstanceTest)
#include "single_instance_test.moc"
