#include "window.hpp"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <vector>

using namespace std::chrono_literals;
namespace {
constexpr auto ping_period = 2s;
constexpr auto ping_timeout = 3s;
constexpr std::string_view converted_regexp = R"(^QT_CONVERT (\S+) (\S+) (\S+) (\S+)$)";
QString describe(std::error_code error) { return QString::fromStdString(error.message()); }
QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
}

// Append-only owned text: the journal keeps every received application message.
class MessageLogModel final : public QAbstractTableModel {
public:
    struct Row {
        QString address;
        quint16 port;
        QString application;
        qint64 received_ms;
        QString kind;
        QString message;
    };
    explicit MessageLogModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 6; }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rowCount()) return {};
        const auto& row = rows_[index.row()];
        if (role == Qt::UserRole && index.column() == 3) return row.received_ms;
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        switch (index.column()) {
        case 0: return row.address;
        case 1: return row.port;
        case 2: return row.application;
        case 3: return QDateTime::fromMSecsSinceEpoch(row.received_ms).toString("yyyy-MM-dd HH:mm:ss.zzz");
        case 4: return row.kind;
        case 5: return row.message;
        default: return {};
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Vertical) return section + 1;
        static const QStringList names{"Adresse", "Port", "Application", "Réception", "Type", "Message"};
        return section >= 0 && section < names.size() ? QVariant(names[section]) : QVariant{};
    }
    void append(Row row) {
        const auto next = rowCount();
        beginInsertRows({}, next, next);
        rows_.push_back(std::move(row));
        endInsertRows();
    }
