/*
 * Minimal Qt demo for the Ivy MTSafe API.
 *
 * The example deliberately shows three common integration points:
 * - a Qt widget sends normal Ivy messages from the GUI thread;
 * - the Ivy main loop runs in its own std::thread;
 * - a small fixed worker pool sends messages from non-Qt threads.
 *
 * The interesting property is not that the GUI has several buttons; it is
 * that all senders use the same IvyContext from different C++ threads.  This
 * is the part that exercises the MTSafe Ivy implementation.
 *
 * Start ivyprobe on the same bus and bind "qtdemo (.*)" to see the worker
 * thread ids stay distinct while repeated sends use the same IvyContext.
 */

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QWidget>

#include <Ivy/ivy.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

/*
 * IvyBridge is the only class that knows about the Ivy C API.
 *
 * Keeping this boundary explicit makes the example easier to read:
 * - MainWindow talks to IvyBridge with Qt signals/slots and plain methods.
 * - IvyBridge owns the IvyContext lifetime.
 * - IvyBridge converts Ivy callbacks back into queued Qt notifications.
 *
 * The queued notification is important. Ivy callbacks run from the Ivy loop
 * thread, while Qt widgets must only be touched from the Qt GUI thread.
 */
class IvyBridge : public QObject
{
  Q_OBJECT

 public:
  explicit IvyBridge(QString bus, QObject* parent = nullptr)
      : QObject(parent), bus_(std::move(bus)) {}

  ~IvyBridge() override { stop(); }

  bool start()
  {
    if (started_.load()) {
      return true;
    }

    /*
     * This demo uses the explicit-context API instead of the historical global
     * Ivy API.  The ownership model is then visible in the code: one
     * MainWindow owns one IvyBridge, and one IvyBridge owns one IvyContext.
     */
    ctx_ = IvyContextCreate("QtDemo", "qtdemo ready", nullptr, nullptr, onDie, this);
    if (!ctx_) {
      qWarning() << "IvyContextCreate failed:" << IvyGetLastError();
      return false;
    }

    /*
     * Ivy callbacks run in the Ivy loop thread. They must not update widgets
     * directly; onIncomingMessage below reposts to the Qt event loop.
     */
    binding_ = IvyContextBindMsg(ctx_, onIncomingMessage, this, "^qtdemo msg (.*)");
    if (!binding_) {
      qWarning() << "IvyContextBindMsg failed:" << IvyGetLastError();
      cleanupCreatedContext();
      return false;
    }

    const QByteArray bus = bus_.toUtf8();
    const int status = IvyContextStart(ctx_, bus.isEmpty() ? nullptr : bus.constData());
    if (status != IVY_OK) {
      qWarning() << "IvyContextStart failed:" << status << IvyGetLastError();
      cleanupCreatedContext();
      return false;
    }

    try {
      /*
       * IvyContextMainLoop is blocking, so it cannot run in the Qt GUI thread.
       * A dedicated std::thread keeps the window responsive while Ivy receives
       * messages in the background.
       */
      loop_ = std::thread(&IvyBridge::loop, this);
      started_.store(true);
    } catch (const std::exception& error) {
      qWarning() << "Unable to start Ivy loop thread:" << error.what();
      cleanupStartedContext();
      return false;
    }
    return true;
  }

  void stop()
  {
    if (!ctx_) {
      return;
    }

    /*
     * Stop order is explicit for readability:
     * 1. remove the subscription while the context is still usable;
     * 2. ask the Ivy loop to stop;
     * 3. join the loop thread;
     * 4. destroy the context.
     */
    if (binding_) {
      IvyContextUnbindMsg(ctx_, binding_);
      binding_ = nullptr;
    }
    IvyContextStop(ctx_);
    if (loop_.joinable()) {
      loop_.join();
    }
    IvyContextDestroy(ctx_);
    ctx_ = nullptr;
    started_.store(false);
  }

  bool sendState(bool on)
  {
    if (!ctx_) {
      return false;
    }
    return IvyContextSendMsg(ctx_, "qtdemo %s", on ? "ON" : "OFF") >= 0;
  }

