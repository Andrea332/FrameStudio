#include "mainwindow.h"

#include "editor.h"
#include "encodercheck.h"
#include "settings.h"
#include "sizeestimator.h"
#include "spinner.h"
#include "systeminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <cmath>

namespace {

using ComboItems = QList<QPair<QString, QVariant>>;

constexpr int PREVIEW_WIDTH = 192;    // preview frame of the source video, 16:9
constexpr int PREVIEW_HEIGHT = 108;

const char *const VIDEO_EXTENSIONS =
    "mp4 m4v mkv mov qt avi webm wmv asf flv f4v ts mts m2ts m2t mpg mpeg mpe m1v m2v vob evo "
    "3gp 3g2 mxf ogv ogm dv divx xvid rm rmvb nut y4m h264 264 h265 265 hevc ivf obu gif apng "
    "amv mjpeg mjpg wtv dvr-ms";

QString videoFilter()
{
    QStringList patterns;
    for (const QString &ext : QString(VIDEO_EXTENSIONS).split(' '))
        patterns << "*." + ext;
    return "Videos (" + patterns.join(' ') + ");;All files (*)";
}

// Replaces the items of a combo box, keeping the current choice when it is still there (if keep).
void fillCombo(QComboBox *combo, const ComboItems &items, bool keep = true)
{
    const QString current = combo->currentText();
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto &[label, data] : items)
        combo->addItem(label, data);
    combo->setCurrentIndex(keep ? std::max(0, combo->findText(current)) : 0);
}

// Lightning bolt that marks codecs with hardware acceleration (transparent if color is invalid).
QIcon boltIcon(const QColor &color = QColor())
{
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    if (color.isValid()) {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPolygon(QPolygonF({{9.5, 0.5}, {3, 9}, {7.5, 9}, {6, 15.5}, {13, 6.5}, {8.5, 6.5}, {11, 0.5}}));
    }
    return QIcon(pix);
}