private:
    std::vector<Row> rows_;
};

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent), converted_pattern_(text(converted_regexp)) {
    setWindowTitle("Ivy — Moniteur Qt6");
    resize(1120, 920);
    auto* layout = new QVBoxLayout(this);
    auto* compose = new QHBoxLayout;
    outgoing_ = new QLineEdit(this);
    outgoing_->setObjectName("outgoingMessage");
    outgoing_->setPlaceholderText("Écrire un message Ivy…");
    send_button_ = new QPushButton("Send", this);
    send_button_->setObjectName("sendMessage");
    compose->addWidget(outgoing_, 1);
    compose->addWidget(send_button_);
    layout->addLayout(compose);

    auto* converted_panel = new QGroupBox("Réception typée — bind_convert", this);
    auto* converted_layout = new QVBoxLayout(converted_panel);
    auto* regexp = new QLabel(text(converted_regexp), converted_panel);
    regexp->setObjectName("convertedRegexp");
    regexp->setTextFormat(Qt::PlainText);
    regexp->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    regexp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    regexp->setWordWrap(true);
    converted_layout->addWidget(new QLabel("Regexp d'abonnement (copiable) :", converted_panel));
    auto* regexp_row = new QHBoxLayout;
    regexp_row->addWidget(regexp, 1);
    auto* source_button = new QPushButton("Voir l'abonnement", converted_panel);
    source_button->setObjectName("convertedSource");
    source_button->setToolTip("Ouvrir window.cpp à l'appel bind_convert");
    connect(source_button, &QPushButton::clicked, this, &MainWindow::showConvertedSource);
    regexp_row->addWidget(source_button);
    converted_layout->addLayout(regexp_row);
    auto* types = new QLabel("Captures attendues, dans cet ordre : long | double | std::string_view | bool", converted_panel);
    types->setObjectName("convertedTypes");
    types->setWordWrap(true);
    converted_layout->addWidget(types);
    auto* rules = new QLabel(
        "Quatre champs non vides, séparés par un espace ; le texte ne contient pas d'espace.\n"
        "Booléen : entier nul = false, non nul = true ; sinon f/F = false, t/T/v/V = true.", converted_panel);
    rules->setWordWrap(true);
    converted_layout->addWidget(rules);
    auto* examples = new QLabel(
        "À envoyer avec ivyprobe :\nQT_CONVERT -42 125.5 avion vrai\n"
        "QT_CONVERT -42 erreur avion vrai", converted_panel);
    examples->setTextFormat(Qt::PlainText);
    examples->setTextInteractionFlags(Qt::TextSelectableByMouse);
    examples->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    converted_layout->addWidget(examples);
    converted_result_ = new QPlainTextEdit(converted_panel);
    converted_result_->setObjectName("convertedResult");
    converted_result_->setReadOnly(true);
    converted_result_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    converted_result_->setPlainText("En attente d'un message QT_CONVERT.");
    converted_result_->setFixedHeight(converted_result_->fontMetrics().height() * 7 + 16);
    converted_layout->addWidget(converted_result_);
    layout->addWidget(converted_panel);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    auto* peer_panel = new QWidget(splitter);
    auto* peer_layout = new QVBoxLayout(peer_panel);
    peer_layout->setContentsMargins(0, 0, 0, 0);
    peer_layout->addWidget(new QLabel("Agents connectés — ping toutes les 2 s", peer_panel));
    agents_ = new QTableWidget(0, 5, peer_panel);
    agents_->setObjectName("agents");
    agents_->setHorizontalHeaderLabels({"Adresse", "Port", "Application", "Ping (ms)", "État"});
    agents_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    agents_->setSelectionBehavior(QAbstractItemView::SelectRows);
    agents_->horizontalHeader()->setStretchLastSection(true);
    agents_->setColumnWidth(0, 160);
    agents_->setColumnWidth(1, 80);
    agents_->setColumnWidth(2, 220);
    agents_->setColumnWidth(3, 110);
    agents_->horizontalHeaderItem(1)->setToolTip("Port TCP annoncé par l'application Ivy");
    peer_layout->addWidget(agents_);

    auto* log_panel = new QWidget(splitter);
    auto* log_layout = new QVBoxLayout(log_panel);
    log_layout->setContentsMargins(0, 0, 0, 0);
    count_ = new QLabel("Messages reçus : 0", log_panel);
    log_layout->addWidget(count_);
    received_ = new QTableView(log_panel);
    received_->setObjectName("receivedMessages");
    log_ = new MessageLogModel(this);
    received_->setModel(log_);
    received_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    received_->setSelectionBehavior(QAbstractItemView::SelectRows);
    received_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    received_->setWordWrap(false);
    received_->horizontalHeader()->setStretchLastSection(true);
    received_->setColumnWidth(0, 135);
    received_->setColumnWidth(1, 65);
    received_->setColumnWidth(2, 150);
    received_->setColumnWidth(3, 195);
    received_->setColumnWidth(4, 85);
    received_->setToolTip("Réception en heure locale. Ctrl+C copie les lignes sélectionnées.");
    log_layout->addWidget(received_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({180, 440});
    layout->addWidget(splitter, 1);

    auto* demo = new QHBoxLayout;
    radio_ = new QRadioButton("qtdemo ON / OFF", this);
    radio_->setObjectName("sendState");
    radio_->setAutoExclusive(false);
    worker_button_ = new QPushButton("Envoyer depuis un worker", this);
    worker_button_->setObjectName("sendWorker");
    demo->addWidget(radio_);
    demo->addWidget(worker_button_);
    demo->addStretch();
    layout->addLayout(demo);
    status_ = new QLabel("Bus non démarré", this);
    status_->setObjectName("status");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    setSendingEnabled(false);
    workers_.setMaxThreadCount(4);
    workers_.setExpiryTimeout(-1);

    connect(send_button_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(outgoing_, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(outgoing_, &QLineEdit::textChanged, this, [this] {
        send_button_->setEnabled(bus_.has_value() && !closing_ && !loop_done_ && !outgoing_->text().isEmpty());
    });
    connect(radio_, &QRadioButton::toggled, this, &MainWindow::sendState);
    connect(worker_button_, &QPushButton::clicked, this, &MainWindow::sendFromWorker);
    auto* copy = new QShortcut(QKeySequence::Copy, received_);
    copy->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copy, &QShortcut::activated, this, &MainWindow::copyMessages);
    connect(this, &MainWindow::incomingMessage, this,
        [this](QString address, quint16 port, QString name, qint64 stamp, QString kind, QString message) {
            Q_ASSERT(QThread::currentThread() == thread());
            if (closing_) return;
            const bool follow = received_->verticalScrollBar()->value() == received_->verticalScrollBar()->maximum();
            log_->append({std::move(address), port, std::move(name), stamp, std::move(kind), std::move(message)});
            count_->setText(QString("Messages reçus : %1").arg(log_->rowCount()));
            if (follow) received_->scrollToBottom();
        }, Qt::QueuedConnection);
    connect(this, &MainWindow::convertedMessage, this, [this](const QString& result) {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!closing_) converted_result_->setPlainText(result);
    }, Qt::QueuedConnection);
    connect(this, &MainWindow::peerChanged, this,
        [this](quint64 id, const QString& address, quint16 port, const QString& name) {
            if (closing_) return;
            int row = agentRow(id);
            if (row < 0) {
                row = agents_->rowCount();
                agents_->insertRow(row);
                for (int column = 0; column < agents_->columnCount(); ++column)
                    agents_->setItem(row, column, new QTableWidgetItem);
                agents_->item(row, 0)->setData(Qt::UserRole, id);
                agents_->item(row, 3)->setText("—");
            }
            agents_->item(row, 0)->setText(address);
            agents_->item(row, 1)->setText(QString::number(port));
            agents_->item(row, 2)->setText(name);
        }, Qt::QueuedConnection);
    connect(this, &MainWindow::peerGone, this, [this](quint64 id) {
        if (const auto row = agentRow(id); row >= 0) agents_->removeRow(row);
    }, Qt::QueuedConnection);
    connect(this, &MainWindow::pingChanged, this, [this](quint64 id, int delay, const QString& state) {
        if (const auto row = agentRow(id); row >= 0 && !closing_) {
            agents_->item(row, 3)->setText(delay < 0 ? QString("—") : QString::number(delay / 1000.0, 'f', 3));
            agents_->item(row, 4)->setText(state);
        }
    }, Qt::QueuedConnection);
    connect(this, &MainWindow::monitorError, this, [this](const QString& error) {
        if (!closing_) status_->setText(error);
    }, Qt::QueuedConnection);
    connect(this, &MainWindow::closeRequested, this, [this] { close(); }, Qt::QueuedConnection);
    connect(this, &MainWindow::loopFinished, this, &MainWindow::finishLoop, Qt::QueuedConnection);
    connect(this, &MainWindow::workerFinished, this,
        [this](const QString& payload, const QString& error, quint64 accepted) {
            --pending_workers_;
            if (!closing_) {
                status_->setText(error.isEmpty()
                    ? QString("Worker : %1 acceptation(s) locale(s)").arg(accepted)
                    : QString("Échec worker : %1").arg(error));
                qInfo().noquote() << payload << (error.isEmpty() ? "" : error);
            }
            finishClose();
        }, Qt::QueuedConnection);
}

std::expected<void, std::error_code> MainWindow::start(std::optional<std::string_view> address) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (bus_) return std::unexpected(ivy::make_error_code(IVY_ESTATE));
    auto created = ivy::Bus::create("QtDemo", "qtdemo ready",
        [this](IvyClientPtr peer, IvyApplicationEvent event) { applicationChanged(peer, event); },
        [this](IvyClientPtr, int) { emit closeRequested(); });
    if (!created) return std::unexpected(created.error());
    bus_.emplace(std::move(*created));
    auto subscription = bus_->bind_raw_unanchored([this](IvyClientPtr peer, std::span<const std::string_view> args) {
        receive(peer, args.empty() ? std::string_view{} : args[0], "Message");
    }, "(.*)");
    if (!subscription) return std::unexpected(subscription.error());
    messages_.emplace(std::move(*subscription));
    auto converted = bus_->bind_convert(
        [this](ivy::ConvertStatus state, long integer, double real, std::string_view word, bool flag) {
            // Copy borrowed views, including the diagnostic, before queuing to Qt.
            if (state != ivy::ConvertStatus::OK) {
                const QString name = state == ivy::ConvertStatus::COUNT_ERROR ? "COUNT_ERROR" : "CONVERT_ERROR";
                emit convertedMessage(name + '\n' + text(bus_->conversion_error()));
                return;
            }
            emit convertedMessage(QString("OK\nlong : %1\ndouble : %2\nstd::string_view : %3\nbool : %4")
                .arg(QString::number(static_cast<qlonglong>(integer)),
                     QString::number(real, 'g', std::numeric_limits<double>::max_digits10),
                     text(word), flag ? "true" : "false"));
        }, converted_regexp);
    if (!converted) return std::unexpected(converted.error());
    converted_.emplace(std::move(*converted));
    auto direct = bus_->bind_direct([this](IvyClientPtr peer, int id, std::string_view message) {
        receive(peer, message, QString("Direct %1").arg(id));
    });
    if (!direct) return std::unexpected(direct.error());
    direct_.emplace(std::move(*direct));
    auto pongs = bus_->bind_event([this](IvyClientPtr peer, int delay) { receivePong(peer, delay); }, ivy::pong);
    if (!pongs) return std::unexpected(pongs.error());
    pongs_.emplace(std::move(*pongs));
    auto timer = bus_->bind_event([this](auto) { scanPings(); }, ivy::every(250ms));
    if (!timer) return std::unexpected(timer.error());
    ping_timer_.emplace(std::move(*timer));

    auto started = address ? bus_->start(*address) : bus_->start();
    if (!started) return started;
    auto running = ivy::LoopThread::create(*bus_, [this] { emit loopFinished(); });
    if (!running) return std::unexpected(running.error());
    loop_.emplace(std::move(*running));
    loop_done_ = false;
    setSendingEnabled(true);
    status_->setText("Bus démarré — journal (.*) et réception typée QT_CONVERT");
    return {};
}

