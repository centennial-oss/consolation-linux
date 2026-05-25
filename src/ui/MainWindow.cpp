#include "ui/MainWindow.h"

#include "AppMetadata.h"
#include "app/BuildInfo.h"
#include "capture/CaptureBackendManager.h"
#include "capture/CaptureSession.h"
#include "ui/AppIcon.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMetaType>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QStringList>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <utility>
#include <unistd.h>
#include <vector>

namespace consolation::ui {

class VideoSurface final : public QWidget {
public:
    explicit VideoSurface(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setFrame(const QImage &frame)
    {
        frame_ = frame;
        update();
    }

    void setOverlay(QWidget *overlay)
    {
        overlay_ = overlay;
        positionOverlay();
    }

    int takePaintCount()
    {
        const auto count = paintCount_;
        paintCount_ = 0;
        return count;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        ++paintCount_;
        painter.fillRect(rect(), Qt::black);
        if (frame_.isNull()) {
            return;
        }

        auto targetSize = frame_.size();
        targetSize.scale(size(), Qt::KeepAspectRatio);
        const QRect target(
            QPoint((width() - targetSize.width()) / 2, (height() - targetSize.height()) / 2),
            targetSize);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, frame_);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        positionOverlay();
    }

private:
    void positionOverlay()
    {
        if (overlay_ == nullptr) {
            return;
        }

        overlay_->adjustSize();
        const int margin = 14;
        const auto overlaySize = overlay_->sizeHint();
        overlay_->setGeometry(
            margin,
            std::max(margin, height() - overlaySize.height() - margin),
            std::min(overlaySize.width(), std::max(0, width() - margin * 2)),
            overlaySize.height());
        overlay_->raise();
    }

    QImage frame_;
    QPointer<QWidget> overlay_;
    int paintCount_ = 0;
};

namespace {

void logCaptureStartup(const char *stage, const QString &detail = {})
{
    std::cout << "[MainWindow capture] " << stage;
    if (!detail.isEmpty()) {
        std::cout << ": " << detail.toStdString();
    }
    std::cout << std::endl;
    std::cout.flush();
}

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

quint32 stringToFourCc(const QString &value)
{
    const auto latin = value.toLatin1();
    if (latin.size() < 4) {
        return 0;
    }
    return v4l2_fourcc(latin[0], latin[1], latin[2], latin[3]);
}

constexpr auto accentColor = "#CC11BB";
constexpr auto panelStyle = R"(
    QFrame#startupPanel {
        background-color: rgba(16, 0, 24, 64);
        border: 1px solid rgba(255, 255, 255, 85);
        border-radius: 18px;
    }
)";
constexpr auto comboStyle = R"(
    QComboBox {
        color: white;
        background: transparent;
        border: none;
        border-bottom: 2px solid rgba(255, 255, 255, 190);
        padding: 6px 28px 7px 0;
        font-size: 18px;
    }
    QComboBox::drop-down {
        border: none;
        width: 24px;
    }
    QComboBox QAbstractItemView {
        color: white;
        background-color: #250019;
        selection-background-color: #CC11BB;
    }
)";
constexpr auto pillButtonStyle = R"(
    QPushButton {
        color: white;
        background-color: rgba(0, 0, 0, 38);
        border: 1px solid rgba(255, 255, 255, 145);
        border-radius: 18px;
        padding: 9px 18px;
        font-weight: 700;
        min-height: 18px;
    }
    QPushButton:hover {
        border-color: #CC11BB;
        background-color: rgba(204, 17, 187, 48);
    }
)";
constexpr auto dialogStyle = R"(
    QDialog {
        background-color: #191919;
        color: white;
    }
    QLabel {
        color: white;
    }
    QTextEdit {
        color: white;
        background-color: rgba(255, 255, 255, 26);
        border: 1px solid rgba(255, 255, 255, 55);
        border-radius: 8px;
        padding: 10px;
        font-family: monospace;
    }
    QPushButton {
        color: white;
        background-color: rgba(255, 255, 255, 20);
        border: 1px solid rgba(255, 255, 255, 120);
        border-radius: 16px;
        padding: 7px 14px;
        font-weight: 700;
    }
    QPushButton:hover {
        border-color: #CC11BB;
        background-color: rgba(204, 17, 187, 48);
    }
)";
constexpr auto playbackButtonStyle = R"(
    QPushButton {
        color: white;
        background-color: rgba(255, 255, 255, 22);
        border: none;
        border-radius: 32px;
        font-size: 30px;
        font-weight: 500;
    }
    QPushButton:hover {
        background-color: rgba(255, 255, 255, 42);
    }
)";
constexpr auto playbackSliderStyle = R"(
    QSlider::groove:horizontal {
        height: 8px;
        background: rgba(255, 255, 255, 145);
        border-radius: 4px;
    }
    QSlider::sub-page:horizontal {
        background: #CC11BB;
        border-radius: 4px;
    }
    QSlider::handle:horizontal {
        background: #CC11BB;
        border: 8px solid #4a4a4f;
        width: 22px;
        height: 22px;
        margin: -15px 0;
        border-radius: 19px;
    }
)";
constexpr bool showVideoStatsOverlay = true;
constexpr bool showAdvancedVideoStats = true;