// Round badge with a white check mark, exclamation mark or cross, for the result of a compression.
QPixmap badge(const QColor &color, char symbol, int size, qreal dpr)
{
    QPixmap pix(qRound(size * dpr), qRound(size * dpr));
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(0, 0, size, size));
    p.setPen(QPen(Qt::white, size / 9.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const auto at = [size](double x, double y) { return QPointF(x * size, y * size); };
    if (symbol == 'v') {
        p.drawPolyline(QPolygonF({at(0.28, 0.52), at(0.44, 0.68), at(0.73, 0.36)}));
    } else if (symbol == '!') {
        p.drawLine(at(0.5, 0.25), at(0.5, 0.56));
        p.drawPoint(at(0.5, 0.74));
    } else {
        p.drawLine(at(0.34, 0.34), at(0.66, 0.66));
        p.drawLine(at(0.66, 0.34), at(0.34, 0.66));
    }
    return pix;
}

// Scrolls only vertically: it is as wide as its content needs, so the window can't get narrower than that.
class VerticalScrollArea : public QScrollArea
{
public:
    QSize minimumSizeHint() const override
    {
        const int content = widget() ? widget()->minimumSizeHint().width() : 0;
        return {content + verticalScrollBar()->sizeHint().width(), QScrollArea::minimumSizeHint().height()};
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();   // the content changed: its width may have too
        return QScrollArea::eventFilter(watched, event);
    }
};

// Explanations of the rate control modes, in the order of the menu
const char *const RATE_TIPS[] = {
    "The encoder gives each scene the bits it needs to keep the chosen quality:\n"
    "the best quality for the size. The size is estimated by encoding a few samples.",
    "An average bitrate of your choice: complex scenes get more, simple ones less\n"
    "(up to twice the average). The size is about bitrate × duration, smaller for simple videos.",
    "The same bitrate all the time, for streaming or for devices that need it.\n"
    "The size is bitrate × duration; the quality changes from scene to scene.",
};

// Puts the seek index at the start of the file, so players can jump anywhere right away (also when the file
// is streamed or opened over the network).
QStringList seekIndexArgs(const QString &ext)
{
    if (ext == "mp4" || ext == "mov" || ext == "m4v")
        return {"-movflags", "+faststart"};
    if (ext == "mkv" || ext == "webm")
        return {"-cues_to_front", "1"};
    return {};
}

QString number(double seconds)
{
    return QString::number(seconds, 'f', 6);
}

QString withSuffix(const QString &path, const QString &ext)
{
    const QString suffix = QFileInfo(path).suffix();
    return (suffix.isEmpty() ? path + "." : path.left(path.size() - suffix.size())) + ext;
}

bool samePath(const QString &a, const QString &b)
{
    return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}

QString capitalized(QString text)
{
    if (!text.isEmpty())
        text[0] = text[0].toUpper();
    return text;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    const QString version = QCoreApplication::applicationVersion();
    setWindowTitle(version.isEmpty() ? QString("FrameStudio") : "FrameStudio " + version);
    setAcceptDrops(true);

    ffmpeg_ = findTool("ffmpeg");
    ffprobe_ = findTool("ffprobe");
    gpus_ = gpuNames();
    cpu_ = cpuName();
    hwIcon_ = boltIcon(QColor("#f5a623"));
    noIcon_ = boltIcon();
    showMessage = [this](QMessageBox::Icon icon, const QString &title, const QString &text) {
        QMessageBox box(icon, title, text, QMessageBox::Ok, this);
        box.exec();
    };
    askQuestion = [this](const QString &title, const QString &text) {
        return QMessageBox::question(this, title, text) == QMessageBox::Yes;
    };

    encoders_ = new EncoderCheck(ffmpeg_, this);
    connect(encoders_, &EncoderCheck::changed, this, &MainWindow::refreshCodecs);
    estimator_ = new SizeEstimator(ffmpeg_, ffprobe_, this);
    connect(estimator_, &SizeEstimator::done, this, &MainWindow::onEstimateDone);
    connect(estimator_, &SizeEstimator::failed, this, &MainWindow::onEstimateFailed);
    estTimer_ = new QTimer(this);
    estTimer_->setSingleShot(true);
    estTimer_->setInterval(600);
    connect(estTimer_, &QTimer::timeout, this, &MainWindow::runEstimate);

    // --- Update notice (hidden until a newer version is found) ---
    updater_ = new UpdateChecker(this);
    updateBar_ = new QFrame;
    updateBar_->setObjectName("updateBar");
    updateBar_->setStyleSheet("#updateBar { background: rgba(232, 137, 12, 0.14); "
                              "border: 1px solid rgba(232, 137, 12, 0.6); border-radius: 6px; }");
    updateText_ = new QLabel;
    updateText_->setWordWrap(true);
    notesBtn_ = new QPushButton("What's new");
    updateBtn_ = new QPushButton;
    laterBtn_ = new QPushButton("Later");
    auto *updateRowTop = new QHBoxLayout(updateBar_);
    updateRowTop->addWidget(updateText_, 1);
    updateRowTop->addWidget(notesBtn_);
    updateRowTop->addWidget(updateBtn_);
    updateRowTop->addWidget(laterBtn_);
    updateBar_->hide();
    connect(notesBtn_, &QPushButton::clicked, this, [this] {
        if (pendingUpdate_)
            QDesktopServices::openUrl(pendingUpdate_->page);
    });
    connect(updateBtn_, &QPushButton::clicked, this, &MainWindow::startUpdate);
    connect(laterBtn_, &QPushButton::clicked, this, [this] {
        updater_->cancelDownload();
        downloadingUpdate_ = false;
        updateBar_->hide();
    });
    connect(updater_, &UpdateChecker::updateAvailable, this, &MainWindow::onUpdateAvailable);
    connect(updater_, &UpdateChecker::upToDate, this, [this] {
        saveLastUpdateCheck();
        if (manualUpdateCheck_)
            updateStatus_->setText("✔ You have the latest version");
        manualUpdateCheck_ = false;
        checkNowBtn_->setEnabled(true);
    });
    connect(updater_, &UpdateChecker::failed, this, [this](const QString &message) {
        if (downloadingUpdate_) {
            downloadingUpdate_ = false;
            updateText_->setText("⚠ The update could not be downloaded: " + message.toHtmlEscaped());
            updateBtn_->setEnabled(true);
            return;
        }
        if (manualUpdateCheck_)   // automatic checks fail silently (e.g. no internet)
            updateStatus_->setText("⚠ Could not check for updates: " + message.toHtmlEscaped());
        manualUpdateCheck_ = false;
        checkNowBtn_->setEnabled(true);
    });
    connect(updater_, &UpdateChecker::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (pendingUpdate_ && total > 0)
            updateText_->setText(QString("Downloading FrameStudio %1… %2%")
                                     .arg(pendingUpdate_->version).arg(received * 100 / total));
    });
    connect(updater_, &UpdateChecker::downloaded, this, &MainWindow::installUpdate);

    // --- Source ---
    inEdit_ = new QLineEdit;
    inEdit_->setPlaceholderText("Drop a video here or click Browse…");
    connect(inEdit_, &QLineEdit::editingFinished, this, &MainWindow::onInputEdited);
    inBtn_ = new QPushButton("Browse…");
    connect(inBtn_, &QPushButton::clicked, this, &MainWindow::pickInput);
    auto *inRow = new QHBoxLayout;
    inRow->addWidget(inEdit_);
    inRow->addWidget(inBtn_);
    srcInfo_ = new QLabel("No video loaded.");
    srcInfo_->setWordWrap(true);
    srcInfo_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    preview_ = new QLabel;   // shown only when the path points to a video that can be read
    preview_->setFixedSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet("background: black;");
    preview_->hide();
    auto *srcBox = new QGroupBox("Source video");
    auto *srcColumn = new QVBoxLayout;
    srcColumn->addLayout(inRow);
    srcColumn->addWidget(srcInfo_, 1);
    auto *srcLayout = new QHBoxLayout(srcBox);
    srcLayout->addWidget(preview_, 0, Qt::AlignTop);
    srcLayout->addLayout(srcColumn, 1);

    // --- Settings ---
    formatCombo_ = new QComboBox;
    for (const Format &f : formats())
        formatCombo_->addItem(f.label, f.ext);
    vcodecCombo_ = new QComboBox;
    vcodecCombo_->setToolTip("⚡ = this PC has a GPU that can encode the codec (hardware acceleration)");
    accelCombo_ = new QComboBox;
    accelCombo_->addItem("Automatic", "auto");
    accelCombo_->addItem("Manual", "manual");
    accelCombo_->addItem("Off (software, CPU)", "off");
    accelCombo_->setToolTip("Automatic: the best GPU that can encode the chosen codec, otherwise the CPU.\n"
                            "Manual: choose the GPU yourself.\n"
                            "Off: always encode on the CPU (slower, but smaller files at the same quality).");
    deviceCombo_ = new QComboBox;
    auto *accelRow = new QHBoxLayout;
    accelRow->addWidget(accelCombo_);
    accelRow->addWidget(deviceCombo_, 1);
    encoderLabel_ = new QLabel;   // always states codec, hardware acceleration and device
    // a table that needs no wrapping: always its own height, neither squeezed nor stretched by the window
    encoderLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // constant quality (the slider) or a bitrate, variable or constant, when the encoder has it
    rateCombo_ = new QComboBox;
    rateCombo_->addItem("Constant quality (recommended)", int(RateMode::Quality));
    rateCombo_->addItem("Variable bitrate (VBR)", int(RateMode::Vbr));
    rateCombo_->addItem("Constant bitrate (CBR)", int(RateMode::Cbr));
    for (int i = 0; i < rateCombo_->count(); ++i)
        rateCombo_->setItemData(i, QString(RATE_TIPS[i]), Qt::ToolTipRole);
    bitrateSpin_ = new QSpinBox;
    bitrateSpin_->setRange(MIN_VIDEO_KBPS, MAX_VIDEO_KBPS);
    bitrateSpin_->setSingleStep(100);
    bitrateSpin_->setValue(DEFAULT_VIDEO_KBPS);
    bitrateSpin_->setSuffix(" kbps");
    bitrateSpin_->setToolTip("Video bitrate. The audio bitrate is set below, next to the audio codec.");
    bitrateSpin_->hide();
    auto *rateRow = new QHBoxLayout;
    rateRow->addWidget(rateCombo_, 1);
    rateRow->addWidget(bitrateSpin_);

    qualitySlider_ = new QSlider(Qt::Horizontal);
    qualitySlider_->setInvertedAppearance(true);   // right = higher quality
    qualitySlider_->setInvertedControls(true);
    qualityLabel_ = new QLabel;
    sliderRow_ = new QHBoxLayout;
    sliderRow_->addWidget(new QLabel("Smaller file"));
    sliderRow_->addWidget(qualitySlider_, 1);
    sliderRow_->addWidget(new QLabel("Higher quality"));

    resCombo_ = new QComboBox;
    fpsCombo_ = new QComboBox;
    acodecCombo_ = new QComboBox;
    abitrateCombo_ = new QComboBox;
    for (int kbps : audioBitrates())
        abitrateCombo_->addItem(QString("%1 kbps").arg(kbps), kbps);
    abitrateCombo_->setCurrentIndex(abitrateCombo_->findData(DEFAULT_AUDIO_KBPS));
    auto *audioRow = new QHBoxLayout;
    audioRow->addWidget(acodecCombo_, 1);
    audioRow->addWidget(abitrateCombo_);
    speedCombo_ = new QComboBox;
    for (const SpeedLevel &s : speedLevels())
        speedCombo_->addItem(s.label, s.key);
    keyintCombo_ = new QComboBox;
    for (const KeyframeInterval &k : keyframeIntervals())
        keyintCombo_->addItem(k.label, k.seconds);
    keyintCombo_->setToolTip(
        "Keyframes are the points where playback can start when you jump in the video.\n"
        "More frequent keyframes give faster, more precise seeking (useful for editing\n"
        "and streaming) but a bigger file, especially for videos with little motion.");

    settingsForm_ = new QFormLayout;
    settingsForm_->addRow("Format:", formatCombo_);
    settingsForm_->addRow("Video codec:", vcodecCombo_);
    settingsForm_->addRow("Hardware acceleration:", accelRow);
    settingsForm_->addRow("", encoderLabel_);
    settingsForm_->addRow("Rate control:", rateRow);
    settingsForm_->addRow("Quality:", sliderRow_);
    settingsForm_->addRow("", qualityLabel_);
    settingsForm_->addRow("Resolution:", resCombo_);
    settingsForm_->addRow("Frame rate:", fpsCombo_);
    settingsForm_->addRow("Audio:", audioRow);
    settingsForm_->addRow("Speed:", speedCombo_);
    settingsForm_->addRow("Keyframes:", keyintCombo_);
    settingsBox_ = new QGroupBox("Settings");
    settingsBox_->setLayout(settingsForm_);
    settingsBox_->setEnabled(false);

    // --- The size limit, in Automatic settings when the goal is the best quality within it ---
    limitSpin_ = new QDoubleSpinBox;
    limitSpin_->setRange(0.5, 4000);
    limitSpin_->setDecimals(1);
    limitSpin_->setValue(10);
    limitSpin_->setSuffix(" MB");
    connect(limitSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateLimitStatus);
    limitStatus_ = new QLabel;
    fitBtn_ = new QPushButton("Fit quality to limit");
    fitBtn_->setEnabled(false);
    fitBtn_->setToolTip("Keeps the settings below and changes only the quality (or the bitrate) to stay under the limit");
    connect(fitBtn_, &QPushButton::clicked, this, [this] { fitToLimit(); });
    limitRow_ = new QWidget;
    auto *limitRow = new QHBoxLayout(limitRow_);
    limitRow->setContentsMargins(0, 0, 0, 0);
    limitRow->addWidget(new QLabel("Limit:"));
    limitRow->addWidget(limitSpin_);
    limitRow->addWidget(limitStatus_, 1);
    limitRow->addWidget(fitBtn_);

    // --- Automatic settings: the analyzer (autosettings.cpp) ---
    goalCombo_ = new QComboBox;
    goalCombo_->addItem("Best quality within the limit", false);
    goalCombo_->addItem("Smallest file with no visible loss", true);
    goalCombo_->setItemData(0, "Analyzes the video and picks quality, resolution, frame rate and sound to stay under\n"
                               "the limit looking as good as possible: rather than letting the quality drop\n"
                               "below good, it lowers the frame rate or the resolution.", Qt::ToolTipRole);
    goalCombo_->setItemData(1, "Analyzes the video and picks the settings of the smallest file that still looks like\n"
                               "the original, whatever the limit.", Qt::ToolTipRole);
    analyzeBtn_ = new QPushButton("Analyze and apply");
    analyzeBtn_->setToolTip("Analyzes the video and sets everything below for the chosen goal");
    connect(analyzeBtn_, &QPushButton::clicked, this, &MainWindow::analyze);
    allowCodecCheck_ = new QCheckBox("It may also change format and codec");
    allowCodecCheck_->setToolTip("Lets it pick a more efficient codec (AV1, otherwise H.265) and a format for it:\n"
                                 "smaller files, but some older devices can't play them.");
    autoResult_ = new QLabel;
    autoResult_->setWordWrap(true);
    autoResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *goalRow = new QHBoxLayout;
    goalRow->addWidget(goalCombo_, 1);
    goalRow->addWidget(analyzeBtn_);
    autoBox_ = new QGroupBox("Automatic settings");
    auto *autoLayout = new QVBoxLayout(autoBox_);
    autoLayout->addLayout(goalRow);
    autoLayout->addWidget(limitRow_);
    autoLayout->addWidget(allowCodecCheck_);
    connect(goalCombo_, &QComboBox::currentIndexChanged, this, [this] { limitRow_->setVisible(limitGoal()); });
    autoLayout->addWidget(autoResult_);
    autoBox_->setEnabled(false);

    connect(formatCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onFormatChanged);
    for (QComboBox *combo : {vcodecCombo_, accelCombo_, deviceCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::updateEncoder);
    connect(acodecCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onAudioCodecChanged);
    connect(rateCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onRateModeChanged);
    connect(bitrateSpin_, &QSpinBox::valueChanged, this, &MainWindow::onSettingsChanged);
    connect(qualitySlider_, &QSlider::valueChanged, this, &MainWindow::onSettingsChanged);
    for (QComboBox *combo : {resCombo_, fpsCombo_, abitrateCombo_, speedCombo_, keyintCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::onSettingsChanged);

    // --- Size preview ---
    sizeLabel_ = new QLabel("—");
    QFont font = sizeLabel_->font();
    font.setPointSize(font.pointSize() + 10);
    font.setBold(true);
    sizeLabel_->setFont(font);
    spinner_ = new Spinner(QFontMetrics(font).height() * 3 / 4);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(12);
    sizeRow->addWidget(sizeLabel_);
    sizeRow->addWidget(spinner_, 0, Qt::AlignVCenter);
    sizeRow->addStretch();
    sizeDetail_ = new QLabel("Load a video to see the estimate.");
    sizeDetail_->setWordWrap(true);

    auto *previewBox = new QGroupBox("Estimated output size");
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->addLayout(sizeRow);
    previewLayout->addWidget(sizeDetail_);

    // --- Output and start ---
    outEdit_ = new QLineEdit;
    outBtn_ = new QPushButton("Browse…");
    connect(outBtn_, &QPushButton::clicked, this, &MainWindow::pickOutput);
    auto *outRow = new QHBoxLayout;
    outRow->addWidget(new QLabel("Save as:"));
    outRow->addWidget(outEdit_);
    outRow->addWidget(outBtn_);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    startBtn_ = new QPushButton("Export");
    startBtn_->setEnabled(false);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::start);
    cancelBtn_ = new QPushButton("Cancel");
    cancelBtn_->setEnabled(false);
    connect(cancelBtn_, &QPushButton::clicked, this, &MainWindow::cancel);
    auto *startRow = new QHBoxLayout;
    startRow->addWidget(startBtn_);
    startRow->addStretch();
    auto *progressRow = new QHBoxLayout;   // for compressing and for saving from the Editor
    progressRow->addWidget(progress_, 1);
    progressRow->addWidget(cancelBtn_);

    // --- Tabs: Compress, Editor ---
    editNotice_ = new QLabel;
    editNotice_->setWordWrap(true);
    editNotice_->setObjectName("editNotice");
    editNotice_->setStyleSheet("#editNotice { background: rgba(37, 99, 235, 0.10); border: 1px solid "
                               "rgba(37, 99, 235, 0.45); border-radius: 6px; padding: 6px; }");
    editNotice_->hide();
    auto *exportTab = new QWidget;
    auto *exportLayout = new QVBoxLayout(exportTab);
    exportLayout->addWidget(editNotice_);
    exportLayout->addWidget(autoBox_);
    exportLayout->addWidget(settingsBox_);
    exportLayout->addWidget(previewBox);
    exportLayout->addLayout(outRow);
    exportLayout->addLayout(startRow);
    exportLayout->addStretch();
    editor_ = new Editor(ffmpeg_, ffprobe_);
    connect(editor_, &Editor::editsChanged, this, &MainWindow::onEditsChanged);
    connect(editor_, &Editor::saveCopyRequested, this, &MainWindow::pickCopyOutput);
    tabs_ = new QTabWidget;
    tabs_->addTab(exportTab, "Export");
    tabs_->addTab(editor_, "Editor");
    connect(editNotice_, &QLabel::linkActivated, this, [this] { tabs_->setCurrentWidget(editor_); });

    // --- Result of the last compression, hard to miss (hidden until one ends) ---
    outcomeBar_ = new QFrame;
    outcomeBar_->setObjectName("outcomeBar");
    outcomeIcon_ = new QLabel;
    outcomeTitle_ = new QLabel;
    QFont titleFont = outcomeTitle_->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    outcomeTitle_->setFont(titleFont);
    outcomeText_ = new QLabel;
    outcomeText_->setWordWrap(true);
    outcomeText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    playBtn_ = new QPushButton("Play");
    playBtn_->setToolTip("Open the exported video with the default player");
    connect(playBtn_, &QPushButton::clicked, this, [this] {
        if (!currentOutput_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentOutput_));
    });
    openBtn_ = new QPushButton("Open folder");
    openBtn_->setEnabled(false);
    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::openFolder);
    auto *outcomeTexts = new QVBoxLayout;
    outcomeTexts->setSpacing(2);
    outcomeTexts->addWidget(outcomeTitle_);
    outcomeTexts->addWidget(outcomeText_);
    auto *outcomeRow = new QHBoxLayout(outcomeBar_);
    outcomeRow->setSpacing(12);
    outcomeRow->addWidget(outcomeIcon_, 0, Qt::AlignTop);
    outcomeRow->addLayout(outcomeTexts, 1);
    outcomeRow->addWidget(playBtn_, 0, Qt::AlignVCenter);
    outcomeRow->addWidget(openBtn_, 0, Qt::AlignVCenter);
    outcomeBar_->hide();

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setMaximumHeight(110);

    autoUpdateCheck_ = new QCheckBox("Check for updates automatically");
    autoUpdateCheck_->setChecked(appSettings()->value("updates/autoCheck", true).toBool());
    connect(autoUpdateCheck_, &QCheckBox::toggled, this,
            [](bool on) { appSettings()->setValue("updates/autoCheck", on); });
    checkNowBtn_ = new QPushButton("Check now");
    connect(checkNowBtn_, &QPushButton::clicked, this, [this] { checkForUpdates(true); });
    updateStatus_ = new QLabel;
    auto *updateRow = new QHBoxLayout;
    updateRow->addWidget(autoUpdateCheck_);
    updateRow->addWidget(checkNowBtn_);
    updateRow->addWidget(updateStatus_, 1);

    // everything in a scroll area: on short screens the window scrolls instead of squeezing what's in it
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->addWidget(updateBar_);
    layout->addWidget(srcBox);
    layout->addWidget(tabs_);
    layout->addLayout(progressRow);
    layout->addWidget(outcomeBar_);
    layout->addWidget(log_);
    layout->addStretch();   // spare height, if any, at the bottom rather than inside the boxes
    layout->addLayout(updateRow);
    scroll_ = new VerticalScrollArea;
    scroll_->setWidget(content);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll_);
    resize(std::max(640, scroll_->minimumSizeHint().width()), 0);
    fitToContent();

    // automatic check: a few seconds after startup, at most once a day
    const QDateTime lastCheck = appSettings()->value("updates/lastCheck").toDateTime();
    if (autoUpdateCheck_->isChecked()
        && (!lastCheck.isValid() || lastCheck.secsTo(QDateTime::currentDateTimeUtc()) > 24 * 3600))
        QTimer::singleShot(3000, this, [this] { checkForUpdates(false); });

    refreshCodecs();
    if (ffmpeg_.isEmpty() || ffprobe_.isEmpty()) {
        srcInfo_->setText("⚠ ffmpeg/ffprobe not found. Put them in the 'ffmpeg' folder "
                          "or install them (e.g. 'winget install Gyan.FFmpeg') and restart the program.");
        inEdit_->setEnabled(false);
        inBtn_->setEnabled(false);
    }
}

