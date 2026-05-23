#include "ui/MainWindow.h"

#include "AppMetadata.h"
#include "app/BuildInfo.h"
#include "ui/AppIcon.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QStringList>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

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

QComboBox *makeMockCombo(const QStringList &items, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->addItems(items);
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
}

void MainWindow::buildStoppedState()
{
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

    form->addWidget(makeFieldLabel(QStringLiteral("Device"), panel), 0, 0);
    form->addWidget(
        makeMockCombo(QStringList{QStringLiteral("Mock Capture Device")}, panel),
        0,
        1);

    form->addWidget(makeFieldLabel(QStringLiteral("Resolution & Frame Rate"), panel), 1, 0);
    form->addWidget(
        makeMockCombo(QStringList{QStringLiteral("1920x1080 @ 60p")}, panel),
        1,
        1);

    panelLayout->addLayout(form);

    auto *playButton = new QPushButton(panel);
    playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton->setIconSize(QSize(42, 42));
    playButton->setFixedSize(72, 72);
    playButton->setEnabled(false);
    playButton->setToolTip(QStringLiteral("Mock playback is not wired yet."));
    playButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: white; border-radius: 36px; border: none; "
        "background-color: rgba(255, 255, 255, 85); }"
        "QPushButton:disabled { background-color: rgba(255, 255, 255, 34); }"));
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
