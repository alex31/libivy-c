#pragma once

#include <Ivy/ivy_thread.hpp>
#include <QRegularExpression>
#include <QThreadPool>
#include <QWidget>
#include <chrono>
#include <optional>
#include <string_view>
#include <unordered_map>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QTableView;
class QTableWidget;
class MessageLogModel;

class MainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    [[nodiscard]] std::expected<void, std::error_code> start(
        std::optional<std::string_view> address = std::nullopt);

signals:
    // Owned values only: no borrowed peer/view crosses into the Qt event queue.
    void incomingMessage(QString address, quint16 port, QString application,
                         qint64 receivedMs, QString kind, QString message);
    void convertedMessage(QString result);
    void peerChanged(quint64 id, QString address, quint16 port, QString application);
    void peerGone(quint64 id);
    void pingChanged(quint64 id, int delayUs, QString state);
    void monitorError(QString error);
    void closeRequested();
    void loopFinished();
    void workerFinished(QString payload, QString error, quint64 accepted);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    using Clock = std::chrono::steady_clock;
    struct PeerState {
        quint64 id;
        ivy::ApplicationInfo info;
        std::optional<Clock::time_point> ping_sent;
        Clock::time_point next_ping{};
        int delay_us = -1;
    };
    PeerState& rememberPeer(IvyClientPtr peer);
    void applicationChanged(IvyClientPtr peer, IvyApplicationEvent event);
    void receive(IvyClientPtr peer, std::string_view message, QString kind);
    void pingPeer(IvyClientPtr peer, Clock::time_point now);
    void scanPings();
    void receivePong(IvyClientPtr peer, int delayUs);
    int agentRow(quint64 id) const;
    void sendMessage();
    void sendState(bool enabled);
    void sendFromWorker();
    void finishLoop();
    void finishClose();
    void setSendingEnabled(bool enabled);
    void showSendResult(QString origin, const ivy::Bus::SendResult& result);
    void copyMessages();
    void showConvertedSource();

    QRadioButton* radio_;
    QPushButton* worker_button_;
    QLineEdit* outgoing_;
    QPushButton* send_button_;
    QTableView* received_;
    MessageLogModel* log_;
    QTableWidget* agents_;
    QLabel* count_;
    QLabel* status_;
    QPlainTextEdit* converted_result_;
    const QRegularExpression converted_pattern_;
    // Accessed only by Ivy callbacks/timers, then by destruction after joining.
    // These fields outlive Bus destruction and its final disconnection callbacks.
    std::unordered_map<IvyClientPtr, PeerState> peers_;
    quint64 next_peer_id_ = 1;
    std::optional<ivy::Bus> bus_;
    std::optional<ivy::Subscription> messages_;
    std::optional<ivy::Subscription> converted_;
    std::optional<ivy::DirectSubscription> direct_;
    std::optional<ivy::EventSubscription> pongs_;
    std::optional<ivy::TimerSubscription> ping_timer_;
    std::optional<ivy::LoopThread> loop_;
    QThreadPool workers_;
    unsigned int pending_workers_ = 0;
    quint64 sequence_ = 0;
    bool closing_ = false;
    bool loop_done_ = true;
};