MainWindow::~MainWindow()
{
    // After this destructor, QWidget's own one still closes the window and deletes the children, and they
    // keep emitting signals: the path field emits editingFinished when it loses the focus, a running
    // FFmpeg process emits finished. Disconnect them all from this window first, since its members are
    // about to be destroyed, then stop the processes.
    for (QObject *child : findChildren<QObject *>())
        child->disconnect(this);
    estimator_->stop();
    updater_->cancelDownload();
    for (QProcess *proc : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(3000);
        }
    }
}

// ------------------------------------------------ loading the video
void MainWindow::pickInput()
{
    const QString path = QFileDialog::getOpenFileName(this, "Choose a video", QString(), videoFilter());
    if (!path.isEmpty())
        setInput(QDir::toNativeSeparators(path));
}

void MainWindow::onInputEdited()
{
    QString path = inEdit_->text().trimmed();
    if (path.size() >= 2 && path.startsWith('"') && path.endsWith('"'))
        path = path.mid(1, path.size() - 2);
    if (!path.isEmpty() && (!media_ || !samePath(path, media_->path)))
        setInput(path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && !encoding_)
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty())
        setInput(QDir::toNativeSeparators(urls.first().toLocalFile()));
}

void MainWindow::setInput(const QString &path)
{
    if (path.isEmpty() || ffprobe_.isEmpty() || encoding_)
        return;
    inEdit_->setText(path);
    estimator_->stop();
    QString error;
    const std::optional<MediaInfo> media = probe(ffprobe_, path, &error);
    if (!media) {
        media_.reset();
        spinner_->stop();
        outcomeBar_->hide();
        editor_->setMedia(std::nullopt);
        stopAuto(QString());
        autoResult_->clear();
        loadPreview();
        settingsBox_->setEnabled(false);
        autoBox_->setEnabled(false);
        startBtn_->setEnabled(false);
        fitBtn_->setEnabled(false);
        srcInfo_->setText("⚠ Could not read the video: " + error);
        return;
    }

    media_ = media;
    estBps_.reset();
    estCache_.clear();
    fit_.reset();
    const MediaInfo &m = *media_;
    const QFileInfo file(path);
    outEdit_->setText(QDir::toNativeSeparators(file.path() + "/" + file.completeBaseName() + "_export." + fmt().ext));
    srcInfo_->setText(QString("Duration %1 · %2×%3 · %4 fps · audio %5 · %6")
                          .arg(fmtTime(m.duration))
                          .arg(m.width)
                          .arg(m.height)
                          .arg(m.fps, 0, 'f', 0)
                          .arg(m.hasAudio ? "yes" : "no")
                          .arg(fmtMb(double(m.size))));

    // The options offered depend on the video: no upscaling and no higher fps
    const int shortSide = std::min(m.width, m.height);
    programmatic_ = true;
    resCombo_->clear();
    resCombo_->addItem(QString("Original (%1p)").arg(shortSide), 0);
    for (int r : resolutionSteps())
        if (r < shortSide)
            resCombo_->addItem(QString("%1p").arg(r), r);
    fpsCombo_->clear();
    fpsCombo_->addItem(QString("Original (%1 fps)").arg(m.fps, 0, 'f', 0), 0);
    for (int f : fpsSteps())
        if (f < m.fps - 0.5)
            fpsCombo_->addItem(QString("%1 fps").arg(f), f);
    programmatic_ = false;
    updateAudioWidgets();

    settingsBox_->setEnabled(true);
    autoBox_->setEnabled(true);
    stopAuto(QString());
    autoResult_->clear();
    openBtn_->setEnabled(false);
    outcomeBar_->hide();
    editor_->setMedia(media_);   // nothing edited
    loadPreview();
    onSettingsChanged();
}

