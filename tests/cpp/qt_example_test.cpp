#include "window.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace std::chrono_literals;

static int messageRow(QTableView* view, const QString& message) {
    for (int row = 0; row < view->model()->rowCount(); ++row)
        if (view->model()->index(row, 5).data().toString() == message) return row;
    return -1;
}
static bool allPongs(QTableWidget* agents, int count) {
    if (agents->rowCount() != count) return false;
    for (int row = 0; row < count; ++row) {
        bool numeric = false;
        const auto delay = agents->item(row, 3)->text().toDouble(&numeric);
        if (!numeric || delay < 0 || agents->item(row, 4)->text() != "OK") return false;
    }
    return true;
}

class QtExampleTest : public QObject {
    Q_OBJECT
    std::string address_;
private slots:
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
        address_ = qEnvironmentVariable("IVY_QT_TEST_BUS", "127.255.255.255:32410").toStdString();
    }
    void startupError() {
        MainWindow window;
        const auto result = window.start(std::string_view("127\0invalid", 11));
        QVERIFY(!result);
        QCOMPARE(result.error(), ivy::make_error_code(IVY_EINVAL));
    }
    void trafficPingsAndClose() {
        auto peer = ivy::Bus::create("qt-test-peer", "AGENT_READY");
        QVERIFY(peer);
        std::atomic<int> state_messages = 0, worker_messages = 0, typed_messages = 0, answers = 0;
        auto states = peer->bind([&](IvyClientPtr, auto) { ++state_messages; }, "^qtdemo (ON|OFF)$");
        auto workers = peer->bind([&](IvyClientPtr, auto) { ++worker_messages; },
                                  "^qtdemo worker thread ([0-9]+) seq ([0-9]+)$");
        auto typed = peer->bind_unanchored([&](IvyClientPtr, auto args) {
            if (!args.empty() && (args[0] == "HELLO été 🌍 100%" || args[0] == "  message avec espaces 100%  "))
                ++typed_messages;
        }, "(.*)");
        auto pongs = peer->bind([&](IvyClientPtr, int delay) { if (delay >= 0) ++answers; }, ivy::pong);
        bool ping_sent = false;
        auto ping = peer->bind([&](auto) {
            if (ping_sent) return;
            auto target = peer->find_application("QtDemo");
            if (target && target->has_value()) ping_sent = peer->send_ping(**target).has_value();
        }, ivy::every(50ms));
        QVERIFY(states && workers && typed && pongs && ping && peer->start(address_));
        auto peer_loop = ivy::LoopThread::create(*peer);
        QVERIFY(peer_loop);

        std::unordered_map<quint64, int> measured;
        QObject observer; // Drops queued observations before the local map is destroyed.
        auto window = std::make_unique<MainWindow>();
        connect(window.get(), &MainWindow::pingChanged, &observer,
                [&](quint64 id, int delay, const QString& status) {
                    if (delay >= 0 && status == "OK") ++measured[id];
                }, Qt::QueuedConnection);
        QVERIFY(window->start(address_));
        window->show();
        auto* received = window->findChild<QTableView*>("receivedMessages");
        auto* agents = window->findChild<QTableWidget*>("agents");
        auto* outgoing = window->findChild<QLineEdit*>("outgoingMessage");
        auto* send = window->findChild<QPushButton*>("sendMessage");
        auto* state = window->findChild<QRadioButton*>("sendState");
        auto* worker = window->findChild<QPushButton*>("sendWorker");
        QVERIFY(received && agents && outgoing && send && state && worker);
        QTRY_VERIFY_WITH_TIMEOUT(messageRow(received, "AGENT_READY") >= 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(allPongs(agents, 1), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(answers.load() >= 1, 3000); // QtDemo answers incoming protocol pings.
        QCOMPARE(received->model()->headerData(0, Qt::Horizontal).toString(), QString("Adresse"));
        QCOMPARE(received->model()->headerData(1, Qt::Horizontal).toString(), QString("Port"));
        QCOMPARE(received->model()->headerData(2, Qt::Horizontal).toString(), QString("Application"));
        QCOMPARE(received->model()->headerData(3, Qt::Horizontal).toString(), QString("Réception"));
        const auto first_port = agents->item(0, 1)->text().toUInt();
        QVERIFY(first_port > 0);
        QVERIFY(agents->item(0, 0)->text().startsWith("127."));
        QCOMPARE(agents->item(0, 2)->text(), QString("qt-test-peer"));
        const auto first_row = messageRow(received, "AGENT_READY");
        QCOMPARE(received->model()->index(first_row, 1).data().toUInt(), first_port);
        auto target = peer->find_application("QtDemo");
        QVERIFY(target && target->has_value());
        auto regexps = peer->application_regexps(**target);
        QVERIFY(regexps && std::ranges::find(*regexps, "(.*)") != regexps->end());

        // A second, silent agent with the same name must still have its own ping row.
        auto silent = ivy::Bus::create("qt-test-peer");
        QVERIFY(silent && silent->start(address_));
        auto silent_loop = ivy::LoopThread::create(*silent);
        QVERIFY(silent_loop);
        QTRY_VERIFY_WITH_TIMEOUT(allPongs(agents, 2), 5000);
        QVERIFY(agents->item(0, 1)->text() != agents->item(1, 1)->text());
        QTRY_VERIFY_WITH_TIMEOUT(measured.size() == 2 && std::ranges::all_of(measured,
            [](const auto& entry) { return entry.second >= 2; }), 5000);

        QVERIFY(!send->isEnabled());
        outgoing->setText(QString::fromUtf8("HELLO été 🌍 100%"));
        QVERIFY(send->isEnabled());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(typed_messages.load(), 1, 3000);
        QVERIFY(outgoing->text().isEmpty() && !send->isEnabled());
        outgoing->setText("  message avec espaces 100%  ");
        QTest::keyClick(outgoing, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(typed_messages.load(), 2, 3000);

        QTest::mouseClick(state, Qt::LeftButton, Qt::NoModifier, QPoint(10, state->height() / 2));
        QTRY_COMPARE_WITH_TIMEOUT(state_messages.load(), 1, 3000);
        QTest::mouseClick(state, Qt::LeftButton, Qt::NoModifier, QPoint(10, state->height() / 2));
        QTRY_COMPARE_WITH_TIMEOUT(state_messages.load(), 2, 3000);
        for (int i = 0; i < 8; ++i) QTest::mouseClick(worker, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(worker_messages.load(), 8, 3000);

        const QString message = QString::fromUtf8("AUTRE_CLASSE — été 🌍 100%");
        const auto before = QDateTime::currentMSecsSinceEpoch();
        const QByteArray utf8 = message.toUtf8();
        QVERIFY(peer->send(std::string_view(utf8.constData(), utf8.size())));
        QTRY_VERIFY_WITH_TIMEOUT(messageRow(received, message) >= 0, 3000);
        const auto row = messageRow(received, message);
        const auto stamp = received->model()->index(row, 3).data(Qt::UserRole).toLongLong();
        QVERIFY(stamp >= before && stamp <= QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(received->model()->index(row, 0).data().toString(), agents->item(0, 0)->text());
        QCOMPARE(received->model()->index(row, 2).data().toString(), QString("qt-test-peer"));
        QCOMPARE(received->model()->index(row, 4).data().toString(), QString("Message"));
        received->selectRow(row);
        received->setFocus();
        QTest::keyClick(received, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::clipboard()->text().contains(message), 1000);
        QVERIFY(peer->send(**target, 7, "DIRECT texte complet"));
        QTRY_VERIFY_WITH_TIMEOUT(messageRow(received, "DIRECT texte complet") >= 0, 3000);
        QCOMPARE(received->model()->index(messageRow(received, "DIRECT texte complet"), 4).data().toString(),
                 QString("Direct 7"));
        for (int i = 0; i < 20; ++i) QVERIFY(peer->send("MESSAGE {}", i));
        QTRY_VERIFY_WITH_TIMEOUT(messageRow(received, "MESSAGE 19") >= 0, 3000);
        for (int i = 0; i < 20; ++i) QVERIFY(messageRow(received, QString("MESSAGE %1").arg(i)) >= 0);
        if (const auto screenshot = qEnvironmentVariable("IVY_QT_SCREENSHOT"); !screenshot.isEmpty())
            QVERIFY(window->grab().save(screenshot));

        QVERIFY(silent_loop->request_stop() && silent_loop->join());
        silent_loop = std::unexpected(std::error_code{});
        silent = std::unexpected(std::error_code{}); // Disconnect, rather than just stop dispatch.
        QTRY_COMPARE_WITH_TIMEOUT(agents->rowCount(), 1, 3000);
        QVERIFY(messageRow(received, message) >= 0); // History survives peer disconnection.
        for (int i = 0; i < 16; ++i) QTest::mouseClick(worker, Qt::LeftButton);
        QVERIFY(peer->send("queued-before-close"));
        window->close();
        QVERIFY(window->isVisible());
        QVERIFY(!worker->isEnabled() && !send->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 3000);
        window.reset();
        QCoreApplication::processEvents();
        QVERIFY(peer_loop->request_stop() && peer_loop->join());
        QVERIFY(peer->take_callback_error());
    }
    void pingTimeout() {
        struct Block {
            std::mutex mutex;
            std::condition_variable changed;
            bool release = false;
            std::atomic<bool> requested = false;
        } block;
        auto peer = ivy::Bus::create("slow-peer");
        QVERIFY(peer);
        auto blocker = peer->bind([&](auto) {
            if (!block.requested) return;
            std::unique_lock lock(block.mutex);
            block.changed.wait_for(lock, 10s, [&] { return block.release; });
        }, ivy::every(50ms));
        QVERIFY(blocker && peer->start(address_));
        auto peer_loop = ivy::LoopThread::create(*peer);
        QVERIFY(peer_loop);
        struct Release {
            Block& block;
            ~Release() { std::lock_guard lock(block.mutex); block.release = true; block.changed.notify_all(); }
        } release{block};
        MainWindow window;
        QVERIFY(window.start(address_)); window.show();
        auto* agents = window.findChild<QTableWidget*>("agents");
        QTRY_VERIFY_WITH_TIMEOUT(allPongs(agents, 1), 3000);
        int gui_ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &window, [&] { ++gui_ticks; });
        heartbeat.start(10);
        block.requested = true;
        QTRY_COMPARE_WITH_TIMEOUT(agents->item(0, 4)->text(), QString("Sans réponse"), 7000);
        QCOMPARE(agents->item(0, 3)->text(), QString("—"));
        QVERIFY(gui_ticks > 10);
        {
            std::lock_guard lock(block.mutex); block.release = true;
            block.requested = false; block.changed.notify_all();
        }
        QTRY_VERIFY_WITH_TIMEOUT(allPongs(agents, 1), 5000);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QVERIFY(peer_loop->request_stop() && peer_loop->join());
        QVERIFY(peer->take_callback_error());
    }
    void remoteDieAndDirectDeletion() {
        auto peer = ivy::Bus::create("qt-die-peer", "connected");
        QVERIFY(peer && peer->start(address_));
        auto peer_loop = ivy::LoopThread::create(*peer);
        QVERIFY(peer_loop);
        {
            MainWindow window;
            QVERIFY(window.start(address_)); window.show();
            auto* received = window.findChild<QTableView*>("receivedMessages");
            QTRY_VERIFY_WITH_TIMEOUT(messageRow(received, "connected") >= 0, 5000);
            auto target = peer->find_application("QtDemo");
            QVERIFY(target && target->has_value());
            QVERIFY(peer->send_die(**target));
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        }
        {
            auto window = std::make_unique<MainWindow>();
            QVERIFY(window->start(address_)); window->show();
            auto* worker = window->findChild<QPushButton*>("sendWorker");
            for (int i = 0; i < 8; ++i) QTest::mouseClick(worker, Qt::LeftButton);
            window.reset();
            QCoreApplication::processEvents();
        }
        QVERIFY(peer_loop->request_stop() && peer_loop->join());
        QVERIFY(peer->take_callback_error());
    }
};
QTEST_MAIN(QtExampleTest)
#include "qt_example_test.moc"