class GradientBackground final : public QWidget {
public:
    explicit GradientBackground(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        QLinearGradient gradient(rect().topLeft(), rect().bottomLeft());
        gradient.setColorAt(0.0, QColor("#8C0573"));
        gradient.setColorAt(0.48, QColor("#470330"));
        gradient.setColorAt(1.0, QColor("#000000"));
        painter.fillRect(rect(), gradient);
    }
};

QLabel *makePlaceholderIcon(QWidget *parent)
{
    auto *icon = new QLabel(parent);
    icon->setFixedSize(64, 64);
    icon->setPixmap(createAppIcon().pixmap(64, 64));
    icon->setScaledContents(true);
    icon->setStyleSheet(QStringLiteral("border-radius: 16px;"));
    return icon;
}

QLabel *makeFieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setStyleSheet(QStringLiteral("color: white; font-size: 17px; font-weight: 700;"));
    return label;
}

QFrame *makeDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 65);"));
    return divider;
}

QComboBox *makeStartupCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setStyleSheet(QString::fromUtf8(comboStyle));
    combo->setMinimumWidth(320);
    combo->setMaximumWidth(340);
    return combo;
}

QPushButton *makePillButton(const QIcon &icon, const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(icon, text, parent);
    button->setIconSize(QSize(18, 18));
    button->setFixedHeight(38);
    button->setStyleSheet(QString::fromUtf8(pillButtonStyle));
    return button;
}

QFrame *makeBarDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedHeight(38);
    divider->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 70);"));
    return divider;
}

QPushButton *makePlaybackCircleButton(const QString &text, const QString &color, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setFixedSize(36, 36);
    button->setStyleSheet(QString::fromUtf8(playbackButtonStyle) + QStringLiteral("QPushButton { color: %1; }").arg(color));
    return button;
}

QSlider *makePlaybackSlider(QWidget *parent)
{
    auto *slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(0, 100);
    slider->setFixedWidth(170);
    slider->setStyleSheet(QString::fromUtf8(playbackSliderStyle));
    return slider;
}

void addInfoRow(QVBoxLayout *layout, QWidget *parent, const QString &symbol, const QString &title, const QString &body)
{
    auto *row = new QHBoxLayout();
    row->setSpacing(14);

    auto *icon = new QLabel(symbol, parent);
    icon->setFixedWidth(32);
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    icon->setStyleSheet(QStringLiteral("color: %1; font-size: 22px; font-weight: 700;").arg(accentColor));

    auto *text = new QLabel(parent);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setText(QStringLiteral("<b>%1</b><br>%2").arg(title.toHtmlEscaped(), body.toHtmlEscaped()));
    text->setStyleSheet(QStringLiteral("color: white; font-size: 14px; line-height: 1.3;"));

    row->addWidget(icon);
    row->addWidget(text, 1);
    layout->addLayout(row);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<capture::VideoTelemetrySnapshot>("consolation::capture::VideoTelemetrySnapshot");

    setWindowTitle(QStringLiteral("%1 %2").arg(
        QString::fromUtf8(consolation::app::AppMetadata::displayName),
        QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)));
    setWindowIcon(createAppIcon());
    resize(1200, 760);
    setMinimumSize(820, 520);

    buildStoppedState();

    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    controlsHideTimer_->setInterval(3000);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this]() { hidePlaybackControls(); });

    videoRenderTimer_ = new QTimer(this);
    videoRenderTimer_->setSingleShot(true);
    videoRenderTimer_->setInterval(16);
    connect(videoRenderTimer_, &QTimer::timeout, this, [this]() { renderLatestVideoFrame(); });

    statsOverlayTimer_ = new QTimer(this);
    statsOverlayTimer_->setInterval(500);
    connect(statsOverlayTimer_, &QTimer::timeout, this, [this]() { updateStatsOverlay(); });
}