// Shows a frame of the source video (at 10% of its length, so it is rarely a black intro frame).
void MainWindow::loadPreview()
{
    ++previewGen_;
    if (previewProc_) {
        previewProc_->disconnect(this);
        previewProc_->kill();
        previewProc_->waitForFinished(1000);
        previewProc_->deleteLater();
        previewProc_ = nullptr;
    }
    preview_->hide();
    preview_->clear();
    if (!media_ || ffmpeg_.isEmpty())
        return;

    // rendered at the screen's pixel density, so it stays sharp on scaled displays
    const qreal dpr = devicePixelRatioF();
    const int width = qRound(PREVIEW_WIDTH * dpr), height = qRound(PREVIEW_HEIGHT * dpr);
    const double at = std::min(media_->duration * 0.1, 10.0);
    const int gen = previewGen_;
    auto *proc = new QProcess(this);
    runInBackground(proc);
    previewProc_ = proc;
    connect(proc, &QProcess::finished, this, [this, proc, gen, dpr, at](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (proc == previewProc_)
            previewProc_ = nullptr;
        if (gen != previewGen_ || status != QProcess::NormalExit || code != 0)
            return;
        QPixmap frame;
        if (!frame.loadFromData(proc->readAllStandardOutput(), "PNG"))
            return;
        frame.setDevicePixelRatio(dpr);
        preview_->setPixmap(frame);
        preview_->setToolTip(QString("Frame at %1").arg(fmtTime(at)));
        preview_->show();
    });
    proc->start(ffmpeg_, {"-v", "error", "-ss", QString::number(at, 'f', 3), "-i", media_->path,
                          "-map", QString("0:%1").arg(media_->videoIndex), "-frames:v", "1",
                          "-vf", QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease").arg(width).arg(height),
                          "-f", "image2pipe", "-c:v", "png", "-"});
}

// ------------------------------------------------ settings
const Format &MainWindow::fmt() const
{
    return formatByExt(formatCombo_->currentData().toString());
}

int MainWindow::quality() const
{
    return qualitySlider_->value();
}

RateMode MainWindow::rateMode() const
{
    return RateMode(rateCombo_->currentData().toInt());
}

RateControl MainWindow::rate() const
{
    return {rateMode(), rateMode() == RateMode::Quality ? quality() : bitrateSpin_->value()};
}

int MainWindow::keyint() const
{
    return keyintCombo_->currentData().toInt();
}

const AudioCodec *MainWindow::audioCodec() const
{
    if (!media_ || !media_->hasAudio)
        return nullptr;
    return audioCodecByName(acodecCombo_->currentData().toString());
}

int MainWindow::audioKbps() const
{
    return audioCodec() ? abitrateCombo_->currentData().toInt() : 0;
}

// GPU encoders for a codec that work on this PC, best first.
std::vector<const VideoCodec *> MainWindow::hwEncoders(const QString &family) const
{
    std::vector<const VideoCodec *> list;
    for (const VideoCodec &c : videoCodecs())
        if (c.family == family && c.hardware() && encoders_->available().contains(c.encoder))
            list.push_back(&c);
    std::stable_sort(list.begin(), list.end(),
                     [](const VideoCodec *a, const VideoCodec *b) { return vendorRank(a->vendor) < vendorRank(b->vendor); });
    return list;
}

QString MainWindow::deviceName(const QString &vendor) const
{
    const Vendor *v = vendorByKey(vendor);
    return gpus_.value(vendor, v ? v->genericName : vendor);
}

// Offers only the codecs that the chosen format accepts and this PC can encode.
void MainWindow::refreshCodecs()
{
    const Format &f = fmt();
    const QSet<QString> &available = encoders_->available();
    const QString prevAudio = acodecCombo_->currentData().toString();
    programmatic_ = true;

    ComboItems familyItems;
    QStringList familyKeys;
    for (const Family &family : families()) {
        const bool usable = std::any_of(videoCodecs().begin(), videoCodecs().end(), [&](const VideoCodec &c) {
            return c.family == family.key && available.contains(c.encoder);
        });
        if (!f.video.contains(family.key) || !usable)
            continue;
        familyItems.append({family.note.isEmpty() ? family.name : family.name + " (" + family.note + ")", family.key});
        familyKeys << family.key;
    }
    fillCombo(vcodecCombo_, familyItems);
    for (int i = 0; i < familyKeys.size(); ++i) {
        QStringList gpus;
        for (const VideoCodec *c : hwEncoders(familyKeys[i]))
            gpus << deviceName(c->vendor);
        vcodecCombo_->setItemIcon(i, gpus.isEmpty() ? noIcon_ : hwIcon_);
        vcodecCombo_->setItemData(i, gpus.isEmpty() ? QString("Software encoding only (CPU)")
                                                    : "Hardware acceleration available on: " + gpus.join(", "),
                                  Qt::ToolTipRole);
    }

    ComboItems audioItems;
    for (const AudioCodec &a : audioCodecs())
        if (f.audio.contains(a.name) && available.contains(a.encoder))
            audioItems.append({a.label, a.name});
    audioItems.append({"No audio", QString()});
    fillCombo(acodecCombo_, audioItems);

    updateEncoder();
    if (acodecCombo_->currentData().toString() != prevAudio)
        onAudioCodecChanged();
    programmatic_ = false;
}

// Picks the encoder from codec, acceleration mode and device, and states clearly what will be used.
void MainWindow::updateEncoder()
{
    const QString family = vcodecCombo_->currentData().toString();
    const QString mode = accelCombo_->currentData().toString();
    std::vector<const VideoCodec *> hardware;
    if (mode != "off")
        hardware = hwEncoders(family);
    const VideoCodec *software = nullptr;
    for (const VideoCodec &c : videoCodecs())
        if (c.family == family && !c.hardware() && encoders_->available().contains(c.encoder)) {
            software = &c;
            break;
        }

    // the device menu always shows what will be used, but can be changed only in manual mode
    ComboItems devices;
    for (const VideoCodec *c : hardware)
        devices.append({deviceName(c->vendor) + " (" + c->label + ")", c->vendor});
    if (devices.isEmpty())
        devices.append({"CPU: " + cpu_, QString()});
    fillCombo(deviceCombo_, devices, mode == "manual");
    deviceCombo_->setEnabled(mode == "manual" && !hardware.empty());
    const QString vendor = deviceCombo_->currentData().toString();
    const VideoCodec *codec = software;
    for (const VideoCodec *c : hardware)
        if (c->vendor == vendor) {
            codec = c;
            break;
        }

    const QString name = familyName(family).toHtmlEscaped();
    QString text;
    if (!codec) {
        text = QString("⚠ No %1encoder for <b>%2</b> on this PC.").arg(mode == "off" ? "software " : "", name);
    } else {
        QString accel, device;
        if (codec->hardware()) {
            accel = "<b>yes</b>";
            device = "<b>" + deviceName(codec->vendor).toHtmlEscaped() + "</b>";
        } else {
            const QString why = mode == "off" ? QString() : " (no GPU in this PC can encode " + name + ")";
            accel = "<b>no</b>" + why;
            device = "<b>CPU</b> " + cpu_.toHtmlEscaped();
        }
        const QList<QPair<QString, QString>> rows = {{"Codec:", "<b>" + name + "</b>"},
                                                     {"Hardware acceleration:", accel},
                                                     {"Device:", device},
                                                     {"Encoder:", codec->label.toHtmlEscaped()}};
        text = "<table cellspacing='0' cellpadding='1'>";
        for (const auto &[label, value] : rows)
            text += "<tr><td>" + label + "&nbsp;&nbsp;</td><td>" + value + "</td></tr>";
        text += "</table>";
    }
    encoderLabel_->setText(text);
    setCodec(codec);
}