  bool sendRaw(const QString& payload)
  {
    if (!ctx_) {
      return false;
    }
    /*
     * Worker threads call this method too.  The demo intentionally shares the
     * same IvyContext between the GUI thread and the worker pool.
     */
    const QByteArray msg = payload.toUtf8();
    return IvyContextSendMsg(ctx_, "%s", msg.constData()) >= 0;
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
    const QString msg = QString::fromUtf8(argv[0]);
    /*
     * Do not emit directly from the Ivy callback.  Qt::QueuedConnection stores
     * the lambda in the Qt event queue, and Qt runs it later in the receiver's
     * thread.
     */
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

  void cleanupCreatedContext()
  {
    if (binding_) {
      IvyContextUnbindMsg(ctx_, binding_);
      binding_ = nullptr;
    }
    IvyContextDestroy(ctx_);
    ctx_ = nullptr;
    started_.store(false);
  }

  void cleanupStartedContext()
  {
    if (binding_) {
      IvyContextUnbindMsg(ctx_, binding_);
      binding_ = nullptr;
    }
    IvyContextStop(ctx_);
    IvyContextDestroy(ctx_);
    ctx_ = nullptr;
    started_.store(false);
  }

  void loop() { IvyContextMainLoop(ctx_); }

  IvyContext* ctx_{nullptr};
  QString bus_;
  MsgRcvPtr binding_{nullptr};
  std::thread loop_;
  std::atomic<bool> started_{false};
};

class MainWindow : public QWidget
{
 public:
  explicit MainWindow(QString bus, QWidget* parent = nullptr)
      : QWidget(parent), bus_(std::move(bus)), ivy_(bus_, this)
  {
    setWindowTitle("Ivy Qt Demo");
    setMinimumSize(560, 260);
    resize(560, 260);

    /*
     * The UI is intentionally small:
     * - the radio button sends from the Qt GUI thread;
     * - the push button asks one persistent worker to send;
     * - the line edit displays a message received by the Ivy callback path.
     */
    auto* layout = new QVBoxLayout(this);
    auto* textLine = new QLineEdit(this);

    radio_ = new QRadioButton("qtdemo ON/OFF", this);
    sendWorkerButton_ = new QPushButton("Envoyer un message worker avec thread-id", this);
    radio_->setAutoExclusive(false);
    textLine->setReadOnly(true);
    textLine->setPlaceholderText("Attente d'un message qtdemo msg xxx");

    layout->addWidget(new QLabel("Envoi depuis le thread Qt :", this));
    layout->addWidget(radio_);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Reception de qtdemo msg xxx :", this));
    layout->addWidget(textLine);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Envoi depuis un pool de workers non-Qt :", this));
    layout->addWidget(sendWorkerButton_);
    setLayout(layout);

    connect(&ivy_, &IvyBridge::messageReceived, this,
            [textLine](const QString& message) { textLine->setText(message); });
    connect(&ivy_, &IvyBridge::closeRequested, this, [this]() { close(); },
            Qt::QueuedConnection);
    connect(radio_, &QRadioButton::toggled, this, [this](bool checked) {
      if (ivy_.isStarted()) {
        ivy_.sendState(checked);
      }
    });
    connect(sendWorkerButton_, &QPushButton::clicked, this,
            [this]() { requestWorkerSend(); });

    if (!ivy_.start()) {
      QMessageBox::critical(this,
                            tr("Ivy"),
                            tr("Impossible de demarrer Ivy sur le bus: %1")
                                .arg(bus_.isEmpty() ? QStringLiteral("defaut") : bus_));
      radio_->setEnabled(false);
      sendWorkerButton_->setEnabled(false);
      return;
    }

    if (!startWorkers()) {
      QMessageBox::critical(this,
                            tr("Ivy"),
                            tr("Impossible de demarrer les workers de demonstration."));
      sendWorkerButton_->setEnabled(false);
    }
  }

  ~MainWindow() override { stopWorkers(); }