MainWindow::~MainWindow()
{
    if (captureSession_ && captureThread_ && captureThread_->isRunning()) {
        QMetaObject::invokeMethod(
            captureSession_.get(), &capture::CaptureSession::stop, Qt::BlockingQueuedConnection);
    }
    if (captureThread_) {
        captureThread_->quit();
        captureThread_->wait();
        delete captureThread_;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == centralWidget() && playbackControls_ && event->type() == QEvent::MouseMove) {
        showPlaybackControls();
        resetPlaybackControlsTimer();
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildStoppedState()
{
    playbackControls_.clear();
    statsOverlay_.clear();
    latestVideoFrame_ = {};
    latestTelemetry_ = {};
    uiFramesSinceStats_ = 0;
    uiFps_ = 0.0;
    paintFps_ = 0.0;
    if (videoRenderTimer_ != nullptr) {
        videoRenderTimer_->stop();
    }
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    if (controlsHideTimer_ != nullptr) {
        controlsHideTimer_->stop();
    }
    devices_ = capture::CaptureBackendManager().enumerateDevices();

    auto *root = new GradientBackground(this);
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(24, 24, 24, 18);
    rootLayout->setSpacing(0);

    rootLayout->addStretch(1);

    auto *panel = new QFrame(root);
    panel->setObjectName(QStringLiteral("startupPanel"));
    panel->setStyleSheet(QString::fromUtf8(panelStyle));
    panel->setMaximumWidth(660);
    panel->setMinimumWidth(560);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(28, 28, 28, 28);
    panelLayout->setSpacing(20);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    header->addWidget(makePlaceholderIcon(panel));

    auto *title = new QLabel(QStringLiteral("Consolation"), panel);
    title->setStyleSheet(QStringLiteral("color: white; font-size: 31px; font-weight: 800;"));
    header->addWidget(title);

    auto *version = new QLabel(
        QStringLiteral("v%1").arg(QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)),
        panel);
    version->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180); font-size: 18px;"));
    header->addWidget(version);
    header->addStretch();
    panelLayout->addLayout(header);
    panelLayout->addWidget(makeDivider(panel));

    auto *form = new QGridLayout();
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(30);
    form->setColumnMinimumWidth(0, 250);

    auto *deviceCombo = makeStartupCombo(panel);
    auto *formatCombo = makeStartupCombo(panel);

    for (auto index = 0; index < static_cast<int>(devices_.size()); ++index) {
        const auto &device = devices_[index];
        deviceCombo->addItem(
            QStringLiteral("%1 (%2)").arg(device.displayName, device.devicePath),
            index);
    }

    const auto refreshFormats = [this, deviceCombo, formatCombo]() {
        formatCombo->clear();
        const auto deviceIndex = deviceCombo->currentData().toInt();
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size())) {
            return;
        }
        const auto &device = devices_[deviceIndex];
        auto preferredFormatComboIndex = -1;
        for (auto formatIndex = 0; formatIndex < static_cast<int>(device.formats.size()); ++formatIndex) {
            const auto &format = device.formats[formatIndex];
            formatCombo->addItem(format.label, formatIndex);
            if (preferredFormatComboIndex < 0 &&
                format.width == 1920 &&
                format.height == 1080 &&
                std::abs(format.framesPerSecond - 60.0) <= 0.5 &&
                (format.pixelFormat == QStringLiteral("YUYV") || format.pixelFormat == QStringLiteral("YUY2"))) {
                preferredFormatComboIndex = formatCombo->count() - 1;
            }
        }
        if (preferredFormatComboIndex >= 0) {
            formatCombo->setCurrentIndex(preferredFormatComboIndex);
        }
    };
    refreshFormats();

    form->addWidget(makeFieldLabel(QStringLiteral("Device"), panel), 0, 0);
    form->addWidget(deviceCombo, 0, 1);

    form->addWidget(makeFieldLabel(QStringLiteral("Resolution & Frame Rate"), panel), 1, 0);
    form->addWidget(formatCombo, 1, 1);

    panelLayout->addLayout(form);

    auto *playButton = new QPushButton(panel);
    playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton->setIconSize(QSize(42, 42));
    playButton->setFixedSize(72, 72);
    playButton->setEnabled(deviceCombo->currentIndex() >= 0 && formatCombo->currentIndex() >= 0);
    playButton->setToolTip(QStringLiteral("Start playback."));
    playButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: white; border-radius: 36px; border: none; "
        "background-color: rgba(255, 255, 255, 85); }"
        "QPushButton:disabled { background-color: rgba(255, 255, 255, 34); }"));
    connect(playButton, &QPushButton::clicked, this, [this, playButton]() {
        const auto deviceIndex = selectedDevice_.devicePath.isEmpty() ? -1 : 0;
        Q_UNUSED(deviceIndex);
        playButton->setEnabled(false);
        startPlayback();
    });
    connect(
        deviceCombo,
        &QComboBox::currentIndexChanged,
        this,
        [refreshFormats, playButton, deviceCombo, formatCombo](int) {
            refreshFormats();
            playButton->setEnabled(deviceCombo->currentIndex() >= 0 && formatCombo->currentIndex() >= 0);
        });
    connect(
        formatCombo,
        &QComboBox::currentIndexChanged,
        this,
        [playButton, deviceCombo, formatCombo](int) {
            playButton->setEnabled(deviceCombo->currentIndex() >= 0 && formatCombo->currentIndex() >= 0);
        });
    const auto updateSelection = [this, deviceCombo, formatCombo]() {
        const auto deviceIndex = deviceCombo->currentData().toInt();
        if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices_.size())) {
            selectedDevice_ = {};
            selectedFormat_ = {};
            return;
        }
        selectedDevice_ = devices_[deviceIndex];
        const auto formatIndex = formatCombo->currentData().toInt();
        if (formatIndex < 0 || formatIndex >= static_cast<int>(selectedDevice_.formats.size())) {
            selectedFormat_ = {};
            return;
        }
        selectedFormat_ = selectedDevice_.formats[formatIndex];
        QTimer::singleShot(0, this, [this]() { preconfigureSelectedFormat(); });
    };
    connect(deviceCombo, &QComboBox::currentIndexChanged, this, [updateSelection](int) { updateSelection(); });
    connect(formatCombo, &QComboBox::currentIndexChanged, this, [updateSelection](int) { updateSelection(); });
    updateSelection();
    QTimer::singleShot(0, this, [this]() { preconfigureSelectedFormat(true); });
    panelLayout->addWidget(playButton, 0, Qt::AlignCenter);

    rootLayout->addWidget(panel, 0, Qt::AlignHCenter);
    rootLayout->addStretch(1);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(14);
    buttonRow->addStretch();

    auto *settingsButton = makePillButton(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView),
        QStringLiteral("Settings"),
        root);

    auto *helpButton = makePillButton(
        style()->standardIcon(QStyle::SP_MessageBoxQuestion),
        QStringLiteral("Help"),
        root);
    auto *aboutButton = makePillButton(
        style()->standardIcon(QStyle::SP_MessageBoxInformation),
        QStringLiteral("About"),
        root);

    connect(settingsButton, &QPushButton::clicked, this, [this]() { showSettingsDialog(); });
    connect(helpButton, &QPushButton::clicked, this, [this]() { showHelpDialog(); });
    connect(aboutButton, &QPushButton::clicked, this, [this]() { showAboutDialog(); });

    buttonRow->addWidget(settingsButton);
    buttonRow->addWidget(helpButton);
    buttonRow->addWidget(aboutButton);
    rootLayout->addLayout(buttonRow);

    setCentralWidget(root);
}

