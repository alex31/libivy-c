/*
 * Minimal Qt demo for Ivy MTSafe API.
 * - Radio button sends "qtdemo ON" / "qtdemo OFF".
 * - Subscribes to "qtdemo msg (.*)" and displays xxx in a QLineEdit.
 */

#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>

#include <Ivy/ivy.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>
#include <vector>

class IvyBridge : public QObject
{
  Q_OBJECT

 public:
  explicit IvyBridge(QString bus, QObject* parent = nullptr)
      : QObject(parent), bus_(std::move(bus)) {}

  ~IvyBridge() override
  {
    stop();
  }

  bool start()
  {
    if (started_.load()) {
      return true;
    }

    ctx_ = IvyContextCreate("QtDemo",
                            "qtdemo ready",
                            nullptr,
                            nullptr,
                            onDie,
                            this);
    if (!ctx_) {
      qWarning() << "IvyContextCreate failed:" << IvyGetLastError();
      return false;
    }

    binding_ = IvyContextBindMsg(ctx_, onIncomingMessage, this, "^qtdemo msg (.*)");
    if (!binding_) {
      qWarning() << "IvyContextBindMsg failed:" << IvyGetLastError();
      IvyContextDestroy(ctx_);
      ctx_ = nullptr;
      return false;
    }

    QByteArray bus = bus_.toUtf8();
    int started = IvyContextStart(ctx_, bus.isEmpty() ? nullptr : bus.constData());
    if (started != IVY_OK) {
      qWarning() << "IvyContextStart failed:" << started << IvyGetLastError();
      IvyContextUnbindMsg(ctx_, binding_);
      IvyContextDestroy(ctx_);
      ctx_ = nullptr;
      binding_ = nullptr;
      return false;
    }

    loop_ = std::thread(&IvyBridge::loop, this);
    started_.store(true);
    return true;
  }

  void stop()
  {
    if (!ctx_ || !started_.load()) {
      return;
    }
    IvyContextStop(ctx_);
    if (loop_.joinable()) {
      loop_.join();
    }
    if (binding_) {
      IvyContextUnbindMsg(ctx_, binding_);
      binding_ = nullptr;
    }
    if (ctx_) {
      IvyContextDestroy(ctx_);
      ctx_ = nullptr;
    }
    started_.store(false);
  }

  bool sendState(bool on)
  {
    if (!ctx_) {
      return false;
    }
    int sent = IvyContextSendMsg(ctx_, "qtdemo %s", on ? "ON" : "OFF");
    return sent >= 0;
  }

  bool sendRaw(const QString& payload)
  {
    if (!ctx_) {
      return false;
    }
    const QByteArray msg = payload.toUtf8();
    int sent = IvyContextSendMsg(ctx_, "%s", msg.constData());
    return sent >= 0;
  }

  bool isStarted() const { return started_.load(); }

  signals:
  void messageReceived(const QString& message);
  void closeRequested();

 private:
  static void onIncomingMessage(IvyClientPtr, void* userData, int argc, char** argv)
  {
    if (!userData || argc < 1 || !argv || !argv[0]) {
      return;
    }
    auto* self = static_cast<IvyBridge*>(userData);
    QString msg = QString::fromUtf8(argv[0]);
    QMetaObject::invokeMethod(self,
                              [self, msg]() { emit self->messageReceived(msg); },
                              Qt::QueuedConnection);
  }

  static void onDie(IvyClientPtr, void* userData, int)
  {
    if (!userData) {
      return;
    }
    auto* self = static_cast<IvyBridge*>(userData);
    QMetaObject::invokeMethod(self,
                              [self]() { emit self->closeRequested(); },
                              Qt::QueuedConnection);
  }

  void loop()
  {
    IvyContextMainLoop(ctx_);
  }

  IvyContext* ctx_{nullptr};
  QString bus_;
  MsgRcvPtr binding_{nullptr};
  std::thread loop_;
  std::atomic<bool> started_{false};
};