// Plain-text version of the encoder summary, for the log.
QString MainWindow::encoderSummary() const
{
    const QString device = codec_->hardware() ? deviceName(codec_->vendor) + ", hardware" : QString("CPU, software");
    return QString("%1 (%2 on %3)").arg(familyName(codec_->family), codec_->label, device);
}

void MainWindow::onFormatChanged()
{
    const QString out = outEdit_->text().trimmed();
    if (!out.isEmpty()) {
        const QString suffix = QFileInfo(out).suffix().toLower();
        if (std::any_of(formats().begin(), formats().end(), [&](const Format &f) { return f.ext == suffix; }))
            outEdit_->setText(withSuffix(out, fmt().ext));
    }
    refreshCodecs();
    onSettingsChanged();   // the format can change the video args (e.g. the HEVC tag)
}

void MainWindow::setCodec(const VideoCodec *codec)
{
    const bool changed = codec != codec_;
    codec_ = codec;
    if (changed && codec) {
        const QSignalBlocker blocker(qualitySlider_);
        qualitySlider_->setRange(codec->quality.best, codec->quality.worst);
        qualitySlider_->setValue(codec->quality.def);
        speedCombo_->setEnabled(!codec->speeds.isEmpty());
    }
    updateRateModes();   // also when the codec is the same: a GPU test may have just finished
    if (changed)
        onSettingsChanged();
}

// Offers the rate control modes the encoder has; a GPU's ones only if they passed the test on this PC.
void MainWindow::updateRateModes()
{
    auto *model = qobject_cast<QStandardItemModel *>(rateCombo_->model());
    for (int i = 0; i < rateCombo_->count(); ++i) {
        const bool ok = !codec_ || encoders_->supports(*codec_, RateMode(rateCombo_->itemData(i).toInt()));
        model->item(i)->setEnabled(ok);
        const QString tip = RATE_TIPS[i];
        rateCombo_->setItemData(i, ok ? tip : tip + "\n\nNot available with " + codec_->label
                                                   + (codec_->hardware() ? " on this PC." : "."),
                                Qt::ToolTipRole);
    }
    if (codec_ && !encoders_->supports(*codec_, rateMode())) {
        // e.g. CBR, then a codec whose encoder hasn't it: the closest mode it has (this updates the estimate)
        const bool vbr = rateMode() == RateMode::Cbr && encoders_->supports(*codec_, RateMode::Vbr);
        rateCombo_->setCurrentIndex(rateCombo_->findData(int(vbr ? RateMode::Vbr : RateMode::Quality)));
    }
}

void MainWindow::onRateModeChanged()
{
    const bool quality = rateMode() == RateMode::Quality;
    // the bitrate starts from the one of the current estimate, so that the size stays about the same
    if (!quality && estBps_) {
        const QSignalBlocker blocker(bitrateSpin_);
        bitrateSpin_->setValue(int(std::lround(*estBps_ * 8 / 1000 / 10)) * 10);
    }
    bitrateSpin_->setVisible(!quality);
    settingsForm_->setRowVisible(sliderRow_, quality);
    settingsForm_->setRowVisible(qualityLabel_, quality);
    fitBtn_->setText(quality ? "Fit quality to limit" : "Fit bitrate to limit");
    onSettingsChanged();
}

void MainWindow::onAudioCodecChanged()
{
    updateAudioWidgets();
    onSettingsChanged();
}

void MainWindow::updateAudioWidgets()
{
    const bool hasAudio = media_ && media_->hasAudio;
    acodecCombo_->setEnabled(hasAudio);
    abitrateCombo_->setEnabled(hasAudio && !acodecCombo_->currentData().toString().isEmpty());
}

QStringList MainWindow::videoFilters() const
{
    const MediaInfo &m = *media_;
    QStringList filters;
    const int target = resCombo_->currentData().toInt();
    if (target)   // scale the short side, so it also works with vertical videos
        filters << (m.width >= m.height ? QString("scale=-2:%1").arg(target) : QString("scale=%1:-2").arg(target));
    else if (m.width % 2 || m.height % 2)
        filters << "scale=trunc(iw/2)*2:trunc(ih/2)*2";   // 4:2:0 video needs even dimensions
    const int fps = fpsCombo_->currentData().toInt();
    if (fps)
        filters << QString("fps=%1").arg(fps);
    return filters;
}

QStringList MainWindow::videoCodecArgs() const
{
    const MediaInfo &m = *media_;
    const VideoCodec &codec = *codec_;
    const Format &f = fmt();
    // same keyframe spacing in seconds for every encoder (their defaults go from 0.4 to 10 s)
    const int fps = fpsCombo_->currentData().toInt();
    const double outFps = fps ? fps : (m.fps > 0 ? m.fps : 30.0);
    const int gopFrames = std::max(1, int(std::nearbyint(outFps * keyint())));
    QStringList args = codec.args(rate(), speedCombo_->currentData().toString(), std::pair{gopFrames, keyint()});
    if (codec.family == "hevc" && (f.ext == "mp4" || f.ext == "mov"))
        args << "-tag:v" << "hvc1";   // needed by Apple players
    else if (codec.family == "mpeg4" && f.ext == "avi")
        args << "-tag:v" << "XVID";   // recognized by old players
    return args;
}

QStringList MainWindow::videoArgs() const
{
    QStringList args{"-map", QString("0:%1").arg(media_->videoIndex)};
    const QStringList filters = videoFilters();
    if (!filters.isEmpty())
        args << "-vf" << filters.join(',');
    return args + videoCodecArgs();
}

// More than one piece: one input each (decoding only that part of the source), joined by the concat filter,
// then the video's own filters.
QStringList MainWindow::concatArgs(int inputs, const AudioCodec *acodec, int kbps) const
{
    QString graph;
    for (int i = 0; i < inputs; ++i) {
        graph += QString("[%1:%2]").arg(i).arg(media_->videoIndex);
        if (acodec)
            graph += QString("[%1:a:0]").arg(i);
    }
    graph += QString("concat=n=%1:v=1:a=%2[v]").arg(inputs).arg(acodec ? 1 : 0);
    if (acodec)
        graph += "[a]";
    QString video = "[v]", audio = "[a]";
    if (const QStringList filters = videoFilters(); !filters.isEmpty()) {
        graph += ";[v]" + filters.join(',') + "[vf]";
        video = "[vf]";
    }
    if (acodec && !acodec->filter.isEmpty()) {   // -af can't be used on the output of a filter graph
        graph += ";[a]" + acodec->filter + "[af]";
        audio = "[af]";
    }
    QStringList args{"-filter_complex", graph, "-map", video};
    args += videoCodecArgs();
    args += acodec ? QStringList{"-map", audio} + acodec->args(kbps, false) : QStringList{"-an"};
    return args;
}

void MainWindow::updateQualityLabel()
{
    if (!codec_) {
        qualityLabel_->clear();
        return;
    }
    qualityLabel_->setText(QString("%1 quality (%2 %3)")
                               .arg(capitalized(qualityName(quality(), codec_->quality)), codec_->qName)
                               .arg(codec_->shownQuality(quality())));
}

void MainWindow::onSettingsChanged()
{
    if (!programmatic_) {   // a manual change interrupts the search, and the analyzer
        fit_.reset();
        stopAuto("Stopped: a setting was changed by hand.");
    }
    updateQualityLabel();
    if (!media_ || encoding_)
        return;
    estimator_->stop();
    estTimer_->stop();
    fitBtn_->setEnabled(false);
    startBtn_->setEnabled(codec_ != nullptr);
    if (!codec_) {
        estBps_.reset();
        spinner_->stop();
        sizeLabel_->setText("—");
        sizeDetail_->setText("⚠ No video encoder available for this format.");
        updateLimitStatus();
        return;
    }
    if (rateMode() != RateMode::Quality) {
        // the size follows from the bitrate: nothing to encode
        pendingKey_.reset();
        onEstimateDone(bitrateSpin_->value() * 1000.0 / 8, false);
        return;
    }
    sizeLabel_->setStyleSheet("color: gray;");
    sizeDetail_->setText("Updating estimate…");
    spinner_->start();
    estTimer_->start();
}

// ------------------------------------------------ the edited video
std::vector<Piece> MainWindow::outputPieces() const
{
    return media_ ? editor_->edits().joined() : std::vector<Piece>{};
}

double MainWindow::outputDuration() const
{
    return media_ ? editor_->edits().duration() : 0;
}

void MainWindow::onEditsChanged()
{
    if (media_ && editor_->edited()) {
        const EditList &edits = editor_->edits();
        editNotice_->setText(QString("✂ <b>Edited in the Editor</b>: %1 piece%2, %3 of %4. Only what is left "
                                     "will be exported. <a href=\"editor\">Show the Editor</a>")
                                 .arg(edits.pieces().size())
                                 .arg(edits.pieces().size() == 1 ? "" : "s")
                                 .arg(fmtPrecise(edits.duration()), fmtPrecise(media_->duration)));
        editNotice_->show();
    } else {
        editNotice_->hide();
    }
    onSettingsChanged();   // the size depends on what is left
}