void MainWindow::startPlayback()
{
    showConnectingState();

    if (selectedDevice_.devicePath.startsWith(QStringLiteral("mock://"))) {
        QTimer::singleShot(700, this, [this]() { showPlaybackState(); });
        return;
    }

    // OBS tears down the active V4L2 source before opening/configuring it again.
    // Do the same here before opening the selected device for the new session.
    if (captureSession_) {
        if (captureThread_ && captureThread_->isRunning()) {
            QMetaObject::invokeMethod(
                captureSession_.get(), &capture::CaptureSession::stop, Qt::BlockingQueuedConnection);
        } else {
            captureSession_->stop();
        }
        captureSession_.reset();
    }
    if (captureThread_) {
        captureThread_->quit();
        captureThread_->wait();
        delete captureThread_;
        captureThread_ = nullptr;
    }

    if (selectedDevice_.devicePath.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Capture Failed"),
            QStringLiteral("No capture device is selected."));
        stopPlayback();
        return;
    }

    captureSession_ = capture::CaptureBackendManager().createSession(selectedDevice_.backend);
    if (!captureSession_) {
        QMessageBox::warning(
            this,
            QStringLiteral("Capture Failed"),
            QStringLiteral("No capture backend is available for the selected device yet."));
        stopPlayback();
        return;
    }

    // Move the session to a dedicated thread so the blocking USB-reset recovery in start()
    // doesn't freeze the UI event loop. Signals from the session to main-thread slots are
    // automatically delivered as queued connections across the thread boundary.
    logCaptureStartup(
        "creating capture thread",
        QStringLiteral("%1 @ %2").arg(selectedDevice_.displayName, selectedFormat_.label));
    captureThread_ = new QThread();
    captureSession_->moveToThread(captureThread_);
    logCaptureStartup("session moved to capture thread");

    connect(captureSession_.get(), &capture::CaptureSession::frameReady, this, [this](const QImage &frame) {
        if (!videoSurface_) {
            showPlaybackState(frame);
            return;
        }
        scheduleVideoFrame(frame);
    });
    connect(captureSession_.get(), &capture::CaptureSession::failed, this, [this](const QString &message) {
        logCaptureStartup("failed", message);
        QMessageBox::warning(this, QStringLiteral("Capture Failed"), message);
        stopPlayback();
    });
    connect(captureSession_.get(), &capture::CaptureSession::logMessage, this, [](const QString &message) {
        logCaptureStartup("session", message);
    });
    connect(captureSession_.get(), &capture::CaptureSession::telemetryReady, this, [this](const capture::VideoTelemetrySnapshot &snapshot) {
        latestTelemetry_ = snapshot;
    });

    const auto deviceSnapshot = selectedDevice_;
    const auto formatSnapshot = selectedFormat_;
    logCaptureStartup(
        "queuing CaptureSession::start",
        QStringLiteral("%1 (%2)").arg(deviceSnapshot.devicePath, formatSnapshot.label));
    QMetaObject::invokeMethod(
        captureSession_.get(),
        [this, deviceSnapshot, formatSnapshot]() {
            logCaptureStartup(
                "CaptureSession::start on worker thread",
                QStringLiteral("thread=%1").arg(
                    QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16)));
            const auto started = captureSession_->start(deviceSnapshot, formatSnapshot);
            logCaptureStartup("CaptureSession::start finished", started ? QStringLiteral("ok") : QStringLiteral("failed"));
        },
        Qt::QueuedConnection);

    captureThread_->start();
    logCaptureStartup("capture thread started");

    auto *sessionPtr = captureSession_.get();
    QTimer::singleShot(10000, this, [this, sessionPtr]() {
        if (captureSession_.get() == sessionPtr && !videoSurface_) {
            logCaptureStartup("timed out waiting for first frame");
            QMessageBox::warning(
                this,
                QStringLiteral("Capture Failed"),
                QStringLiteral("Timed out waiting for capture frames."));
            stopPlayback();
        }
    });
    logCaptureStartup("started 10s first-frame timeout");
}