MainWindow::PeerState& MainWindow::rememberPeer(IvyClientPtr peer) {
    auto found = peers_.find(peer);
    if (found != peers_.end() && found->second.info.port != 0) return found->second;
    if (found == peers_.end())
        found = peers_.emplace(peer, PeerState{next_peer_id_++, {}, {}, {}, -1}).first;
    auto info = bus_->application_info(peer);
    if (info) found->second.info = std::move(*info);
    else emit monitorError(QString("Informations d'agent : %1").arg(describe(info.error())));
    const auto& state = found->second;
    emit peerChanged(state.id, text(state.info.address), state.info.port, text(state.info.name));
    return found->second;
}

void MainWindow::applicationChanged(IvyClientPtr peer, IvyApplicationEvent event) {
    // Congestion events can originate in a sending thread; they do not access this registry.
    if (event == IvyApplicationConnected) {
        rememberPeer(peer);
        pingPeer(peer, Clock::now());
    } else if (event == IvyApplicationDisconnected) {
        if (const auto found = peers_.find(peer); found != peers_.end()) {
            const auto id = found->second.id;
            peers_.erase(found);
            emit peerGone(id);
        }
    }
}

void MainWindow::receive(IvyClientPtr peer, std::string_view message, QString kind) {
    const auto received = QDateTime::currentMSecsSinceEpoch(); // Reception time, before any GUI queuing/query.
    if (kind == "Message" && (message == "QT_CONVERT" || message.starts_with("QT_CONVERT ")) &&
        !converted_pattern_.match(text(message)).hasMatch()) {
        // Ivy never dispatches a regexp callback for a nonmatching message.
        // Use the existing raw journal subscription to report malformed test frames.
        emit convertedMessage("REGEXP_ERROR\nLe message ne correspond pas à la regexp affichée.\n"
                              "Quatre champs non vides séparés par un espace sont attendus.\nReçu : " + text(message));
    }
    const auto& state = rememberPeer(peer);
    emit incomingMessage(text(state.info.address), state.info.port, text(state.info.name),
                         received, std::move(kind), text(message));
}