// ------------------------------------------------ size estimate
// Where the samples are taken (start, length in the source): spread over what will be in the file. A short
// result is encoded whole; exact if it is one piece.
QList<QPair<double, double>> MainWindow::estimateSamples(bool *exact) const
{
    const std::vector<Piece> pieces = outputPieces();
    const double total = outputDuration();
    QList<QPair<double, double>> samples;
    *exact = total <= WHOLE_IF_SHORTER && pieces.size() == 1;
    if (total <= WHOLE_IF_SHORTER) {
        for (const Piece &p : pieces)
            samples.append({p.start, p.length()});
        return samples;
    }
    EditList output;
    output.setPieces(pieces);
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        const auto [index, source] = output.locate(total * (i + 0.5) / SAMPLE_COUNT);
        const Piece &piece = pieces[size_t(index)];
        const double length = std::min(SAMPLE_LEN, piece.length());
        samples.append({std::clamp(source - length / 2, piece.start, piece.end - length), length});
    }
    return samples;
}

// what an estimate depends on: the video settings and where the samples are
QString MainWindow::estimateKey() const
{
    bool exact = false;
    QStringList parts = videoArgs();
    for (const auto &[start, length] : estimateSamples(&exact))
        parts << number(start) << number(length);
    return parts.join(QChar(0x1F));
}

void MainWindow::runEstimate()
{
    if (!media_ || encoding_ || !codec_ || rateMode() != RateMode::Quality)
        return;
    bool exact = false;
    const QList<QPair<double, double>> segments = estimateSamples(&exact);
    const QStringList args = videoArgs();
    const QString key = estimateKey();
    if (estCache_.contains(key)) {
        pendingKey_.reset();
        const auto [bps, cachedExact] = estCache_.value(key);
        onEstimateDone(bps, cachedExact);
        return;
    }
    pendingKey_ = key;
    sizeDetail_->setText(fit_ ? "Searching for the best quality under the limit…"
                              : "Estimating (encoding a few short samples)…");
    estimator_->start(media_->path, segments, args, fmt().ext, keyint(), exact);
}

double MainWindow::estimatedBytes() const
{
    return (estBps_.value_or(0) + audioKbps() * 1000.0 / 8) * outputDuration() * CONTAINER_OVERHEAD;
}

void MainWindow::onEstimateDone(double bps, bool exact)
{
    if (pendingKey_) {
        estCache_.insert(*pendingKey_, {bps, exact});
        pendingKey_.reset();
    }
    estBps_ = bps;
    estExact_ = exact;
    spinner_->stop();
    const double total = estimatedBytes();
    const QString share = QString::number(total / double(media_->size) * 100, 'f', 0);
    sizeLabel_->setStyleSheet(QString());
    sizeLabel_->setText((exact ? "" : "≈ ") + fmtMb(total));
    if (rateMode() == RateMode::Quality) {
        const QString accuracy = exact ? "exact estimate" : "sample-based estimate, roughly ±10%";
        sizeDetail_->setText(QString("Video ≈ %1 kbps · audio %2 kbps · %3% of the original (%4)")
                                 .arg(bps * 8 / 1000, 0, 'f', 0)
                                 .arg(audioKbps())
                                 .arg(share, accuracy));
    } else {
        const bool variable = rateMode() == RateMode::Vbr;
        sizeDetail_->setText(QString("Video %1 kbps %2 · audio %3 kbps · %4% of the original (from the bitrate%5)")
                                 .arg(bitrateSpin_->value())
                                 .arg(variable ? "on average" : "constant")
                                 .arg(audioKbps())
                                 .arg(share, variable ? "; simple videos come out smaller" : ""));
    }
    updateLimitStatus();
    if (fit_)
        fitStep();
    else if (auto_ && auto_->fitting)
        finishAuto();
    else if (auto_)
        autoStep();
    fitBtn_->setEnabled(!fit_);
}

void MainWindow::onEstimateFailed(const QString &message)
{
    fit_.reset();
    stopAuto("⚠ The analysis stopped: the estimate failed.");
    pendingKey_.reset();
    estBps_.reset();
    spinner_->stop();
    sizeLabel_->setStyleSheet(QString());
    sizeLabel_->setText("—");
    sizeDetail_->setText("⚠ Estimate failed: " + message);
    updateLimitStatus();
}

// The size limit counts only with this goal: otherwise it isn't shown.
bool MainWindow::limitGoal() const
{
    return !goalCombo_->currentData().toBool();
}

void MainWindow::updateLimitStatus()
{
    if (!media_ || !estBps_) {
        limitStatus_->clear();
        return;
    }
    if (estimatedBytes() <= limitSpin_->value() * MB) {
        limitStatus_->setText("✔ Under the limit");
        limitStatus_->setStyleSheet("color: #2e7d32; font-weight: bold;");
    } else {
        limitStatus_->setText("✖ Over the limit");
        limitStatus_->setStyleSheet("color: #c62828; font-weight: bold;");
    }
}

// ------------------------------------------------ fit to limit
// Searches for the best quality value that stays under the limit; with a bitrate, computes it.
void MainWindow::fitToLimit(std::optional<int> bestAllowed)
{
    if (!media_ || !estBps_ || !codec_)
        return;
    // GPU encoders can end up to ~10% above an average bitrate (CBR and software encoders: a few %)
    const double margin = rateMode() == RateMode::Vbr && codec_->hardware() ? GPU_VBR_LIMIT_MARGIN : LIMIT_MARGIN;
    const double target = limitSpin_->value() * MB * margin;
    const double audioBytes = audioKbps() * 1000.0 / 8 * outputDuration() * CONTAINER_OVERHEAD;
    if (audioBytes >= target) {
        showMessage(QMessageBox::Information, "Limit too low",
                    "The audio alone already exceeds the limit: lower or remove it.");
        return;
    }
    if (rateMode() != RateMode::Quality) {
        const double videoBytesPerSecond = (target - audioBytes) / (outputDuration() * CONTAINER_OVERHEAD);
        const int kbps = int(videoBytesPerSecond * 8 / 1000 / 10) * 10;
        if (kbps < MIN_VIDEO_KBPS) {
            showMessage(QMessageBox::Information, "Limit not reachable",
                        QString("The video would need less than %1 kbps.\n"
                                "Try reducing the resolution, frame rate or audio.").arg(MIN_VIDEO_KBPS));
            return;
        }
        setBitrate(std::min(kbps, MAX_VIDEO_KBPS));
        return;
    }
    const int best = std::max(bestAllowed.value_or(codec_->quality.best), codec_->quality.best);
    fit_ = Fit{best, codec_->quality.worst, std::nullopt, {}, 8, target};
    fitBtn_->setEnabled(false);
    fitStep();
}

void MainWindow::setQuality(int value)
{
    programmatic_ = true;
    qualitySlider_->setValue(value);
    programmatic_ = false;
}

void MainWindow::setBitrate(int kbps)
{
    programmatic_ = true;
    bitrateSpin_->setValue(kbps);
    programmatic_ = false;
}

void MainWindow::fitStep()
{
    Fit &f = *fit_;
    const int worst = codec_->quality.worst;
    const int q = quality();
    const double size = estimatedBytes();
    f.points.emplace_back(q, size);
    if (size <= f.target) {
        f.best = f.best ? std::min(*f.best, q) : q;
        f.hi = std::min(f.hi, q - 1);   // try a higher quality
    } else {
        f.lo = std::max(f.lo, q + 1);   // needs more compression
    }
    --f.left;

    if (f.lo > f.hi || f.left <= 0) {
        const Fit result = f;
        fit_.reset();
        if (result.best) {
            if (*result.best != q)
                setQuality(*result.best);
        } else if (result.lo > worst) {
            showMessage(QMessageBox::Information, "Limit not reachable",
                        "Even at the lowest quality the file exceeds the limit.\n"
                        "Try reducing the resolution, frame rate or audio.");
        } else {
            setQuality(std::min(result.lo, worst));
        }
        // the analyzer ends with this search: when the last estimate is ready, or now if there is none
        if (auto_ && auto_->fitting && !estTimer_->isActive() && !estimator_->running())
            finishAuto();
        return;
    }

    // guess the next value: size drops roughly exponentially as the quality value grows
    double slope = -std::log(2.0) / codec_->halving;
    if (f.points.size() >= 2) {
        const auto [q1, s1] = f.points[f.points.size() - 2];
        const auto [q2, s2] = f.points.back();
        if (q1 != q2 && s1 > 0 && s2 > 0) {
            const double measured = (std::log(s2) - std::log(s1)) / (q2 - q1);
            if (measured < 0)
                slope = measured;
        }
    }
    const int next = int(std::nearbyint(q + (std::log(f.target) - std::log(size)) / slope));
    setQuality(std::max(f.lo, std::min(f.hi, next)));
}