void MainWindow::showConnectingState()
{
    playbackControls_.clear();
    statsOverlay_.clear();
    latestVideoFrame_ = {};
    latestTelemetry_ = {};
    uiFramesSinceStats_ = 0;
    uiFps_ = 0.0;
    paintFps_ = 0.0;
    if (videoRenderTimer_ != nullptr) {
        videoRenderTimer_->stop();
    }
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    if (controlsHideTimer_ != nullptr) {
        controlsHideTimer_->stop();
    }

    auto *root = new QWidget(this);
    root->setStyleSheet(QStringLiteral("background-color: black;"));

    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 24, 24, 24);

    auto *label = new QLabel(QStringLiteral("Connecting to Capture Card..."), root);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: #808080; font-size: 32px; font-weight: 500;"));

    layout->addWidget(label, 1);
    setCentralWidget(root);
}

void MainWindow::showPlaybackState(const QImage &firstFrame)
{
    auto *root = new QWidget(this);
    root->setStyleSheet(QStringLiteral("background-color: black;"));
    root->setMouseTracking(true);
    root->installEventFilter(this);

    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 24, 24, 32);

    auto *video = new VideoSurface(root);
    videoSurface_ = video;

    auto *statsOverlay = new QLabel(video);
    statsOverlay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statsOverlay->setWordWrap(false);
    statsOverlay->setMargin(10);
    statsOverlay->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background-color: rgba(0, 0, 0, 170); "
        "border: 1px solid rgba(255, 255, 255, 80); border-radius: 6px; "
        "font-family: monospace; font-size: 13px; }"));
    statsOverlay->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    statsOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    statsOverlay_ = statsOverlay;
    video->setOverlay(statsOverlay);
    updateStatsOverlay();

    layout->addWidget(video, 1);

    auto *controls = new QFrame(root);
    controls->setFixedHeight(58);
    controls->setStyleSheet(QStringLiteral(
        "QFrame { background-color: rgba(34, 34, 34, 221); "
        "border: 1px solid rgba(68, 68, 68, 238); border-radius: 29px; }"));

    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(12, 4, 12, 4);
    controlsLayout->setSpacing(10);

    auto *powerButton = makePlaybackCircleButton(QStringLiteral("P"), QStringLiteral("#ff453a"), controls);
    auto *volumeButton = makePlaybackCircleButton(QStringLiteral("V"), QStringLiteral("white"), controls);
    auto *zoomOut = new QLabel(QStringLiteral("-"), controls);
    auto *zoomIn = new QLabel(QStringLiteral("+"), controls);
    auto *settingsButton = makePlaybackCircleButton(QStringLiteral("S"), QStringLiteral("white"), controls);

    zoomOut->setAlignment(Qt::AlignCenter);
    zoomIn->setAlignment(Qt::AlignCenter);
    zoomOut->setStyleSheet(QStringLiteral("color: white; font-size: 40px; font-weight: 300; border: none;"));
    zoomIn->setStyleSheet(QStringLiteral("color: white; font-size: 34px; font-weight: 300; border: none;"));

    auto *volumeSlider = makePlaybackSlider(controls);
    volumeSlider->setValue(settings_.volumePercent());
    auto *zoomSlider = makePlaybackSlider(controls);
    zoomSlider->setValue(0);

    connect(powerButton, &QPushButton::clicked, this, [this]() { stopPlaybackAsync(); });
    connect(settingsButton, &QPushButton::clicked, this, [this]() { showSettingsDialog(); });
    connect(volumeSlider, &QSlider::valueChanged, this, [this](const int value) {
        settings_.setVolumePercent(value);
        resetPlaybackControlsTimer();
    });

    controlsLayout->addWidget(powerButton);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(volumeButton);
    controlsLayout->addWidget(volumeSlider);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(zoomOut);
    controlsLayout->addWidget(zoomSlider);
    controlsLayout->addWidget(zoomIn);
    controlsLayout->addWidget(makeBarDivider(controls));
    controlsLayout->addWidget(settingsButton);

    layout->addWidget(controls, 0, Qt::AlignHCenter | Qt::AlignBottom);
    playbackControls_ = controls;
    setCentralWidget(root);
    if (!firstFrame.isNull()) {
        updateVideoFrame(firstFrame);
    }
    if (statsOverlayTimer_ != nullptr && showVideoStatsOverlay) {
        statsOverlayTimer_->start();
    }
    showPlaybackControls();
    resetPlaybackControlsTimer();
}