void MainWindow::pingPeer(IvyClientPtr peer, Clock::time_point now) {
    auto found = peers_.find(peer);
    if (found == peers_.end() || found->second.ping_sent) return;
    found->second.ping_sent = now;
    const auto result = bus_->send_ping(peer); // Always on the Ivy thread, with a live tracked peer.
    found = peers_.find(peer);
    if (found == peers_.end()) return;
    auto& state = found->second;
    if (!result) {
        state.ping_sent.reset();
        state.next_ping = now + ping_period;
        state.delay_us = -1;
        emit pingChanged(state.id, -1, QString("Erreur : %1").arg(describe(result.error())));
    } else {
        emit pingChanged(state.id, state.delay_us, "En attente");
    }
}

void MainWindow::scanPings() {
    const auto now = Clock::now();
    // Snapshot handles so a callback caused by a send cannot invalidate iteration.
    std::vector<IvyClientPtr> peers;
    peers.reserve(peers_.size());
    for (const auto& [peer, state] : peers_) { (void)state; peers.push_back(peer); }
    for (auto peer : peers) {
        auto found = peers_.find(peer);
        if (found == peers_.end()) continue;
        auto& state = found->second;
        if (state.ping_sent && now - *state.ping_sent >= ping_timeout) {
            state.ping_sent.reset();
            state.delay_us = -1;
            state.next_ping = now + ping_period;
            emit pingChanged(state.id, -1, "Sans réponse");
        } else if (!state.ping_sent && now >= state.next_ping) {
            pingPeer(peer, now);
        }
    }
}