 protected:
  void closeEvent(QCloseEvent* event) override
  {
    stopWorkers();
    ivy_.stop();
    QWidget::closeEvent(event);
  }

 private:
  /*
   * Each worker owns its synchronization primitives and a tiny command queue.
   * "pending" is only a counter because every command is the same operation:
   * send one Ivy message.  A real application would usually store richer jobs.
   */
  struct Worker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    unsigned int pending{0};
    unsigned int sequence{0};
    bool stop{false};
  };

  static constexpr std::size_t kWorkerCount = 4;

  bool startWorkers()
  {
    /*
     * Workers stay alive until the window closes. That makes their thread ids
     * stable and visibly different in ivyprobe, unlike create/send/join loops
     * where pthread ids may be recycled immediately by the OS.
     */
    for (std::size_t i = 0; i < workers_.size(); ++i) {
      try {
        workers_[i].thread = std::thread(&MainWindow::workerLoop, this, i);
      } catch (const std::exception& error) {
        qWarning() << "Unable to start worker" << i << ":" << error.what();
        stopWorkers();
        return false;
      }
    }
    workersStarted_ = true;
    return true;
  }

  void stopWorkers()
  {
    /*
     * The GUI thread requests termination, then joins outside of the worker
     * locks.  Joining while holding the worker mutex would make it easier to
     * introduce a shutdown deadlock later.
     */
    for (auto& worker : workers_) {
      std::lock_guard<std::mutex> lock(worker.mutex);
      worker.stop = true;
      worker.pending = 0;
      worker.wake.notify_all();
    }

    for (auto& worker : workers_) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    }
    workersStarted_ = false;
  }

  void requestWorkerSend()
  {
    if (!ivy_.isStarted() || !workersStarted_) {
      qWarning() << "Ivy ou workers non demarres";
      return;
    }

    /*
     * Round-robin dispatch makes the visible thread id change in ivyprobe.
     * The workers are persistent, so Linux cannot hide the demo by recycling
     * the same short-lived pthread id over and over.
     */
    Worker& worker = workers_[nextWorker_++ % workers_.size()];
    {
      std::lock_guard<std::mutex> lock(worker.mutex);
      ++worker.pending;
    }
    worker.wake.notify_one();
  }

  void workerLoop(std::size_t index)
  {
    for (;;) {
      unsigned int sequence;
      {
        Worker& worker = workers_[index];
        std::unique_lock<std::mutex> lock(worker.mutex);
        worker.wake.wait(lock, [&worker]() {
          return worker.stop || worker.pending > 0;
        });
        if (worker.stop) {
          return;
        }
        --worker.pending;
        sequence = ++worker.sequence;
      }

      /*
       * The thread id is part of the Ivy payload on purpose.  It is not needed
       * by the program; it is the observable proof that several C++ threads are
       * sending through the same IvyContext.
       */
      std::ostringstream threadId;
      threadId << std::this_thread::get_id();
      const QString payload = QStringLiteral("qtdemo worker %1 thread %2 seq %3")
                                  .arg(index)
                                  .arg(QString::fromStdString(threadId.str()))
                                  .arg(sequence);
      const bool ok = ivy_.sendRaw(payload);
      qInfo() << (ok ? "Worker message sent:" : "Worker message failed:") << payload;
    }
  }

  QRadioButton* radio_{nullptr};
  QPushButton* sendWorkerButton_{nullptr};
  QString bus_;
  IvyBridge ivy_;
  std::array<Worker, kWorkerCount> workers_;
  std::size_t nextWorker_{0};
  bool workersStarted_{false};
};

int main(int argc, char* argv[])
{
  QApplication app(argc, argv);

  /*
   * Accept the bus on the command line for easy demos, but keep the usual
   * IVYBUS fallback so the example behaves like normal Ivy tools.
   */
  QString bus = (argc >= 2) ? QString::fromLocal8Bit(argv[1]) : QString();
  if (bus.isEmpty()) {
    bus = QString::fromLatin1(qgetenv("IVYBUS"));
  }

  MainWindow w(bus);
  w.show();
  return app.exec();
}

#include "main.moc"