class MainWindow : public QWidget
{
 public:
  MainWindow(QString bus, QWidget* parent = nullptr)
      : QWidget(parent), bus_(std::move(bus)), ivy_(bus_, this)
  {
    setWindowTitle("Ivy Qt Demo");
    resize(420, 130);

    auto* layout = new QVBoxLayout(this);

    radio_ = new QRadioButton("qtdemo ON/OFF", this);
    radio_->setChecked(false);
    radio_->setAutoExclusive(false);
    auto* textLine = new QLineEdit(this);
    textLine->setReadOnly(true);
    textLine->setPlaceholderText("Attente d’un message qtdemo msg xxx");
    auto* label = new QLabel("Dernier xxx :", this);
    auto* sendThreadButton = new QPushButton("Envoyer depuis un thread", this);
    auto* joinAllButton = new QPushButton("Join all threads", this);

    layout->addWidget(new QLabel("Envoyer état Ivy :", this));
    layout->addWidget(radio_);
    layout->addSpacing(12);
    layout->addWidget(label);
    layout->addWidget(textLine);
    layout->addSpacing(12);
    layout->addWidget(sendThreadButton);
    layout->addWidget(joinAllButton);
    setLayout(layout);

    connect(&ivy_, &IvyBridge::messageReceived, this,
            [textLine](const QString& message) { textLine->setText(message); });
    connect(&ivy_, &IvyBridge::closeRequested, this, [this]() {
      ivy_.stop();
      close();
    }, Qt::QueuedConnection);
    connect(sendThreadButton, &QPushButton::clicked, this, [this]() {
      if (!ivy_.isStarted()) {
        qWarning() << "Ivy non démarré";
        return;
      }
      if (stopAllSenders_.load(std::memory_order_acquire)) {
        qWarning() << "Join-all en cours, envoi ignoré";
        return;
      }
      std::thread sender([this]() {
        std::ostringstream thread_id;
        thread_id << std::this_thread::get_id();
        const QString payload =
            QStringLiteral("qtdemo from another thread %1").arg(QString::fromStdString(thread_id.str()));
        const bool ok = ivy_.sendRaw(payload);
        qInfo() << (ok ? "Thread message sent: " : "Thread message failed: ") << payload;

        std::unique_lock<std::mutex> lock(senderThreadsMutex_);
        senderStopCv_.wait(lock, [this]() {
          return stopAllSenders_.load(std::memory_order_acquire);
        });
      });
      {
        std::lock_guard<std::mutex> lock(senderThreadsMutex_);
        senderThreads_.emplace_back(std::move(sender));
      }
    });
    connect(joinAllButton, &QPushButton::clicked, this, [this]() {
      stopAndJoinAllSenderThreads();
    });
    connect(radio_, &QRadioButton::toggled, this, [this](bool checked) {
      if (ivy_.isStarted()) {
        ivy_.sendState(checked);
      }
    });

    if (!ivy_.start()) {
      QMessageBox::critical(this,
                            tr("Ivy"),
                            tr("Impossible de démarrer Ivy sur le bus: %1")
                                .arg(bus_.isEmpty() ? QStringLiteral("défaut") : bus_));
      radio_->setEnabled(false);
    }
  }

  ~MainWindow() override = default;

 protected:
  void closeEvent(QCloseEvent* event) override
  {
    stopAndJoinAllSenderThreads();
    ivy_.stop();
    QWidget::closeEvent(event);
  }

  void stopAndJoinAllSenderThreads()
  {
    stopAllSenders_.store(true, std::memory_order_release);
    senderStopCv_.notify_all();

    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(senderThreadsMutex_);
      workers.swap(senderThreads_);
    }

    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }

    stopAllSenders_.store(false, std::memory_order_release);
  }

 private:
  QRadioButton* radio_{nullptr};
  QString bus_;
  IvyBridge ivy_;
  std::vector<std::thread> senderThreads_;
  std::mutex senderThreadsMutex_;
  std::condition_variable senderStopCv_;
  std::atomic<bool> stopAllSenders_{false};
};

int main(int argc, char* argv[])
{
  QApplication app(argc, argv);

  QString bus = (argc >= 2) ? QString::fromLocal8Bit(argv[1]) : QString();
  if (bus.isEmpty()) {
    QByteArray fromEnv = qgetenv("IVYBUS");
    bus = QString::fromLatin1(fromEnv);
  }

  MainWindow w(bus);
  w.show();
  return app.exec();
}

#include "main.moc"