// ------------------------------------------------ output
void MainWindow::pickOutput()
{
    const Format &f = fmt();
    QString path = QFileDialog::getSaveFileName(this, "Save as", outEdit_->text(), QString("%1 (*.%2)").arg(f.label, f.ext));
    if (path.isEmpty())
        return;
    if (!path.endsWith("." + f.ext, Qt::CaseInsensitive))
        path += "." + f.ext;
    outEdit_->setText(QDir::toNativeSeparators(path));
    confirmedOutput_ = outEdit_->text();   // the dialog already asked whether to replace it
}

void MainWindow::openFolder()
{
    if (!currentOutput_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(currentOutput_).absolutePath()));
}

// ------------------------------------------------ compression
void MainWindow::start()
{
    if (!media_ || !codec_)
        return;
    const Format &f = fmt();
    const QString input = media_->path;
    QString out = outEdit_->text().trimmed();
    if (out.size() >= 2 && out.startsWith('"') && out.endsWith('"'))
        out = out.mid(1, out.size() - 2);
    if (out.isEmpty()) {
        showMessage(QMessageBox::Warning, "Error", "Choose where to save the file.");
        return;
    }
    if (QFileInfo(out).suffix().toLower() != f.ext) {
        out = withSuffix(out, f.ext);
        outEdit_->setText(out);
    }
    if (samePath(input, out)) {
        showMessage(QMessageBox::Warning, "Error", "The output file must be different from the input file.");
        return;
    }
    // an existing file is replaced only if the user says so (once, when the Save as dialog already asked)
    const bool confirmed = samePath(out, std::exchange(confirmedOutput_, QString()));
    if (QFileInfo::exists(out) && !confirmed
        && !askQuestion("Replace the file?", QString("“%1” already exists in %2.\n\nReplace it with the export?")
                                                 .arg(QFileInfo(out).fileName(),
                                                      QDir::toNativeSeparators(QFileInfo(out).absolutePath()))))
        return;

    resumeEstimate_ = spinner_->isSpinning();   // estimated again when the compression ends
    estTimer_->stop();
    estimator_->stop();
    spinner_->stop();
    const AudioCodec *acodec = audioCodec();
    const int kbps = audioKbps();
    // the edited video: each piece is an input that decodes only that part of the source
    const std::vector<Piece> pieces = outputPieces();
    QStringList args{"-y", "-hide_banner", "-nostats", "-progress", "pipe:1"};
    if (!editor_->edited()) {
        args << "-i" << input;
    } else {
        for (const Piece &piece : pieces)
            args << "-ss" << number(piece.start) << "-t" << number(piece.length()) << "-i" << input;
    }
    if (pieces.size() <= 1) {
        args += videoArgs();
        args += acodec ? QStringList{"-map", "0:a:0"} + acodec->args(kbps) : QStringList{"-an"};
    } else {
        args += concatArgs(int(pieces.size()), acodec, kbps);
    }
    args += seekIndexArgs(f.ext);
    args << out;

    log_->clear();
    const QString audio = acodec ? QString("%1 %2 kbps").arg(acodec->label).arg(kbps) : QString("no audio");
    const QString speed = codec_->speeds.isEmpty() ? QString() : " · " + speedCombo_->currentText().toLower();
    const QString rateText = rateMode() == RateMode::Quality
                                 ? QString("%1 %2").arg(codec_->qName).arg(codec_->shownQuality(quality()))
                                 : QString("%1 %2 kbps").arg(rateModeName(rateMode())).arg(bitrateSpin_->value());
    log_->appendPlainText(QString("Exporting: %1 · %2 · %3 · %4 · %5 · %6%7 · keyframes every %8 s")
                              .arg(f.label, encoderSummary(), rateText, resCombo_->currentText(),
                                   fpsCombo_->currentText(), audio, speed)
                              .arg(keyint()));
    if (editor_->edited())
        log_->appendPlainText(QString("Edited: %1 pieces, %2").arg(pieces.size()).arg(fmtPrecise(outputDuration())));
    runFfmpeg(args, out, outputDuration(), Job::Export);
}

void MainWindow::pickCopyOutput()
{
    if (!media_ || encoding_)
        return;
    const QFileInfo source(media_->path);
    const QString suggested = QDir(source.path()).filePath(source.completeBaseName() + "_edited." + source.suffix());
    const QString out = QFileDialog::getSaveFileName(this, "Save without re-encoding", suggested,
                                                     QString("%1 (*.%2)").arg(source.suffix().toUpper(), source.suffix()));
    if (!out.isEmpty())
        saveCopy(QDir::toNativeSeparators(out));
}

// Copies the edited video without re-encoding: FFmpeg's concat demuxer with an in and out point for each
// piece. Decoding can only begin on a keyframe, so each piece starts at the keyframe at or before its start.
void MainWindow::saveCopy(const QString &out)
{
    if (!media_ || encoding_ || !editor_->keyframesReady())
        return;
    if (samePath(media_->path, out)) {
        showMessage(QMessageBox::Warning, "Error", "The output file must be different from the input file.");
        return;
    }
    const std::vector<double> &keyframes = editor_->keyframes();
    const double frame = 1 / std::max(1.0, media_->fps);
    std::vector<Piece> pieces = outputPieces();
    int moved = 0;
    double earliest = 0;
    for (Piece &piece : pieces) {
        const auto after = std::upper_bound(keyframes.begin(), keyframes.end(), piece.start + 1e-3);
        const double keyframe = after == keyframes.begin() ? 0.0 : *std::prev(after);
        if (piece.start - keyframe > frame / 2) {
            ++moved;
            earliest = std::max(earliest, piece.start - keyframe);
        }
        piece.start = keyframe;
    }
    if (moved && !askQuestion("Save without re-encoding",
                              QString("Without re-encoding, a piece can only start on a keyframe: %1 of the %2 "
                                      "pieces will start earlier, by up to %3 s.\n\n"
                                      "To cut exactly where you chose, use Export. Save anyway?")
                                  .arg(moved)
                                  .arg(pieces.size())
                                  .arg(earliest, 0, 'f', 2)))
        return;

    copyDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/video_copy_XXXXXX");
    const QString list = copyDir_->filePath("pieces.ffconcat");
    QString text = "ffconcat version 1.0\n";
    const QString file = "file '" + QDir::fromNativeSeparators(media_->path).replace("'", "'\\''") + "'\n";
    // The demuxer works in the file's own timestamps and stops a piece at the out point in decoding order:
    // with B-frames the first frame after the piece is decoded before its last frames are, so it stops at
    // that frame's decoding time. The duration keeps the following pieces in place.
    double duration = 0;
    for (const Piece &piece : pieces) {
        text += file;
        text += QString("inpoint %1\n").arg(number(piece.start + media_->startTime));
        if (piece.end < media_->duration - frame / 2)
            text += QString("outpoint %1\n").arg(number(editor_->copyStop(piece.end) + media_->startTime));
        text += QString("duration %1\n").arg(number(piece.length()));
        duration += piece.length();
    }
    QFile listFile(list);
    if (!listFile.open(QIODevice::WriteOnly) || listFile.write(text.toUtf8()) < 0) {
        showMessage(QMessageBox::Critical, "Error", "Could not write a temporary file: " + listFile.errorString());
        return;
    }
    listFile.close();

    QStringList args{"-y", "-hide_banner", "-nostats", "-progress", "pipe:1", "-f", "concat", "-safe", "0",
                     "-i", list, "-map", QString("0:%1").arg(media_->videoIndex)};
    if (media_->hasAudio)
        args << "-map" << "0:a:0";
    args << "-c" << "copy" << "-avoid_negative_ts" << "make_zero";
    args += seekIndexArgs(QFileInfo(out).suffix().toLower());
    args << out;
    log_->clear();
    log_->appendPlainText(QString("Saving without re-encoding: %1 piece%2, %3")
                              .arg(pieces.size())
                              .arg(pieces.size() == 1 ? "" : "s")
                              .arg(fmtPrecise(duration)));
    runFfmpeg(args, out, duration, Job::Copy);
}

void MainWindow::runFfmpeg(const QStringList &args, const QString &out, double duration, Job job)
{
    job_ = job;
    jobDuration_ = duration;
    cancelled_ = false;
    currentOutput_ = out;
    outBuf_.clear();
    stderrTail_.clear();
    progress_->setValue(0);
    outcomeBar_->hide();
    encodeClock_.start();
    setRunning(true);
    if (proc_)
        proc_->deleteLater();
    proc_ = new QProcess(this);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &MainWindow::onStdout);
    connect(proc_, &QProcess::readyReadStandardError, this, &MainWindow::onStderr);
    connect(proc_, &QProcess::finished, this, &MainWindow::onFinished);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onFinished(-1, QProcess::CrashExit);
    });
    proc_->start(ffmpeg_, args);
}

void MainWindow::onStdout()
{
    outBuf_ += QString::fromUtf8(proc_->readAllStandardOutput());
    QStringList lines = outBuf_.split('\n');
    outBuf_ = lines.takeLast();
    for (QString line : lines) {
        line = line.trimmed();
        if (!line.startsWith("out_time_us=") && !line.startsWith("out_time_ms="))
            continue;
        bool ok = false;
        const qint64 value = line.section('=', 1).toLongLong(&ok);
        if (ok && value >= 0) {
            const double frac = std::min(value / 1e6 / std::max(jobDuration_, 0.001), 1.0);
            progress_->setValue(int(frac * 100));
        }
    }
}

