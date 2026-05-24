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
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>
#include <vector>

namespace consolation::ui {

namespace {

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
        for (auto formatIndex = 0; formatIndex < static_cast<int>(device.formats.size()); ++formatIndex) {
            formatCombo->addItem(device.formats[formatIndex].label, formatIndex);
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
    };
    connect(deviceCombo, &QComboBox::currentIndexChanged, this, [updateSelection](int) { updateSelection(); });
    connect(formatCombo, &QComboBox::currentIndexChanged, this, [updateSelection](int) { updateSelection(); });
    updateSelection();
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

    const auto selectedStableId = selectedDevice_.stableId;
    const auto selectedV4l2Path = selectedDevice_.v4l2DevicePath.isEmpty()
        ? selectedDevice_.devicePath
        : selectedDevice_.v4l2DevicePath;
    const auto selectedDisplayName = selectedDevice_.displayName;
    const auto selectedFormatLabel = selectedFormat_.label;

    devices_ = capture::CaptureBackendManager().enumerateDevices();
    auto refreshedDeviceIndex = -1;
    for (auto index = 0; index < static_cast<int>(devices_.size()); ++index) {
        const auto &device = devices_[static_cast<size_t>(index)];
        if (!selectedStableId.isEmpty() && device.stableId == selectedStableId) {
            refreshedDeviceIndex = index;
            break;
        }
        if (!selectedV4l2Path.isEmpty() &&
            (device.v4l2DevicePath == selectedV4l2Path || device.devicePath == selectedV4l2Path)) {
            refreshedDeviceIndex = index;
            break;
        }
        if (!selectedDisplayName.isEmpty() && device.displayName == selectedDisplayName) {
            refreshedDeviceIndex = index;
        }
    }

    if (refreshedDeviceIndex < 0) {
        QMessageBox::warning(
            this,
            QStringLiteral("Capture Failed"),
            QStringLiteral(
                "The selected capture device is no longer available. It may still be reconnecting after a USB reset."));
        stopPlayback();
        return;
    }

    selectedDevice_ = devices_[static_cast<size_t>(refreshedDeviceIndex)];
    auto refreshedFormatIndex = 0;
    for (auto index = 0; index < static_cast<int>(selectedDevice_.formats.size()); ++index) {
        if (selectedDevice_.formats[static_cast<size_t>(index)].label == selectedFormatLabel) {
            refreshedFormatIndex = index;
            break;
        }
    }
    selectedFormat_ = selectedDevice_.formats[static_cast<size_t>(refreshedFormatIndex)];

    const auto beginCapture = [&]() -> bool {
        if (captureSession_) {
            captureSession_->stop();
            captureSession_.reset();
        }

        captureSession_ = capture::CaptureBackendManager().createSession(selectedDevice_.backend);
        if (!captureSession_) {
            QMessageBox::warning(
                this,
                QStringLiteral("Capture Failed"),
                QStringLiteral("No capture backend is available for the selected device yet."));
            return false;
        }

        QString startupError;
        const auto startupFailureConnection = connect(
            captureSession_.get(),
            &capture::CaptureSession::failed,
            this,
            [&startupError](const QString &message) {
                startupError = message;
            });

        const auto started = captureSession_->start(selectedDevice_, selectedFormat_);
        disconnect(startupFailureConnection);
        if (!started) {
            QMessageBox::warning(
                this,
                QStringLiteral("Capture Failed"),
                startupError.isEmpty() ? QStringLiteral("Could not start capture.") : startupError);
            captureSession_->stop();
            captureSession_.reset();
            return false;
        }

        connect(captureSession_.get(), &capture::CaptureSession::frameReady, this, [this](const QImage &frame) {
            if (!videoSurface_) {
                showPlaybackState(frame);
                return;
            }
            updateVideoFrame(frame);
        });
        connect(captureSession_.get(), &capture::CaptureSession::failed, this, [this](const QString &message) {
            QMessageBox::warning(this, QStringLiteral("Capture Failed"), message);
            stopPlayback();
        });

        return true;
    };

    if (beginCapture()) {
        auto *session = captureSession_.get();
        QTimer::singleShot(10000, this, [this, session]() {
            if (captureSession_.get() == session && !videoSurface_) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Capture Failed"),
                    QStringLiteral("Timed out waiting for capture frames."));
                stopPlayback();
            }
        });
        return;
    }

    stopPlayback();
}

void MainWindow::showConnectingState()
{
    playbackControls_.clear();
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

    auto *video = new QLabel(root);
    video->setAlignment(Qt::AlignCenter);
    video->setStyleSheet(QStringLiteral("background-color: black; color: #808080; font-size: 24px;"));
    video->setText(QStringLiteral("Waiting for video frame..."));
    video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(video, 1);
    videoSurface_ = video;

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

    connect(powerButton, &QPushButton::clicked, this, [this]() { stopPlayback(); });
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
    showPlaybackControls();
    resetPlaybackControlsTimer();
}

void MainWindow::updateVideoFrame(const QImage &frame)
{
    if (!videoSurface_ || frame.isNull()) {
        return;
    }

    const auto scaled = QPixmap::fromImage(frame).scaled(
        videoSurface_->size(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
    videoSurface_->setPixmap(scaled);
}

void MainWindow::stopPlayback()
{
    if (captureSession_) {
        captureSession_->stop();
        captureSession_.reset();
    }
    videoSurface_.clear();
    buildStoppedState();
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