void MainWindow::updateVideoFrame(const QImage &frame)
{
    if (!videoSurface_ || frame.isNull()) {
        return;
    }

    videoSurface_->setFrame(frame);
    ++uiFramesSinceStats_;
}

void MainWindow::scheduleVideoFrame(const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    latestVideoFrame_ = frame;
    if (videoRenderTimer_ != nullptr && !videoRenderTimer_->isActive()) {
        videoRenderTimer_->start();
    }
}

void MainWindow::renderLatestVideoFrame()
{
    if (latestVideoFrame_.isNull()) {
        return;
    }

    const auto frame = latestVideoFrame_;
    latestVideoFrame_ = {};
    updateVideoFrame(frame);
}

void MainWindow::updateStatsOverlay()
{
    if (!statsOverlay_) {
        return;
    }

    uiFps_ = uiFramesSinceStats_ * 2.0;
    uiFramesSinceStats_ = 0;
    paintFps_ = videoSurface_ ? videoSurface_->takePaintCount() * 2.0 : 0.0;

    if (!showVideoStatsOverlay) {
        statsOverlay_->hide();
        return;
    }

    statsOverlay_->setText(buildStatsOverlayText());
    statsOverlay_->adjustSize();
    if (videoSurface_) {
        videoSurface_->setOverlay(statsOverlay_);
    }
    statsOverlay_->show();
}

QString MainWindow::buildStatsOverlayText() const
{
    const auto width = latestTelemetry_.width > 0 ? latestTelemetry_.width : selectedFormat_.width;
    const auto height = latestTelemetry_.height > 0 ? latestTelemetry_.height : selectedFormat_.height;
    const auto configuredFps = latestTelemetry_.configuredFps > 0.0 ? latestTelemetry_.configuredFps : selectedFormat_.framesPerSecond;
    const auto pixelFormat = latestTelemetry_.pixelFormat.isEmpty() ? selectedFormat_.pixelFormat : latestTelemetry_.pixelFormat;

    QStringList fields {
        QStringLiteral("%1x%2/%3").arg(width).arg(height).arg(QString::number(configuredFps, 'f', 0)),
        pixelFormat,
        QStringLiteral("FPS:%1").arg(QString::number(latestTelemetry_.decodedFps, 'f', 0)),
    };

    if (!showAdvancedVideoStats) {
        return fields.join(QStringLiteral(" | "));
    }

    fields += QStringList {
        QStringLiteral("UI:%1").arg(QString::number(uiFps_, 'f', 0)),
        QStringLiteral("Paint:%1").arg(QString::number(paintFps_, 'f', 0)),
        QStringLiteral("Cnv:%1").arg(QString::number(latestTelemetry_.decodeAvgMs, 'f', 1)),
        QStringLiteral("CnvMx:%1").arg(QString::number(latestTelemetry_.decodeMaxMs, 'f', 1)),
        QStringLiteral("Cad:%1").arg(configuredFps > 0.0 ? QString::number(1000.0 / configuredFps, 'f', 1) : QStringLiteral("0.0")),
        QStringLiteral("Buf:%1").arg(latestTelemetry_.bufferCount),
        QStringLiteral("Payload:%1KiB").arg(QString::number(latestTelemetry_.payloadAvgKb, 'f', 0)),
    };
    return fields.join(QStringLiteral(" | "));
}