void MainWindow::onStderr()
{
    stderrTail_ += QString::fromUtf8(proc_->readAllStandardError()).split(QRegularExpression("\r?\n|\r"),
                                                                          Qt::SkipEmptyParts);
    if (stderrTail_.size() > 30)
        stderrTail_ = stderrTail_.mid(stderrTail_.size() - 30);
}

void MainWindow::onFinished(int code, QProcess::ExitStatus status)
{
    if (cancelled_)
        return;
    setRunning(false);
    if (status != QProcess::NormalExit || code != 0) {
        log_->appendPlainText(stderrTail_.join('\n'));
        const QString reason = stderrTail_.isEmpty() ? QString("FFmpeg could not be started.")
                                                     : stderrTail_.last().trimmed();
        showOutcome(Outcome::Failed, reason.toHtmlEscaped() + "<br>The log below has the details.",
                    job_ == Job::Copy ? "Saving failed" : "");
        return;
    }
    progress_->setValue(100);
    const double size = double(QFileInfo(currentOutput_).size());
    QString msg = QString("Done! %1 → %2").arg(fmtMb(size), currentOutput_);
    if (estBps_ && job_ == Job::Export)
        msg += QString("\n(estimate was %1)").arg(fmtMb(estimatedBytes()));
    log_->appendPlainText(msg);
    openBtn_->setEnabled(true);

    const double original = double(media_->size);
    const QString change = size <= original
                               ? QString("%1% smaller than the original").arg((1 - size / original) * 100, 0, 'f', 0)
                               : QString("%1% bigger than the original").arg((size / original - 1) * 100, 0, 'f', 0);
    // (non-breaking spaces: "3.5 MB" and "took 0:12" are never split across two lines)
    const QString what = QString("<b>%1</b> · %2, %3 · took&nbsp;%4")
                             .arg(QFileInfo(currentOutput_).fileName().toHtmlEscaped(),
                                  fmtMb(size).replace(' ', "&nbsp;"), change,
                                  fmtTime(encodeClock_.elapsed() / 1000.0));
    if (job_ == Job::Copy) {
        showOutcome(Outcome::Done, what, "Saved without re-encoding");
    } else if (limitGoal() && size > limitSpin_->value() * MB) {
        const bool quality = rateMode() == RateMode::Quality;
        showOutcome(Outcome::OverLimit,
                    what + QString("<br>It is over the %1 MB limit: use “%2” or lower the %3 a bit and try again.")
                               .arg(limitSpin_->value(), 0, 'f', 1)
                               .arg(fitBtn_->text(), quality ? "quality" : "bitrate"));
    } else {
        showOutcome(Outcome::Done, what);
    }
}

// Makes the window as tall as its content, if the screen has room (it never shrinks it): otherwise the
// content scrolls.
void MainWindow::fitToContent()
{
    QWidget *content = scroll_->widget();
    content->layout()->activate();
    int needed = content->heightForWidth(width());   // texts that wrap need more lines in a narrow window
    if (needed < 0)
        needed = content->sizeHint().height();
    const QRect room = screen()->availableGeometry();
    const int titleBar = isVisible() ? frameGeometry().height() - height() : 40;
    const int wanted = std::min(needed, room.height() - titleBar);
    if (wanted <= height())
        return;
    resize(width(), wanted);
    if (isVisible() && frameGeometry().bottom() > room.bottom())   // keep it all on the screen
        move(x(), std::max(room.top(), room.bottom() - frameGeometry().height()));
}

// Shows how the compression ended in a colored box under the buttons, and draws attention to the window
// (taskbar button on Windows, Dock icon on macOS) if it is in the background.
void MainWindow::showOutcome(Outcome outcome, const QString &text, const QString &title)
{
    static const struct {
        const char *title;
        const char *color;
        const char *rgb;
        char symbol;
    } looks[] = {
        {"Export complete", "#2e7d32", "46, 125, 50", 'v'},
        {"Export complete, but over the limit", "#d07a00", "232, 137, 12", '!'},
        {"Export failed", "#c62828", "198, 40, 40", 'x'},
    };
    const auto &look = looks[int(outcome)];
    outcomeBar_->setStyleSheet(QString("#outcomeBar { background: rgba(%1, 0.13); border: 1px solid rgba(%1, 0.7); "
                                       "border-radius: 6px; }")
                                   .arg(look.rgb));
    const int size = outcomeTitle_->fontMetrics().height() * 2;
    outcomeIcon_->setPixmap(badge(QColor(look.color), look.symbol, size, devicePixelRatioF()));
    outcomeTitle_->setText(title.isEmpty() ? QString(look.title) : title);
    outcomeText_->setText(text);
    playBtn_->setVisible(outcome != Outcome::Failed);
    openBtn_->setVisible(outcome != Outcome::Failed);
    outcomeBar_->show();
    QTimer::singleShot(0, this, [this] {   // once the box has its size
        fitToContent();
        scroll_->ensureWidgetVisible(outcomeBar_);
    });
    QApplication::alert(this);
}

void MainWindow::cancel()
{
    cancelled_ = true;
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->kill();
        proc_->waitForFinished(3000);
    }
    if (!currentOutput_.isEmpty() && QFileInfo::exists(currentOutput_))
        QFile::remove(currentOutput_);
    progress_->setValue(0);
    log_->appendPlainText("Cancelled.");
    setRunning(false);
}

void MainWindow::setRunning(bool running)
{
    encoding_ = running;
    startBtn_->setEnabled(!running);
    cancelBtn_->setEnabled(running);
    fitBtn_->setEnabled(!running && estBps_ && !fit_);
    if (running)
        openBtn_->setEnabled(false);
    for (QWidget *w : std::initializer_list<QWidget *>{inEdit_, inBtn_, outEdit_, outBtn_, settingsBox_, autoBox_,
                                                       limitSpin_})
        w->setEnabled(!running);
    editor_->setBusy(running);
    if (!running && resumeEstimate_) {   // the compression interrupted an estimate: finish it
        resumeEstimate_ = false;
        onSettingsChanged();
    }
}

// ------------------------------------------------ updates
void MainWindow::checkForUpdates(bool manual)
{
    manualUpdateCheck_ = manual;
    if (manual) {
        checkNowBtn_->setEnabled(false);
        updateStatus_->setText("Checking…");
    }
    updater_->check();
}

// As text (2026-10-08T09:30:00Z), readable in the portable version's FrameStudio.ini; it's read back
// as a date like the values that older versions saved as dates.
void MainWindow::saveLastUpdateCheck()
{
    appSettings()->setValue("updates/lastCheck", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

void MainWindow::onUpdateAvailable(const UpdateInfo &info)
{
    saveLastUpdateCheck();
    pendingUpdate_ = info;
    updateText_->setText(QString("<b>FrameStudio %1</b> is available (you have %2).")
                             .arg(info.version.toHtmlEscaped(), QCoreApplication::applicationVersion()));
    updateBtn_->setText(canSelfUpdate() ? "Update now" : "Download");
    updateBtn_->setEnabled(true);
    updateBar_->show();
    QTimer::singleShot(0, this, [this] {
        fitToContent();
        scroll_->ensureWidgetVisible(updateBar_);
    });
    if (manualUpdateCheck_)
        updateStatus_->clear();
    manualUpdateCheck_ = false;
    checkNowBtn_->setEnabled(true);
}

// The app updates itself only when it was installed with the Windows installer; otherwise
// (portable zip, macOS, Linux) "Download" opens the release page.
bool MainWindow::canSelfUpdate() const
{
#ifdef Q_OS_WIN
    return pendingUpdate_ && !pendingUpdate_->installerUrl.isEmpty()
           && QFileInfo::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
#else
    return false;
#endif
}

void MainWindow::startUpdate()
{
    if (!pendingUpdate_)
        return;
    if (!canSelfUpdate()) {
        QDesktopServices::openUrl(pendingUpdate_->page);
        return;
    }
    if (encoding_) {
        showMessage(QMessageBox::Information, "Update", "Wait for the export to finish, then update.");
        return;
    }
    downloadingUpdate_ = true;
    updateBtn_->setEnabled(false);
    updateText_->setText(QString("Downloading FrameStudio %1…").arg(pendingUpdate_->version));
    updater_->download(*pendingUpdate_);
}

// Runs the new installer without its wizard (for the same user, or for all users, as before) and closes
// the app so that its files can be replaced; the installer starts the new version when it's done.
void MainWindow::installUpdate(const QString &installer)
{
    downloadingUpdate_ = false;
    const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString userData = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const bool forCurrentUser = appDir.startsWith(userData, Qt::CaseInsensitive);
    const QStringList args{"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS",
                           forCurrentUser ? "/CURRENTUSER" : "/ALLUSERS"};
    if (!QProcess::startDetached(installer, args)) {
        updateText_->setText("⚠ The installer could not be started.");
        updateBtn_->setEnabled(true);
        return;
    }
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    estimator_->stop();
    updater_->cancelDownload();
    if (previewProc_)
        previewProc_->kill();
    if (proc_ && proc_->state() != QProcess::NotRunning)
        cancel();
    event->accept();
}