void MainWindow::receivePong(IvyClientPtr peer, int delayUs) {
    auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.ping_sent || delayUs < 0) return;
    auto& state = found->second;
    if (Clock::now() - *state.ping_sent >= ping_timeout) return;
    state.ping_sent.reset();
    state.delay_us = delayUs;
    state.next_ping = Clock::now() + ping_period;
    emit pingChanged(state.id, delayUs, "OK");
}

int MainWindow::agentRow(quint64 id) const {
    for (int row = 0; row < agents_->rowCount(); ++row)
        if (agents_->item(row, 0)->data(Qt::UserRole).toULongLong() == id) return row;
    return -1;
}

void MainWindow::sendMessage() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!bus_ || closing_ || loop_done_ || outgoing_->text().isEmpty()) return;
    const auto bytes = outgoing_->text().toUtf8();
    const auto result = bus_->send(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    showSendResult("Send", result);
    if (result) outgoing_->clear();
}

void MainWindow::sendState(bool enabled) {
    if (!bus_ || closing_ || loop_done_) return;
    showSendResult("Interface", bus_->send("qtdemo {}", enabled ? "ON" : "OFF"));
}

void MainWindow::sendFromWorker() {
    if (!bus_ || closing_ || loop_done_) return;
    const auto sequence = ++sequence_;
    ++pending_workers_;
    workers_.start([this, sequence] {
        const auto id = reinterpret_cast<quintptr>(QThread::currentThreadId());
        const auto result = bus_->send("qtdemo worker thread {} seq {}", id, sequence);
        const auto payload = QString("qtdemo worker thread %1 seq %2").arg(id).arg(sequence);
        emit workerFinished(payload, result ? QString{} : describe(result.error()), result ? *result : 0);
    });
}

void MainWindow::showSendResult(QString origin, const ivy::Bus::SendResult& result) {
    status_->setText(result
        ? QString("%1 : %2 acceptation(s) locale(s)").arg(origin).arg(*result)
        : QString("%1 : %2").arg(origin, describe(result.error())));
}