void MainWindow::preconfigureSelectedFormat(const bool force)
{
    if (selectedDevice_.backend != capture::CaptureBackend::V4L2 ||
        selectedDevice_.devicePath.isEmpty() ||
        selectedFormat_.width <= 0 ||
        selectedFormat_.height <= 0 ||
        selectedFormat_.pixelFormat.isEmpty()) {
        return;
    }

    const auto key = QStringLiteral("%1|%2x%3|%4|%5")
                         .arg(selectedDevice_.devicePath)
                         .arg(selectedFormat_.width)
                         .arg(selectedFormat_.height)
                         .arg(selectedFormat_.pixelFormat)
                         .arg(selectedFormat_.framesPerSecond, 0, 'f', 2);
    if (!force && preconfiguredFormatKey_ == key) {
        return;
    }
    preconfiguredFormatKey_ = key;

    const auto requestedPixelFormat = stringToFourCc(selectedFormat_.pixelFormat);
    if (requestedPixelFormat == 0) {
        return;
    }

    logCaptureStartup("preconfiguring selected capture format", selectedFormat_.label);
    const int fd = ::open(selectedDevice_.devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logCaptureStartup("preconfigure open failed", QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }

    v4l2_format current {};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &current) != 0) {
        logCaptureStartup("preconfigure G_FMT failed", QString::fromLocal8Bit(std::strerror(errno)));
        ::close(fd);
        return;
    }

    const auto alreadySelected =
        static_cast<int>(current.fmt.pix.width) == selectedFormat_.width &&
        static_cast<int>(current.fmt.pix.height) == selectedFormat_.height &&
        current.fmt.pix.pixelformat == requestedPixelFormat;
    if (!alreadySelected) {
        current.fmt.pix.width = static_cast<__u32>(selectedFormat_.width);
        current.fmt.pix.height = static_cast<__u32>(selectedFormat_.height);
        current.fmt.pix.pixelformat = requestedPixelFormat;
        if (xioctl(fd, VIDIOC_S_FMT, &current) != 0) {
            logCaptureStartup("preconfigure S_FMT failed", QString::fromLocal8Bit(std::strerror(errno)));
        } else {
            logCaptureStartup("preconfigure S_FMT applied; device may reset before Play");
        }
    } else {
        logCaptureStartup("preconfigure skipped; selected format already active");
    }

    ::close(fd);
}

void MainWindow::stopPlayback()
{
    if (captureSession_) {
        if (captureThread_ && captureThread_->isRunning()) {
            QMetaObject::invokeMethod(
                captureSession_.get(), &capture::CaptureSession::stop, Qt::BlockingQueuedConnection);
        } else {
            captureSession_->stop();
        }
        captureSession_.reset();
    }
    if (captureThread_) {
        captureThread_->quit();
        captureThread_->wait();
        delete captureThread_;
        captureThread_ = nullptr;
    }
    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    statsOverlay_.clear();
    videoSurface_.clear();
    buildStoppedState();
}

void MainWindow::stopPlaybackAsync()
{
    auto *session = captureSession_.release();
    auto *thread = captureThread_;
    captureThread_ = nullptr;

    if (statsOverlayTimer_ != nullptr) {
        statsOverlayTimer_->stop();
    }
    statsOverlay_.clear();
    videoSurface_.clear();
    buildStoppedState();

    if (session == nullptr) {
        if (thread != nullptr) {
            thread->quit();
            thread->deleteLater();
        }
        return;
    }

    QObject::disconnect(session, nullptr, this, nullptr);
    if (thread != nullptr && thread->isRunning()) {
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        QMetaObject::invokeMethod(
            session,
            [session, thread]() {
                session->stop();
                session->deleteLater();
                thread->quit();
            },
            Qt::QueuedConnection);
        return;
    }

    session->stop();
    delete session;
    if (thread != nullptr) {
        delete thread;
    }
}