void MainWindow::setSendingEnabled(bool enabled) {
    outgoing_->setEnabled(enabled);
    send_button_->setEnabled(enabled && !outgoing_->text().isEmpty());
    radio_->setEnabled(enabled);
    worker_button_->setEnabled(enabled);
}

void MainWindow::showConvertedSource() {
    if (auto* existing = findChild<QDialog*>("convertedSourceDialog")) {
        existing->showNormal();
        existing->raise();
        existing->activateWindow();
        return;
    }
    QFile source(QString::fromUtf8(__FILE__));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Source de l'abonnement",
            QString("Impossible d'ouvrir %1 :\n%2").arg(source.fileName(), source.errorString()));
        return;
    }
    const auto code = QString::fromUtf8(source.readAll());
    // Find the call in the current source so edits above it do not invalidate the link.
    const auto subscription = QRegularExpression(
        R"(^[ \t]*auto converted = bus_->bind_convert\()", QRegularExpression::MultilineOption).match(code);
    if (!subscription.hasMatch()) {
        QMessageBox::warning(this, "Source de l'abonnement",
            QString("L'appel bind_convert est introuvable dans %1.").arg(source.fileName()));
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setObjectName("convertedSourceDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Source de l'abonnement — bind_convert");
    dialog->resize(1000, 650);
    auto* layout = new QVBoxLayout(dialog);
    auto* view = new QPlainTextEdit(dialog);
    view->setObjectName("convertedSourceCode");
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setPlainText(code);
    auto cursor = view->textCursor();
    cursor.setPosition(static_cast<int>(subscription.capturedStart()));
    view->setTextCursor(cursor);
    QTextEdit::ExtraSelection highlight;
    highlight.cursor = cursor;
    highlight.format.setBackground(view->palette().brush(QPalette::Highlight));
    highlight.format.setForeground(view->palette().brush(QPalette::HighlightedText));
    highlight.format.setProperty(QTextFormat::FullWidthSelection, true);
    view->setExtraSelections({highlight});
    auto* location = new QLabel(QString("%1 — ligne %2").arg(source.fileName()).arg(cursor.blockNumber() + 1), dialog);
    location->setTextFormat(Qt::PlainText);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location->setWordWrap(true);
    layout->addWidget(location);
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
    view->centerCursor();
    view->setFocus();
}

void MainWindow::copyMessages() {
    auto selected = received_->selectionModel()->selectedRows();
    std::sort(selected.begin(), selected.end(), [](auto a, auto b) { return a.row() < b.row(); });
    QStringList lines;
    for (const auto& index : selected) {
        QStringList cells;
        for (int column = 0; column < log_->columnCount(); ++column)
            cells.push_back(log_->data(log_->index(index.row(), column), Qt::DisplayRole).toString());
        lines.push_back(cells.join('\t'));
    }
    if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join('\n'));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (loop_done_ && pending_workers_ == 0) { event->accept(); return; }
    event->ignore();
    if (!closing_) {
        closing_ = true;
        setSendingEnabled(false);
        status_->setText("Arrêt en cours…");
        if (auto result = loop_->request_stop(); !result) status_->setText(describe(result.error()));
    }
}

void MainWindow::finishLoop() {
    const auto result = loop_->join();
    const auto callbacks = bus_->take_callback_error();
    loop_done_ = true;
    agents_->setRowCount(0);
    setSendingEnabled(false);
    if (!result || !callbacks) {
        const auto error = !result ? result.error() : callbacks.error();
        status_->setText(describe(error));
        qWarning().noquote() << "Ivy:" << describe(error);
    } else if (!closing_) status_->setText("Bus arrêté");
    finishClose();
}

void MainWindow::finishClose() {
    if (closing_ && loop_done_ && pending_workers_ == 0) {
        workers_.waitForDone();
        close();
    }
}

MainWindow::~MainWindow() {
    if (loop_) (void)loop_->request_stop();
    workers_.waitForDone();
    if (loop_) (void)loop_->join();
}