void MainWindow::showPlaybackControls()
{
    if (playbackControls_) {
        playbackControls_->show();
    }
}

void MainWindow::hidePlaybackControls()
{
    if (playbackControls_) {
        playbackControls_->hide();
    }
}

void MainWindow::resetPlaybackControlsTimer()
{
    if (controlsHideTimer_ != nullptr && playbackControls_) {
        controlsHideTimer_->start();
    }
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(580, 420);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("Settings"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    layout->addWidget(title);
    layout->addWidget(makeDivider(&dialog));

    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("S"),
        QStringLiteral("Playback settings"),
        QStringLiteral("Video stats, low-frame-rate warnings, flip, rotation, and other playback preferences will be added with the mock playback UX."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("V"),
        QStringLiteral("Volume"),
        QStringLiteral("The app volume preference is already backed by QSettings and will be wired to the playback controls."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addStretch();
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::showHelpDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Help"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(680, 520);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("Help"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    layout->addWidget(title);
    layout->addWidget(makeDivider(&dialog));

    addInfoRow(
        layout,
        &dialog,
        QStringLiteral(">"),
        QStringLiteral("Getting started"),
        QStringLiteral("Connect a UVC capture card, choose the device and capture format, then press Play."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("FPS"),
        QStringLiteral("Frame rate"),
        QStringLiteral("Higher frame rates feel better for games, but require capture-card and USB bandwidth support."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("[]"),
        QStringLiteral("Video controls"),
        QStringLiteral("During playback, controls will provide stop, app volume, zoom, pan, and settings access."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("A"),
        QStringLiteral("Audio"),
        QStringLiteral("Consolation controls app playback volume independently of the system volume."));
    addInfoRow(
        layout,
        &dialog,
        QStringLiteral("USB"),
        QStringLiteral("Device support"),
        QStringLiteral("Any Linux-supported UVC capture device should work once the real backend is implemented."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::showAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("About Consolation"));
    dialog.setStyleSheet(QString::fromUtf8(dialogStyle));
    dialog.resize(660, 610);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(16);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    header->addWidget(makePlaceholderIcon(&dialog));

    auto *titleBlock = new QVBoxLayout();
    auto *title = new QLabel(
        QStringLiteral("Consolation %1").arg(QString::fromUtf8(consolation::app::BuildInfo::releaseVersion)),
        &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 800;"));
    auto *subtitle = new QLabel(QStringLiteral("Copyright Centennial OSS Inc."), &dialog);
    subtitle->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    titleBlock->addWidget(title);
    titleBlock->addWidget(subtitle);
    header->addLayout(titleBlock, 1);
    layout->addLayout(header);

    auto *body = new QLabel(
        QStringLiteral(
            "Consolation is a no-frills UVC capture viewer for using a Linux workstation "
            "as a display for consoles, Raspberry Pis, and other HDMI devices through a capture card.\n\n"
            "No recording, streaming, analytics, or network access is part of the app design. "
            "Audio and video stay local and transient while you are watching."),
        &dialog);
    body->setWordWrap(true);
    body->setStyleSheet(QStringLiteral("font-size: 14px;"));
    layout->addWidget(body);

    layout->addWidget(makeDivider(&dialog));

    auto *buildInfo = new QTextEdit(&dialog);
    buildInfo->setReadOnly(true);
    buildInfo->setPlainText(consolation::app::BuildInfo::copyableBlob());
    buildInfo->setFixedHeight(120);
    layout->addWidget(buildInfo);

    auto *actions = new QHBoxLayout();
    auto *githubButton = new QPushButton(QStringLiteral("GitHub"), &dialog);
    auto *privacyButton = new QPushButton(QStringLiteral("Privacy Policy"), &dialog);
    auto *copyButton = new QPushButton(QStringLiteral("Copy Build Info"), &dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), &dialog);

    connect(githubButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(consolation::app::AppMetadata::websiteUrl)));
    });
    connect(privacyButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(consolation::app::AppMetadata::privacyUrl)));
    });
    connect(copyButton, &QPushButton::clicked, this, []() {
        QApplication::clipboard()->setText(consolation::app::BuildInfo::copyableBlob());
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    actions->addWidget(githubButton);
    actions->addWidget(privacyButton);
    actions->addStretch();
    actions->addWidget(copyButton);
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    dialog.exec();
}

} // namespace consolation::ui
