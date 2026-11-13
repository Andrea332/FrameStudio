// End-to-end tests: they drive the real window (offscreen) and check the files FFmpeg produces.
// Test videos are generated with FFmpeg at startup; GPU encoders are tested only if this PC has them.

#include "codecs.h"
#include "editlist.h"
#include "editor.h"
#include "encodercheck.h"
#include "mainwindow.h"
#include "media.h"
#include "settings.h"
#include "sizeestimator.h"
#include "spinner.h"
#include "timeline.h"
#include "updatechecker.h"
#include "videoview.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <memory>

class TestVideoCompressor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void probesTrickyInputs();
    void showsVersionAndSourcePreview();
    void offersOnlyCompatibleCodecs();
    void statesEncoderClearly();
    void encodesEveryFormatCodecAndDevice();
    void scalesPortraitPhoneVideos();
    void usesVideoNotCoverArt();
    void placesKeyframesWithClosedGop();
    void writesSeekIndexAtStart();
    void estimatesAccurately_data();
    void estimatesAccurately();
    void spinsWhileEstimating();
    void fitsToLimit();
    void encodesAtTheChosenBitrate();
    void fitsBitrateToLimit();
    void analyzesAndSetsTheBest();
    void showsTheOutcomeClearly();
    void scrollsOnShortScreens();
    void notifiesAboutUpdates();
    void keepsPortableSettingsNextToTheApp();
    void editsAListOfPieces();
    void editsWithKeysAndMouse();
    void playsTheEditedVideo();
    void compressesTheEditedVideo();
    void estimatesTheEditedVideo();
    void editsPhoneVideos();
    void savesWithoutReencoding();
    void rendersWindow();
    // last: it kills FFmpeg in the middle of GPU encodes, and on the macOS CI machine (a virtual Mac) the
    // VideoToolbox encodes started after that sometimes never ended
    void survivesClosingWhileBusy();

private:
    bool usable(const QString &family) const;
    QString path(const QString &name) const { return dir_.filePath(name); }
    QByteArray run(const QString &program, const QStringList &args, int timeoutMs = 300000);
    void select(QComboBox *combo, const QVariant &data);
    bool settled() const;
    QString compress(const QString &input, const QString &format, const QString &family, const QString &mode,
                     const QString &vendor = {}, const QString &audio = {}, int keyint = 10, int resolution = 0);
    QJsonArray streams(const QString &file);
    QList<double> keyframeTimes(const QString &file);
    QList<int> nalTypes(const QString &file);
    QStringList hardwareVendors(const QString &family) const;
    QString colorsAt(const QString &file, const QList<double> &times);
    double durationOf(const QString &file);

    QTemporaryDir dir_;
    QString ffmpeg_;
    QString ffprobe_;
    std::unique_ptr<MainWindow> win_;
    QStringList messages_;
};

// ------------------------------------------------ helpers
QByteArray TestVideoCompressor::run(const QString &program, const QStringList &args, int timeoutMs)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(timeoutMs) || proc.exitCode() != 0)
        qWarning().noquote() << program << args.join(' ') << "failed:" << proc.readAllStandardError().right(500);
    return proc.readAllStandardOutput() + proc.readAllStandardError();
}

void TestVideoCompressor::select(QComboBox *combo, const QVariant &data)
{
    const int index = combo->findData(data);
    QVERIFY2(index >= 0, qPrintable(QString("%1 not offered").arg(data.toString())));
    combo->setCurrentIndex(index);
}

bool TestVideoCompressor::settled() const
{
    const MainWindow &w = *win_;
    if (w.estTimer_->isActive() || w.estimator_->running())
        return false;
    if (w.rateMode() != RateMode::Quality)
        return w.estBps_.has_value();   // computed from the bitrate, right away
    return w.sizeDetail_->text().startsWith("⚠") || w.estCache_.contains(w.estimateKey());
}

QString TestVideoCompressor::compress(const QString &input, const QString &format, const QString &family,
                                      const QString &mode, const QString &vendor, const QString &audio, int keyint,
                                      int resolution)
{
    MainWindow &w = *win_;
    w.setInput(input);
    select(w.formatCombo_, format);
    select(w.vcodecCombo_, family);
    select(w.accelCombo_, mode);
    if (mode == "manual")
        select(w.deviceCombo_, vendor);
    select(w.acodecCombo_, audio);
    select(w.keyintCombo_, keyint);
    select(w.resCombo_, resolution);
    const QString out = path("out." + format);
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    if (!QTest::qWaitFor([&] { return !w.encoding_; }, 600000))
        return QString();
    return w.log_->toPlainText().contains("Done!") ? out : QString();
}

QJsonArray TestVideoCompressor::streams(const QString &file)
{
    const QByteArray json = run(ffprobe_, {"-v", "error", "-show_entries",
                                           "stream=codec_type,codec_name,width,height,channels:stream_tags=encoder",
                                           "-of", "json", file});
    return QJsonDocument::fromJson(json).object().value("streams").toArray();
}

QList<double> TestVideoCompressor::keyframeTimes(const QString &file)
{
    QList<double> times;
    const QString csv = run(ffprobe_, {"-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "packet=pts_time,flags", "-of", "csv=p=0", file});
    for (const QString &line : csv.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.trimmed().split(',');
        if (parts.size() >= 2 && parts[1].contains('K'))
            times << parts[0].toDouble();
    }
    return times;
}

QList<int> TestVideoCompressor::nalTypes(const QString &file)
{
    QList<int> types;
    const QString trace = run(ffmpeg_, {"-v", "trace", "-i", file, "-c", "copy", "-bsf:v", "trace_headers",
                                        "-f", "null", "-"});
    static const QRegularExpression re("nal_unit_type\\s+\\d+ = (\\d+)");
    for (auto it = re.globalMatch(trace); it.hasNext();)
        types << it.next().captured(1).toInt();
    return types;
}

// whether this PC's FFmpeg has an encoder for the codec (the FFmpeg builds differ a little between platforms)
bool TestVideoCompressor::usable(const QString &family) const
{
    return std::any_of(videoCodecs().begin(), videoCodecs().end(), [&](const VideoCodec &c) {
        return c.family == family && win_->encoders_->available().contains(c.encoder);
    });
}

QStringList TestVideoCompressor::hardwareVendors(const QString &family) const
{
    QStringList list;
    for (const VideoCodec *c : win_->hwEncoders(family))
        list << c->vendor;
    return list;
}

// The colour of the frames at these times: r, g, b (or ? for something else), e.g. "brg"
QString TestVideoCompressor::colorsAt(const QString &file, const QList<double> &times)
{
    QString colors;
    for (double t : times) {
        const QByteArray rgb = run(ffmpeg_, {"-v", "error", "-ss", QString::number(t, 'f', 3), "-i", file, "-frames:v",
                                             "1", "-vf", "scale=1:1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
        if (rgb.size() < 3) {
            colors += '?';
            continue;
        }
        const int r = quint8(rgb[0]), g = quint8(rgb[1]), b = quint8(rgb[2]);
        colors += r > 150 && g < 100 && b < 100 ? 'r' : g > 80 && r < 100 && b < 100 ? 'g' : b > 150 && r < 100 && g < 100 ? 'b' : '?';
    }
    return colors;
}

double TestVideoCompressor::durationOf(const QString &file)
{
    return run(ffprobe_, {"-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", file}).trimmed().toDouble();
}

QString plainText(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText().simplified();
}

// ------------------------------------------------ setup
void TestVideoCompressor::initTestCase()
{
    QVERIFY(dir_.isValid());
    ffmpeg_ = findTool("ffmpeg");
    ffprobe_ = findTool("ffprobe");
    QVERIFY2(!ffmpeg_.isEmpty() && !ffprobe_.isEmpty(), "FFmpeg is needed to run the tests");

    const QStringList common{"-y", "-v", "error"};
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=8", "-f", "lavfi", "-i",
                                      "sine=f=440:d=8", "-c:v", "libx264", "-crf", "18", "-c:a", "aac", "-shortest",
                                      path("input.mp4")});
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=40", "-f", "lavfi", "-i",
                                      "sine=f=440:d=40", "-c:v", "libx264", "-crf", "18", "-c:a", "aac", "-shortest",
                                      path("long.mp4")});
    run(ffmpeg_, common + QStringList{"-t", "20", "-i", path("long.mp4"), "-c", "copy", path("20s.mp4")});
    // phone video: stored landscape with a 90 degree rotation flag
    run(ffmpeg_, common + QStringList{"-display_rotation", "90", "-i", path("input.mp4"), "-c", "copy",
                                      path("rotated.mp4")});
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "color=red:s=300x300", "-frames:v", "1", path("cover.png")});
    run(ffmpeg_, common + QStringList{"-i", path("cover.png"), "-i", path("input.mp4"), "-map", "0", "-map", "1",
                                      "-c", "copy", "-disposition:v:0", "attached_pic", path("cover.mp4")});
    // 5.1(side) audio and subtitles: the cases that broke Opus and subtitle auto-mapping
    QFile srt(path("subs.srt"));
    QVERIFY(srt.open(QIODevice::WriteOnly));
    srt.write("1\n00:00:01,000 --> 00:00:02,000\nHello\n");
    srt.close();
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=640x360:r=25:d=3", "-f", "lavfi", "-i",
                                      "sine=f=440:d=3", "-i", path("subs.srt"), "-filter_complex",
                                      "[1:a]aformat=channel_layouts=5.1(side)[a]", "-map", "0:v", "-map", "[a]",
                                      "-map", "2:s", "-c:v", "libx264", "-crf", "20", "-c:a", "ac3", "-c:s", "srt",
                                      path("stress.mkv")});
    // little motion, like a screen recording: keyframes are most of the size
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=1", "-f", "lavfi", "-i",
                                      "color=red:s=60x60:r=30:d=30", "-filter_complex",
                                      "[0:v]loop=loop=-1:size=1,trim=duration=30,setpts=N/30/TB[bg];"
                                      "[bg][1:v]overlay=x='mod(t*80,1860)':y=500",
                                      "-c:v", "libx264", "-crf", "12", path("static.mp4")});
    // 2 s red, 2 s green, 2 s blue, a keyframe every 0.5 s, with sound: for the Editor
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "color=red:s=320x240:r=30:d=2", "-f", "lavfi", "-i",
                                      "color=green:s=320x240:r=30:d=2", "-f", "lavfi", "-i",
                                      "color=blue:s=320x240:r=30:d=2", "-f", "lavfi", "-i", "sine=f=440:d=6",
                                      "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]", "-map", "[v]",
                                      "-map", "3:a", "-c:v", "libx264", "-g", "15", "-keyint_min", "15",
                                      "-sc_threshold", "0", "-c:a", "aac", "-shortest", path("colors.mp4")});
    for (const char *name : {"input.mp4", "long.mp4", "20s.mp4", "rotated.mp4", "cover.mp4", "stress.mkv", "static.mp4",
                             "colors.mp4"})
        QVERIFY2(QFile::exists(path(name)), qPrintable(QString(name) + " was not created"));

    win_ = std::make_unique<MainWindow>();
    win_->showMessage = [this](QMessageBox::Icon, const QString &title, const QString &) { messages_ << title; };
    win_->askQuestion = [this](const QString &title, const QString &) {
        messages_ << title;
        return true;
    };
    win_->show();
    QVERIFY(QTest::qWaitFor([this] { return !win_->encoders_->testing(); }, 30000));
    qInfo().noquote() << "Encoders:" << QStringList(win_->encoders_->available().values()).join(' ');
    qInfo().noquote() << "GPUs:" << QStringList(win_->gpus_.values()).join(", ") << "| CPU:" << win_->cpu_;
}

// ------------------------------------------------ tests
void TestVideoCompressor::probesTrickyInputs()
{
    QString error;
    const auto rotated = probe(ffprobe_, path("rotated.mp4"), &error);
    QVERIFY2(rotated, qPrintable(error));
    QCOMPARE(rotated->width, 720);    // as displayed
    QCOMPARE(rotated->height, 1280);
    QCOMPARE(rotated->rotation, 270); // made with -display_rotation 90, which is counterclockwise

    const auto cover = probe(ffprobe_, path("cover.mp4"), &error);
    QVERIFY(cover);
    QCOMPARE(cover->width, 1280);     // the video, not the 300x300 cover
    QCOMPARE(cover->height, 720);

    const auto stress = probe(ffprobe_, path("stress.mkv"), &error);
    QVERIFY(stress && stress->hasAudio);
    QVERIFY(std::abs(stress->duration - 3.0) < 0.1);

    QVERIFY(!probe(ffprobe_, path("missing.mp4"), &error));
    QCOMPARE(error, QString("file not found"));
}

void TestVideoCompressor::showsVersionAndSourcePreview()
{
    MainWindow &w = *win_;
    QCOMPARE(w.windowTitle(), QString("FrameStudio 9.9.9"));   // version set in main() below

    const auto previewSize = [&](const QString &name) {
        w.setInput(path(name));
        if (!QTest::qWaitFor([&] { return w.preview_->isVisible(); }, 30000))
            return QSize();
        return w.preview_->pixmap().deviceIndependentSize().toSize();
    };
    const QSize landscape = previewSize("input.mp4");   // 16:9: fills the 192x108 box
    QVERIFY2(landscape.width() == 192 && std::abs(landscape.height() - 108) <= 1,
             qPrintable(QString("%1x%2").arg(landscape.width()).arg(landscape.height())));
    const QSize portrait = previewSize("rotated.mp4");  // phone video: stays vertical
    QVERIFY2(portrait.height() == 108 && portrait.width() < 70,
             qPrintable(QString("%1x%2").arg(portrait.width()).arg(portrait.height())));

    w.setInput(path("missing.mp4"));
    QVERIFY(!w.preview_->isVisible());
}

void TestVideoCompressor::offersOnlyCompatibleCodecs()
{
    MainWindow &w = *win_;
    QMap<QString, QStringList> expected;   // the families each format accepts, if this FFmpeg can encode them
    const QMap<QString, QStringList> accepted = {
        {"mp4", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}}, {"mkv", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}},
        {"webm", {"av1", "vp9"}}, {"mov", {"h264", "hevc"}}, {"avi", {"mpeg4"}}};
    for (auto it = accepted.begin(); it != accepted.end(); ++it)
        for (const QString &family : it.value())
            if (usable(family))
                expected[it.key()] << family;
    QVERIFY(usable("h264"));   // x264 is in every FFmpeg build we ship
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        select(w.formatCombo_, it.key());
        QStringList offered;
        for (int i = 0; i < w.vcodecCombo_->count(); ++i) {
            const QString family = w.vcodecCombo_->itemData(i).toString();
            offered << family;
            // the bolt icon appears exactly when a GPU of this PC can encode the codec
            const bool bolt = w.vcodecCombo_->itemIcon(i).cacheKey() == w.hwIcon_.cacheKey();
            QCOMPARE(bolt, !hardwareVendors(family).isEmpty());
        }
        QCOMPARE(offered, it.value());
    }
    select(w.formatCombo_, "mp4");
}

void TestVideoCompressor::statesEncoderClearly()
{
    MainWindow &w = *win_;
    w.setInput(path("input.mp4"));
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    QString text = plainText(w.encoderLabel_->text());
    QVERIFY2(text.contains("Codec: H.264 / AVC") && text.contains("Hardware acceleration: no")
                 && text.contains("Device: CPU " + w.cpu_) && text.contains("Encoder: x264"),
             qPrintable(text));
    QCOMPARE(w.codec_->encoder, QString("libx264"));
    QVERIFY(!w.deviceCombo_->isEnabled());

    select(w.accelCombo_, "auto");
    const QStringList gpus = hardwareVendors("h264");
    text = plainText(w.encoderLabel_->text());
    if (gpus.isEmpty()) {
        QVERIFY2(text.contains("no (no GPU in this PC can encode H.264 / AVC)"), qPrintable(text));
    } else {
        QVERIFY2(text.contains("Hardware acceleration: yes") && text.contains(w.deviceName(gpus.first())),
                 qPrintable(text));
        QCOMPARE(w.codec_->vendor, gpus.first());   // the automatic choice is the preferred vendor
        select(w.accelCombo_, "manual");
        QVERIFY(w.deviceCombo_->isEnabled());
        select(w.deviceCombo_, gpus.last());
        QCOMPARE(w.codec_->vendor, gpus.last());
    }

    if (usable("vp9")) {
        select(w.formatCombo_, "webm");
        select(w.vcodecCombo_, "vp9");
        select(w.accelCombo_, "auto");
        text = plainText(w.encoderLabel_->text());
        QVERIFY2(text.contains("no (no GPU in this PC can encode VP9)") && text.contains("Encoder: libvpx"),
                 qPrintable(text));
    }
    select(w.formatCombo_, "mp4");
}

void TestVideoCompressor::encodesEveryFormatCodecAndDevice()
{
    MainWindow &w = *win_;
    static const QMap<QString, QString> streamCodec = {{"h264", "h264"}, {"hevc", "hevc"}, {"av1", "av1"},
                                                       {"vp9", "vp9"}, {"vvc", "vvc"}, {"mpeg4", "mpeg4"}};
    int encodes = 0;
    for (const Format &f : formats()) {
        select(w.formatCombo_, f.ext);
        QStringList families, audios;
        for (int i = 0; i < w.vcodecCombo_->count(); ++i)
            families << w.vcodecCombo_->itemData(i).toString();
        for (int i = 0; i < w.acodecCombo_->count(); ++i)
            audios << w.acodecCombo_->itemData(i).toString();   // the last one is "" (no audio)
        if (families.isEmpty())
            continue;   // e.g. AVI without an Xvid encoder in this FFmpeg

        // every codec on every device, then every audio codec
        QList<std::tuple<QString, QString, QString, QString>> cases;
        for (const QString &family : families) {
            cases.append({family, "off", "", audios.first()});
            for (const QString &vendor : hardwareVendors(family))
                cases.append({family, "manual", vendor, audios.first()});
        }
        for (const QString &audio : audios.mid(1))
            cases.append({families.first(), "off", "", audio});

        for (const auto &[family, mode, vendor, audio] : cases) {
            const QString out = compress(path("stress.mkv"), f.ext, family, mode, vendor, audio);
            const QString what = QString("%1 %2 %3 %4").arg(f.ext, family, vendor.isEmpty() ? "CPU" : vendor,
                                                            audio.isEmpty() ? "no audio" : audio);
            QVERIFY2(!out.isEmpty(), qPrintable(what + ": " + w.log_->toPlainText().right(300)));
            const QJsonArray s = streams(out);
            QJsonObject video, sound;
            int others = 0;
            for (const QJsonValue &v : s) {
                const QString type = v.toObject().value("codec_type").toString();
                if (type == "video")
                    video = v.toObject();
                else if (type == "audio")
                    sound = v.toObject();
                else
                    ++others;
            }
            QCOMPARE(video.value("codec_name").toString(), streamCodec.value(family));
            // the encoder written in the file must be the one the window announced
            const QString tag = video.value("tags").toObject().value("encoder").toString();
            QVERIFY2(tag.isEmpty() || tag.contains(w.codec_->encoder), qPrintable(what + ": " + tag));
            QCOMPARE(sound.isEmpty(), audio.isEmpty());
            QCOMPARE(others, 0);   // subtitles are not copied
            ++encodes;
        }
    }
    qInfo() << encodes << "encodes checked";
}

void TestVideoCompressor::scalesPortraitPhoneVideos()
{
    const QString out = compress(path("rotated.mp4"), "mp4", "h264", "off", {}, "aac", 10, 540);
    QVERIFY(!out.isEmpty());
    const QJsonObject video = streams(out).first().toObject();
    QCOMPARE(video.value("width").toInt(), 540);
    QCOMPARE(video.value("height").toInt(), 960);
}

void TestVideoCompressor::usesVideoNotCoverArt()
{
    const QString out = compress(path("cover.mp4"), "mp4", "h264", "off", {}, "aac");
    QVERIFY(!out.isEmpty());
    const QJsonArray s = streams(out);
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0].toObject().value("width").toInt(), 1280);
}

void TestVideoCompressor::placesKeyframesWithClosedGop()
{
    MainWindow &w = *win_;
    for (const VideoCodec &codec : videoCodecs()) {
        if (!w.encoders_->available().contains(codec.encoder))
            continue;
        const QString mode = codec.hardware() ? "manual" : "off";
        if (codec.family == "vvc")
            select(w.speedCombo_, "fast");   // VVenC is very slow
        for (int keyint : {10, 2}) {
            const QString out = compress(path("20s.mp4"), "mkv", codec.family, mode, codec.vendor, {}, keyint);
            QVERIFY2(!out.isEmpty(), qPrintable(codec.encoder));
            const QList<double> kf = keyframeTimes(out);
            double maxGap = 0;
            for (int i = 1; i < kf.size(); ++i)
                maxGap = std::max(maxGap, kf[i] - kf[i - 1]);
            // VVenC aligns keyframes to its 32-frame structure: up to ~7% later
            QVERIFY2(maxGap <= keyint * 1.08 + 0.05 && kf.size() >= int(20 / (keyint * 1.08)),
                     qPrintable(QString("%1 every %2 s: %3 keyframes, max gap %4 s")
                                    .arg(codec.encoder).arg(keyint).arg(kf.size()).arg(maxGap)));
            if (keyint == 10 && (codec.family == "h264" || codec.family == "hevc" || codec.family == "vvc")) {
                // closed GOP: every keyframe is an IDR (H.264: 5; HEVC: 19-20, CRA 21; VVC: 7-8, CRA 9)
                const QList<int> types = nalTypes(out);
                const auto count = [&](std::initializer_list<int> values) {
                    return int(std::count_if(types.begin(), types.end(),
                                             [&](int t) { return std::find(values.begin(), values.end(), t) != values.end(); }));
                };
                const int idr = codec.family == "h264" ? count({5}) : codec.family == "hevc" ? count({19, 20}) : count({7, 8});
                const int cra = codec.family == "hevc" ? count({21}) : codec.family == "vvc" ? count({9}) : 0;
                QVERIFY2(cra == 0 && idr >= kf.size(),
                         qPrintable(QString("%1: IDR %2, CRA %3, keyframes %4").arg(codec.encoder).arg(idr).arg(cra).arg(kf.size())));
            }
        }
        select(w.speedCombo_, "balanced");
    }
}

namespace {

// EBML variable-length integer (Matroska)
quint64 readVint(QFile &f, bool keepMarker)
{
    char first = 0;
    f.getChar(&first);
    const quint8 b = quint8(first);
    int length = 1;
    while (length <= 8 && !(b & (0x80 >> (length - 1))))
        ++length;
    quint64 value = keepMarker ? b : (b & (0xFF >> length));
    for (int i = 1; i < length; ++i) {
        char c = 0;
        f.getChar(&c);
        value = (value << 8) | quint8(c);
    }
    return value;
}

// Order of the top-level elements inside the Matroska Segment
QStringList mkvElements(const QString &path)
{
    static const QMap<quint64, QString> names = {{0x114D9B74, "SeekHead"}, {0x1549A966, "Info"},
                                                 {0x1654AE6B, "Tracks"}, {0x1C53BB6B, "Cues"},
                                                 {0x1F43B675, "Cluster"}, {0x1254C367, "Tags"}};
    QStringList order;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return order;
    readVint(f, true);
    const qint64 headerSize = qint64(readVint(f, false));   // read before f.pos(): the size field moves it
    f.seek(f.pos() + headerSize);                           // skip the EBML header
    readVint(f, true);
    readVint(f, false);                              // Segment
    while (!f.atEnd()) {
        const quint64 id = readVint(f, true);
        const quint64 size = readVint(f, false);
        const QString name = names.value(id, "other");
        if (name != "other" && (order.isEmpty() || order.last() != name))
            order << name;
        f.seek(f.pos() + qint64(size));
    }
    return order;
}

QStringList mp4Boxes(const QString &path)
{
    QStringList order;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return order;
    while (f.bytesAvailable() >= 8) {
        const QByteArray head = f.read(8);
        quint64 size = qFromBigEndian<quint32>(head.constData());
        qint64 headerSize = 8;
        if (size == 1) {
            size = qFromBigEndian<quint64>(f.read(8).constData());
            headerSize = 16;
        }
        order << QString::fromLatin1(head.mid(4, 4));
        f.seek(f.pos() + qint64(size) - headerSize);
    }
    return order;
}

} // namespace

void TestVideoCompressor::writesSeekIndexAtStart()
{
    QString out = compress(path("20s.mp4"), "mp4", "h264", "off", {}, "aac");
    QVERIFY(!out.isEmpty());
    const QStringList boxes = mp4Boxes(out);
    QVERIFY2(boxes.indexOf("moov") >= 0 && boxes.indexOf("moov") < boxes.indexOf("mdat"), qPrintable(boxes.join(' ')));

    for (const auto &[format, family] : {std::pair{"mkv", "h264"}, std::pair{"webm", "vp9"}}) {
        if (!usable(family) || !win_->encoders_->available().contains("libopus"))
            continue;
        out = compress(path("20s.mp4"), format, family, "off", {}, "opus");
        QVERIFY(!out.isEmpty());
        const QStringList elements = mkvElements(out);
        QVERIFY2(elements.indexOf("Cues") >= 0 && elements.indexOf("Cues") < elements.indexOf("Cluster"),
                 qPrintable(QString(format) + ": " + elements.join(' ')));
    }
}

void TestVideoCompressor::estimatesAccurately_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("family");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("keyint");
    for (const char *input : {"static.mp4", "long.mp4"})
        for (int keyint : {10, 2}) {
            for (const char *family : {"h264", "av1"})
                QTest::addRow("%s %s CPU %d s", input, family, keyint) << QString(input) << QString(family) << QString("off") << keyint;
            QTest::addRow("%s h264 GPU %d s", input, keyint) << QString(input) << QString("h264") << QString("auto") << keyint;
        }
}

void TestVideoCompressor::estimatesAccurately()
{
    QFETCH(QString, input);
    QFETCH(QString, family);
    QFETCH(QString, mode);
    QFETCH(int, keyint);
    MainWindow &w = *win_;
    if (mode == "auto" && hardwareVendors(family).isEmpty())
        QSKIP("no GPU encoder on this PC");
    if (!usable(family))
        QSKIP("no encoder for this codec in this FFmpeg");

    w.setInput(path(input));
    select(w.formatCombo_, "mkv");
    select(w.vcodecCombo_, family);
    select(w.accelCombo_, mode);
    select(w.acodecCombo_, QString());
    select(w.keyintCombo_, keyint);
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    QVERIFY2(w.estBps_, qPrintable(w.sizeDetail_->text()));
    const double estimate = w.estimatedBytes();
    const QString out = path("estimate.mkv");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
    const double real = double(QFileInfo(out).size());
    const double error = estimate / real - 1;
    qInfo().noquote() << QString("%1 %2 %3: estimate %4 KB, real %5 KB, error %6%")
                             .arg(input, w.codec_->encoder).arg(keyint).arg(estimate / 1024, 0, 'f', 0)
                             .arg(real / 1024, 0, 'f', 0).arg(error * 100, 0, 'f', 1);
    QVERIFY(std::abs(error) < 0.12);   // the README promises roughly ±10%
}

// The turning wheel is visible exactly while an estimate is on its way.
void TestVideoCompressor::spinsWhileEstimating()
{
    MainWindow &w = *win_;
    w.setInput(path("input.mp4"));   // a new video: nothing estimated yet
    QVERIFY(w.spinner_->isSpinning() && w.spinner_->isVisible());
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    QVERIFY(!w.spinner_->isSpinning() && !w.spinner_->isVisible());

    select(w.rateCombo_, int(RateMode::Vbr));   // nothing to encode with a bitrate: no wheel
    QVERIFY(!w.spinner_->isSpinning());
    select(w.rateCombo_, int(RateMode::Quality));
}

void TestVideoCompressor::fitsToLimit()
{
    MainWindow &w = *win_;
    if (!win_->encoders_->available().contains("libx265"))
        QSKIP("no x265 in this FFmpeg");
    messages_.clear();
    w.setInput(path("long.mp4"));
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "hevc");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, "aac");
    select(w.keyintCombo_, 10);
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    w.limitSpin_->setValue(2.0);
    w.fitToLimit();
    QVERIFY(QTest::qWaitFor([&] { return !w.fit_ && settled(); }, 300000));
    QVERIFY2(messages_.isEmpty(), qPrintable(messages_.join(", ")));
    QVERIFY(w.estimatedBytes() <= 2.0 * MB);

    const QString out = path("fit.mp4");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
    const qint64 size = QFileInfo(out).size();
    qInfo().noquote() << "Fit to 2 MB:" << w.qualityLabel_->text() << "->" << fmtMb(double(size));
    QVERIFY(size > 0 && size <= 2 * MB);
}

// Every encoder offers the bitrate modes it has (a GPU's only if they work on this PC) and keeps to the
// bitrate: the size is computed from it right away, and the file is never much bigger.
void TestVideoCompressor::encodesAtTheChosenBitrate()
{
    MainWindow &w = *win_;
    const auto *model = qobject_cast<const QStandardItemModel *>(w.rateCombo_->model());
    constexpr int kbps = 1500;
    w.setInput(path("20s.mp4"));
    select(w.formatCombo_, "mkv");
    select(w.acodecCombo_, QString());
    select(w.keyintCombo_, 10);
    int encodes = 0;
    for (const VideoCodec &codec : videoCodecs()) {
        if (!w.encoders_->available().contains(codec.encoder))
            continue;
        select(w.vcodecCombo_, codec.family);
        select(w.accelCombo_, codec.hardware() ? "manual" : "off");
        if (codec.hardware())
            select(w.deviceCombo_, codec.vendor);
        select(w.speedCombo_, codec.family == "vvc" ? "fast" : "balanced");   // VVenC is very slow
        QCOMPARE(w.codec_, &codec);
        for (RateMode mode : {RateMode::Vbr, RateMode::Cbr}) {
            const int index = w.rateCombo_->findData(int(mode));
            const bool offered = model->item(index)->isEnabled();
            const QString what = codec.encoder + " " + rateModeName(mode);
            QCOMPARE(offered, w.encoders_->supports(codec, mode));
            if (!codec.hardware())
                QVERIFY2(offered == codec.hasMode(mode), qPrintable(what));
            if (!offered)
                continue;
            w.rateCombo_->setCurrentIndex(index);
            w.bitrateSpin_->setValue(kbps);
            QVERIFY2(!w.spinner_->isSpinning() && w.estBps_ && *w.estBps_ == kbps * 1000.0 / 8, qPrintable(what));
            const double estimate = w.estimatedBytes();
            const QString out = path("bitrate.mkv");
            QFile::remove(out);
            w.outEdit_->setText(out);
            w.start();
            QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
            const QString log = w.log_->toPlainText();
            QVERIFY2(log.contains("Done!") && log.contains(QString("%1 %2 kbps").arg(rateModeName(mode)).arg(kbps)),
                     qPrintable(what + ": " + log.right(300)));
            const double ratio = double(QFileInfo(out).size()) / estimate;
            qInfo().noquote() << QString("%1 at %2 kbps: %3% of the estimate").arg(what).arg(kbps).arg(ratio * 100, 0, 'f', 1);
            // GPU encoders can go up to ~10% over an average bitrate, the others a few %
            QVERIFY2(ratio < 1.12, qPrintable(what));
            // these pad a constant bitrate, so it stays constant even where the video is simple
            const bool pads = codec.encoder == "libx264" || codec.encoder == "libx265" || codec.label == "NVENC";
            if (mode == RateMode::Cbr && pads)
                QVERIFY2(ratio > 0.92, qPrintable(what));
            ++encodes;
        }
        select(w.rateCombo_, int(RateMode::Quality));
    }
    select(w.speedCombo_, "balanced");
    qInfo() << encodes << "bitrate encodes checked";
}

void TestVideoCompressor::fitsBitrateToLimit()
{
    MainWindow &w = *win_;
    messages_.clear();
    w.setInput(path("long.mp4"));
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, "aac");
    select(w.rateCombo_, int(RateMode::Vbr));
    QCOMPARE(w.fitBtn_->text(), QString("Fit bitrate to limit"));
    w.limitSpin_->setValue(3.0);
    w.fitToLimit();
    QVERIFY2(messages_.isEmpty(), qPrintable(messages_.join(", ")));
    // computed, not searched: right under the margin
    QVERIFY2(w.estimatedBytes() <= 3.0 * LIMIT_MARGIN * MB && w.estimatedBytes() > 3.0 * LIMIT_MARGIN * MB * 0.98,
             qPrintable(fmtMb(w.estimatedBytes())));

    const QString out = path("fitbitrate.mp4");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
    const qint64 size = QFileInfo(out).size();
    qInfo().noquote() << "Fit to 3 MB:" << w.bitrateSpin_->value() << "kbps ->" << fmtMb(double(size));
    QVERIFY(size > 2.5 * MB && size <= 3 * MB);
    select(w.rateCombo_, int(RateMode::Quality));
    w.limitSpin_->setValue(10);
}

// The analyzer: within a limit it lowers the size of the picture rather than the quality; for the smallest file
// it uses the "no visible loss" quality; it changes the codec only when allowed.
void TestVideoCompressor::analyzesAndSetsTheBest()
{
    MainWindow &w = *win_;
    w.setInput(path("long.mp4"));   // 40 s at 720p, full of detail, with sound
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, "aac");
    w.allowCodecCheck_->setChecked(false);
    const auto analyze = [&] {
        w.analyze();
        return QTest::qWaitFor([&] { return !w.auto_ && !w.fit_ && settled(); }, 300000);
    };

    select(w.goalCombo_, true);   // smallest with no visible loss
    QVERIFY(analyze());
    QVERIFY2(w.autoResult_->text().contains("Smallest with no visible loss") && w.autoResult_->text().contains("Analysis:"),
             qPrintable(w.autoResult_->text()));
    QCOMPARE(w.quality(), w.qualityAt(0.15));
    QCOMPARE(w.resCombo_->currentData().toInt(), 0);
    QCOMPARE(w.codec_->family, QString("h264"));   // not allowed to change it
    qInfo().noquote() << plainText(w.autoResult_->text());

    select(w.goalCombo_, false);   // best within 2 MB: too little for 720p at good quality
    w.limitSpin_->setValue(2.0);
    QVERIFY(analyze());
    const QString text = w.autoResult_->text();
    qInfo().noquote() << plainText(text);
    QVERIFY2(text.contains("Best within 2.0 MB") || text.contains("Even at the smallest size"), qPrintable(text));
    QVERIFY(w.resCombo_->currentData().toInt() != 0);   // a smaller picture
    QVERIFY(w.estimatedBytes() <= 2.0 * MB);
    if (text.contains("Best within"))
        QVERIFY(w.quality() <= w.qualityAt(0.38));   // still good quality

    w.allowCodecCheck_->setChecked(true);   // now it may pick a more efficient codec
    select(w.goalCombo_, true);
    QVERIFY(analyze());
    QVERIFY2(w.codec_->family == "av1" || w.codec_->family == "hevc", qPrintable(w.codec_->family));
    w.allowCodecCheck_->setChecked(false);
    w.limitSpin_->setValue(10);
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
}

// The end of a compression is shown in a colored box with what happened, until the next one starts.
void TestVideoCompressor::showsTheOutcomeClearly()
{
    MainWindow &w = *win_;
    QVERIFY(!compress(path("input.mp4"), "mp4", "h264", "off", {}, "aac").isEmpty());
    QVERIFY(w.outcomeBar_->isVisible());
    QCOMPARE(w.outcomeTitle_->text(), QString("Export complete"));
    QVERIFY2(w.outcomeText_->text().contains("out.mp4") && w.outcomeText_->text().contains("smaller than the original"),
             qPrintable(w.outcomeText_->text()));
    QVERIFY(w.playBtn_->isVisible() && w.openBtn_->isVisible() && w.openBtn_->isEnabled());

    w.limitSpin_->setValue(0.5);   // the file is bigger: still done, but it says so
    QVERIFY(!compress(path("input.mp4"), "mp4", "h264", "off", {}, "aac").isEmpty());
    QCOMPARE(w.outcomeTitle_->text(), QString("Export complete, but over the limit"));
    QVERIFY2(w.outcomeText_->text().contains("over the 0.5 MB limit"), qPrintable(w.outcomeText_->text()));
    w.limitSpin_->setValue(10);

    w.setInput(path("input.mp4"));
    QVERIFY(!w.outcomeBar_->isVisible());   // a new video: the box of the previous one goes away
    w.outEdit_->setText(path("missing folder/out.mp4"));
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 60000));
    QVERIFY(w.outcomeBar_->isVisible());
    QCOMPARE(w.outcomeTitle_->text(), QString("Export failed"));
    QVERIFY(!w.playBtn_->isVisible() && !w.openBtn_->isVisible());

    w.outEdit_->setText(path("out.mp4"));
    w.start();
    QVERIFY(!w.outcomeBar_->isVisible());   // hidden while the next one runs
    w.cancel();
    QVERIFY(!w.outcomeBar_->isVisible());
}

// The window can be shorter than what's in it (laptop screens): then it scrolls, nothing is squeezed, and
// the box at the end of a compression is scrolled into view.
void TestVideoCompressor::scrollsOnShortScreens()
{
    MainWindow &w = *win_;
    const QSize before = w.size();
    w.resize(w.width(), 400);
    QCOMPARE(w.height(), 400);
    QVERIFY(!compress(path("input.mp4"), "mp4", "h264", "off", {}, "aac").isEmpty());
    QTest::qWait(100);
    QVERIFY(w.scroll_->verticalScrollBar()->isVisible());
    QVERIFY2(w.encoderLabel_->height() >= w.encoderLabel_->sizeHint().height(),
             qPrintable(QString("%1 < %2").arg(w.encoderLabel_->height()).arg(w.encoderLabel_->sizeHint().height())));
    const QRect box(w.outcomeBar_->mapTo(w.scroll_->viewport(), QPoint(0, 0)), w.outcomeBar_->size());
    QVERIFY(w.scroll_->viewport()->rect().contains(box));
    w.resize(before);
}

// No network: the version logic and GitHub's JSON are checked directly, the notice by emitting the signal.
void TestVideoCompressor::notifiesAboutUpdates()
{
    QVERIFY(UpdateChecker::isNewer("v9.9.10", "9.9.9"));    // numeric, not alphabetical
    QVERIFY(UpdateChecker::isNewer("v10.0.0", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("v9.9.9", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("v9.9", "9.9.0"));
    QVERIFY(!UpdateChecker::isNewer("v9.8.0", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("nightly", "9.9.9"));

    const QByteArray json = R"({
        "tag_name": "v9.9.10",
        "html_url": "https://github.com/Andrea332/VideoCompressor/releases/tag/v9.9.10",
        "assets": [
            {"name": "FrameStudio-9.9.10-win64.zip", "browser_download_url": "https://example.com/a.zip"},
            {"name": "FrameStudio-9.9.10-win64.exe", "browser_download_url": "https://example.com/win64.exe",
             "digest": "sha256:ABCDEF0123"},
            {"name": "FrameStudio-9.9.10-win-arm64.exe", "browser_download_url": "https://example.com/win-arm64.exe",
             "digest": "sha256:ABCDEF4567"},
            {"name": "FrameStudio-9.9.10-macos-arm64.dmg", "browser_download_url": "https://example.com/a.dmg"}
        ]})";
    const auto info = UpdateChecker::parseLatestRelease(json, "9.9.9");
    QVERIFY(info);
    QCOMPARE(info->version, QString("9.9.10"));
    // the installer for the processor this build is for (x64 or ARM64)
    const QString platform = QString(UpdateChecker::INSTALLER_SUFFIX).chopped(4).mid(1);
    QVERIFY(platform == "win64" || platform == "win-arm64");
    QCOMPARE(info->installerName, "FrameStudio-9.9.10-" + platform + ".exe");
    QCOMPARE(info->installerUrl, QUrl("https://example.com/" + platform + ".exe"));
    QCOMPARE(info->installerSha256, QString(platform == "win64" ? "abcdef0123" : "abcdef4567"));
    QVERIFY(!UpdateChecker::parseLatestRelease(json, "9.9.10"));

    MainWindow &w = *win_;
    QVERIFY(!w.autoUpdateCheck_->isChecked());   // turned off in main(): tests never go online
    QVERIFY(!w.updateBar_->isVisible());
    emit w.updater_->updateAvailable(*info);
    QVERIFY(w.updateBar_->isVisible());
    QVERIFY2(w.updateText_->text().contains("9.9.10") && w.updateText_->text().contains("9.9.9"),
             qPrintable(w.updateText_->text()));
    // not installed with the installer (no uninstaller next to the test executable): download page
    QCOMPARE(w.updateBtn_->text(), QString("Download"));
    QTest::mouseClick(w.laterBtn_, Qt::LeftButton);
    QVERIFY(!w.updateBar_->isVisible());

    // the time of the check is saved as readable text (the portable versions' settings file can be opened)
    // and read back as a date
    const QDateTime saved = QSettings().value("updates/lastCheck").toDateTime();
    QVERIFY(saved.isValid() && saved.secsTo(QDateTime::currentDateTimeUtc()) < 60);
    QFile ini(QSettings().fileName());
    QVERIFY(ini.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(ini.readAll());
    QVERIFY2(text.contains("lastCheck=" + QString::number(QDate::currentDate().year())), qPrintable(text));
}

// The portable versions come with VideoCompressor.ini next to the program, and keep their settings in it.
void TestVideoCompressor::keepsPortableSettingsNextToTheApp()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
#if defined(Q_OS_WIN)
    const QString appDir = root.path();                                     // VideoCompressor.exe
#elif defined(Q_OS_MACOS)
    const QString appDir = root.filePath("VideoCompressor.app/Contents/MacOS");   // next to the app
#else
    const QString appDir = root.filePath("usr/bin");                        // top of the extracted folder
#endif
    const QString ini = portableSettingsPath(appDir);
    QCOMPARE(ini, QDir::cleanPath(root.filePath("FrameStudio.ini")));

    // the file of the packages (comments only) is read and then written like any settings file
    QVERIFY(QFile::copy(QStringLiteral(SOURCE_DIR "/packaging/portable/FrameStudio.ini"), ini));
    QFile::setPermissions(ini, QFile::ReadOwner | QFile::WriteOwner);
    {
        QSettings settings(ini, QSettings::IniFormat);
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(settings.allKeys().isEmpty());
        settings.setValue("updates/autoCheck", false);
    }
    QCOMPARE(QSettings(ini, QSettings::IniFormat).value("updates/autoCheck", true).toBool(), false);

    // the tests' own program has no such file: its settings go where main() below puts them
    QVERIFY(!QFileInfo::exists(portableSettingsPath(QCoreApplication::applicationDirPath())));
    QCOMPARE(appSettings()->fileName(), QSettings().fileName());
}

// A window destroyed while its children still emit signals must not receive them any more: FFmpeg
// processes finishing (preview, estimate, GPU tests) and the path field, which emits editingFinished
// when it loses the focus as the window closes. This used to corrupt the heap at exit (seen on Linux;
// the sanitizer job reports it every time).
void TestVideoCompressor::survivesClosingWhileBusy()
{
    for (int delayMs : {0, 300, 800}) {
        auto window = std::make_unique<MainWindow>();
        window->showMessage = [](QMessageBox::Icon, const QString &, const QString &) {};
        window->show();
        window->activateWindow();
        window->setInput(path("long.mp4"));
        window->inEdit_->setFocus();
        QTest::qWait(delayMs);
        QVERIFY(window->previewProc_ || window->estTimer_->isActive() || window->estimator_->running()
                || delayMs > 0);
    }
}

// ------------------------------------------------ editor
void TestVideoCompressor::editsAListOfPieces()
{
    using Pieces = std::vector<Piece>;
    EditList e(10);
    QCOMPARE(e.duration(), 10.0);
    QVERIFY(e.isWhole(10));
    e.split(4);
    QCOMPARE(e.pieces(), (Pieces{{0, 4}, {4, 10}}));
    QVERIFY(e.isWhole(10));                                   // split, but still the whole video in order
    QCOMPARE(e.joined(), (Pieces{{0, 10}}));
    e.split(4);                                               // on a boundary: nothing
    QCOMPARE(e.pieces().size(), size_t(2));

    const Pieces cut = e.remove(2, 6);                        // across two pieces
    QCOMPARE(cut, (Pieces{{2, 4}, {4, 6}}));
    QCOMPARE(e.pieces(), (Pieces{{0, 2}, {6, 10}}));
    QCOMPARE(e.duration(), 6.0);
    QVERIFY(!e.isWhole(10));
    e.insert(6, cut);                                         // pasted at the end
    QCOMPARE(e.pieces(), (Pieces{{0, 2}, {6, 10}, {2, 4}, {4, 6}}));
    QCOMPARE(e.joined(), (Pieces{{0, 2}, {6, 10}, {2, 6}}));
    QCOMPARE(e.copy(1, 3), (Pieces{{1, 2}, {6, 7}}));         // copying changes nothing
    QCOMPARE(e.duration(), 10.0);

    const auto [index, source] = e.locate(7);                 // in the 3rd piece, 1 s after its start
    QCOMPARE(index, 2);
    QCOMPARE(source, 3.0);
    QCOMPARE(e.pieceStart(2), 6.0);
    QCOMPARE(e.locate(100).second, 6.0);                      // clamped to the end

    EditList m(10);
    QCOMPARE(m.move(8, 10, 0), 0.0);                          // the end to the start
    QCOMPARE(m.pieces(), (Pieces{{8, 10}, {0, 8}}));
    QCOMPARE(m.move(0, 2, 10), 8.0);                          // and back: the target moves left by its length
    QCOMPARE(m.joined(), (Pieces{{0, 10}}));
    QCOMPARE(m.move(2, 5, 3), 2.0);                           // into itself: nothing
    QCOMPARE(m.joined(), (Pieces{{0, 10}}));
}

void TestVideoCompressor::editsWithKeysAndMouse()
{
    using Pieces = std::vector<Piece>;
    MainWindow &w = *win_;
    Editor &e = *w.editor_;
    w.setInput(path("colors.mp4"));
    w.tabs_->setCurrentWidget(&e);
    w.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&w));
    e.timeline_->setFocus();
    QVERIFY(QTest::qWaitFor([&] { return e.timeline_->hasFocus(); }, 5000));
    const auto key = [&](int k, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(e.timeline_, Qt::Key(k), modifiers);
    };

    e.seek(2);
    key(Qt::Key_I);                                          // the selection starts here...
    e.seek(4);
    key(Qt::Key_O);                                          // ...and ends here
    QCOMPARE(e.timeline_->selectionStart(), 2.0);
    QCOMPARE(e.timeline_->selectionEnd(), 4.0);
    key(Qt::Key_X, Qt::ControlModifier);                     // cut the green part
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 2}, {4, 6}}));
    QVERIFY(!w.editNotice_->isHidden());   // in the Compress tab
    QCOMPARE(w.outputDuration(), 4.0);
    key(Qt::Key_End);
    key(Qt::Key_V, Qt::ControlModifier);                     // pasted at the end
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 2}, {4, 6}, {2, 4}}));
    key(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 2}, {4, 6}}));
    key(Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(!e.edited());
    QVERIFY(w.editNotice_->isHidden());
    key(Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 2}, {4, 6}}));

    e.seek(1);
    key(Qt::Key_S);                                          // split at the playhead
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 1}, {1, 2}, {4, 6}}));
    key(Qt::Key_A, Qt::ControlModifier);
    key(Qt::Key_Delete);                                     // everything: refused
    QCOMPARE(e.edits().pieces().size(), size_t(3));
    key(Qt::Key_Escape);
    QVERIFY(!e.timeline_->hasSelection());

    // the mouse: x of a time on the timeline, and a drag with one button
    Timeline &t = *e.timeline_;
    const auto x = [&](double time) { return int(8 + time * (t.width() - 16) / e.edits().duration()); };
    const auto drag = [&](Qt::MouseButton button, int from, int to, int y = 40) {
        QTest::mousePress(&t, button, Qt::NoModifier, QPoint(from, y));
        for (int step = 1; step <= 4; ++step) {
            const QPointF at(from + (to - from) * step / 4.0, y);
            QMouseEvent move(QEvent::MouseMove, at, t.mapToGlobal(at), Qt::NoButton, button, Qt::NoModifier);
            QApplication::sendEvent(&t, &move);
        }
        QTest::mouseRelease(&t, button, Qt::NoModifier, QPoint(to, y));
    };

    // left button: the playhead follows the mouse, the selection stays
    e.timeline_->setSelection(0, 1);
    drag(Qt::LeftButton, x(1.5), x(2.5), 10);
    QVERIFY2(std::abs(e.playhead_ - 2.5) < 0.04, qPrintable(QString::number(e.playhead_)));
    QCOMPARE(t.selectionEnd(), 1.0);
    // right button: dragging selects, a click asks for the menu
    drag(Qt::RightButton, x(1.5), x(3.0));
    QVERIFY2(std::abs(t.selectionStart() - 1.5) < 0.04 && std::abs(t.selectionEnd() - 3.0) < 0.04,
             qPrintable(QString("%1-%2").arg(t.selectionStart()).arg(t.selectionEnd())));
    QObject::disconnect(&t, &Timeline::menuRequested, &e, nullptr);   // the menu would wait for a click
    QSignalSpy menu(&t, &Timeline::menuRequested);
    QTest::mouseClick(&t, Qt::RightButton, Qt::NoModifier, QPoint(x(1.0), 40));
    QCOMPARE(menu.count(), 1);
    QObject::connect(&t, &Timeline::menuRequested, &e, &Editor::showMenu);

    // double-click selects the first piece; dragging it (away from the playhead) puts it after the last one
    QTest::mouseDClick(&t, Qt::LeftButton, Qt::NoModifier, QPoint(x(0.5), 40));
    QCOMPARE(t.selectionStart(), 0.0);
    QCOMPARE(t.selectionEnd(), 1.0);
    drag(Qt::LeftButton, x(0.25), x(4.0) - 1);
    QCOMPARE(e.edits().pieces(), (Pieces{{1, 2}, {4, 6}, {0, 1}}));
    QCOMPARE(t.selectionStart(), 3.0);                       // the moved piece stays selected
    key(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(e.edits().pieces(), (Pieces{{0, 1}, {1, 2}, {4, 6}}));
    w.tabs_->setCurrentIndex(0);
}

// The player shows the frame at the playhead and, when playing, jumps over what was cut.
void TestVideoCompressor::playsTheEditedVideo()
{
    MainWindow &w = *win_;
    Editor &e = *w.editor_;
    w.setInput(path("colors.mp4"));
    w.tabs_->setCurrentWidget(&e);
    e.player_->setAudioOutput(nullptr);   // the test machines may have no sound card
    const auto center = [&] {
        const QImage &f = e.view_->frame();
        return f.isNull() ? QColor() : f.pixelColor(f.width() / 2, f.height() / 2);
    };
    e.seek(3);
    if (!QTest::qWaitFor([&] { return !e.view_->frame().isNull(); }, 15000))
        QSKIP(qPrintable("no video from Qt Multimedia here: " + e.player_->errorString()));
    QVERIFY(QTest::qWaitFor([&] { return center().green() > 100 && center().red() < 100; }, 10000));

    e.edits_.setPieces({{0, 1}, {4, 5}});   // 1 s red, then 1 s blue
    e.editsUpdated();
    e.seek(0);
    QVERIFY(QTest::qWaitFor([&] { return center().red() > 150; }, 10000));
    QString seen;   // the colours shown while playing, each once in a row
    const QMetaObject::Connection watch = QObject::connect(e.view_, &VideoView::frameShown, [&] {
        const QColor c = center();
        const QChar color = c.red() > 150 && c.green() < 100 ? 'r'
                            : c.green() > 100 && c.red() < 100 && c.blue() < 100 ? 'g'
                            : c.blue() > 150 && c.green() < 100 ? 'b' : '?';
        if (color != '?' && !seen.endsWith(color))
            seen += color;
    });
    e.togglePlay();
    QVERIFY(QTest::qWaitFor([&] { return e.player_->playbackState() != QMediaPlayer::PlayingState; }, 20000));
    QObject::disconnect(watch);
    QCOMPARE(seen, QString("rb"));   // red, then blue: never the green part that was cut
    QCOMPARE(e.playhead_, 2.0);      // stopped at the end of the edited video
    e.player_->setAudioOutput(e.audio_);
    w.tabs_->setCurrentIndex(0);
}

// Moved, pasted and deleted parts end up in the compressed file, cut exactly on their frames.
void TestVideoCompressor::compressesTheEditedVideo()
{
    MainWindow &w = *win_;
    Editor &e = *w.editor_;
    w.setInput(path("colors.mp4"));
    e.seek(4);
    e.setStart();                      // to the end: the blue part
    e.cut();
    e.seek(0);
    e.paste();                         // blue, red, green
    e.timeline_->setSelection(5.3, 6); // and 0.7 s less green, not on a keyframe
    e.deleteSelection();
    QCOMPARE(w.outputDuration(), 5.3);
    for (const char *audio : {"aac", "opus"}) {   // Opus has its audio filter inside the filter graph
        if (!w.encoders_->available().contains(audio == QString("aac") ? "aac" : "libopus"))
            continue;
        const QString format = audio == QString("aac") ? "mp4" : "mkv";
        select(w.formatCombo_, format);
        select(w.vcodecCombo_, "h264");
        select(w.accelCombo_, "off");
        select(w.acodecCombo_, audio);
        const QString out = path("edited." + format);
        QFile::remove(out);
        w.outEdit_->setText(out);
        w.start();
        QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 120000));
        QVERIFY2(w.log_->toPlainText().contains("Done!"), qPrintable(w.log_->toPlainText().right(500)));
        QVERIFY2(std::abs(durationOf(out) - 5.3) < 0.1, qPrintable(QString::number(durationOf(out))));
        QCOMPARE(colorsAt(out, {0.5, 1.95, 2.05, 3.9, 4.1, 5.2}), QString("bbrrgg"));
        QCOMPARE(streams(out).size(), 2);   // video and sound
    }
    e.reset();
    QVERIFY(!e.edited());
}

void TestVideoCompressor::estimatesTheEditedVideo()
{
    MainWindow &w = *win_;
    w.setInput(path("long.mp4"));
    select(w.formatCombo_, "mkv");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, QString());
    w.editor_->edits_.setPieces({{25, 35}, {5, 15}});   // 20 s out of 40, in another order
    w.editor_->editsUpdated();
    bool exact = true;
    QCOMPARE(w.estimateSamples(&exact).size(), 2);     // short enough: each piece is encoded whole
    QVERIFY(!exact);
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    const double estimate = w.estimatedBytes();
    const QString out = path("edited-estimate.mkv");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 300000));
    const double real = double(QFileInfo(out).size());
    qInfo().noquote() << QString("Edited video: estimate %1 KB, real %2 KB").arg(estimate / 1024, 0, 'f', 0)
                             .arg(real / 1024, 0, 'f', 0);
    QVERIFY(std::abs(estimate / real - 1) < 0.12);
    w.editor_->reset();
}

// Phone videos, stored landscape with a rotation flag, stay upright when edited: in the player and in the file.
void TestVideoCompressor::editsPhoneVideos()
{
    MainWindow &w = *win_;
    Editor &e = *w.editor_;
    w.setInput(path("rotated.mp4"));
    w.tabs_->setCurrentWidget(&e);
    e.player_->setAudioOutput(nullptr);
    e.seek(1);
    if (QTest::qWaitFor([&] { return !e.view_->frame().isNull(); }, 15000))
        QVERIFY2(e.view_->frame().height() > e.view_->frame().width(),
                 qPrintable(QString("%1x%2").arg(e.view_->frame().width()).arg(e.view_->frame().height())));
    e.player_->setAudioOutput(e.audio_);
    w.tabs_->setCurrentIndex(0);

    e.edits_.setPieces({{4, 6}, {0, 2}});   // two pieces: joined by the concat filter
    e.editsUpdated();
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, "aac");
    select(w.resCombo_, 0);
    const QString out = path("edited-phone.mp4");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 120000));
    QVERIFY2(w.log_->toPlainText().contains("Done!"), qPrintable(w.log_->toPlainText().right(500)));
    const QJsonObject video = streams(out).first().toObject();
    QCOMPARE(video.value("width").toInt(), 720);
    QCOMPARE(video.value("height").toInt(), 1280);
    e.reset();
}

// The copy without re-encoding: each piece starts on a keyframe (asking first if one moves earlier).
void TestVideoCompressor::savesWithoutReencoding()
{
    MainWindow &w = *win_;
    Editor &e = *w.editor_;
    w.setInput(path("colors.mp4"));
    QVERIFY(QTest::qWaitFor([&] { return e.keyframesReady(); }, 30000));
    QCOMPARE(e.keyframes().size(), size_t(12));   // every 0.5 s
    e.edits_.setPieces({{2, 4}, {0, 1.25}, {4.25, 6}});   // green, red, blue from 4.25 (not a keyframe)
    e.editsUpdated();
    messages_.clear();
    const QString out = path("copy.mp4");
    QFile::remove(out);
    w.saveCopy(out);
    QCOMPARE(messages_, QStringList{"Save without re-encoding"});   // the blue piece starts at 4.0 instead
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 60000));
    QVERIFY2(w.log_->toPlainText().contains("Done!"), qPrintable(w.log_->toPlainText().right(500)));
    QCOMPARE(w.outcomeTitle_->text(), QString("Saved without re-encoding"));
    QVERIFY2(std::abs(durationOf(out) - 5.25) < 0.15, qPrintable(QString::number(durationOf(out))));
    QCOMPARE(colorsAt(out, {0.5, 1.9, 2.1, 3.1, 3.4, 5.0}), QString("ggrrbb"));
    const QJsonArray s = streams(out);
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0].toObject().value("codec_name").toString(), QString("h264"));   // copied, not re-encoded
    e.reset();
}

// Saves a screenshot to VC_SCREENSHOT_DIR, if set (use QT_QPA_FONTDIR=C:/Windows/Fonts to get text offscreen).
// With VC_SCREENSHOT_VIDEO it is the README picture: that video, default settings, a fresh-looking window
// (see the workflow, which runs it on Windows with the real platform plugin).
void TestVideoCompressor::rendersWindow()
{
    MainWindow &w = *win_;
    const QString video = qEnvironmentVariable("VC_SCREENSHOT_VIDEO");
    w.setInput(video.isEmpty() ? path("long.mp4") : video);
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, video.isEmpty() ? "hevc" : "h264");
    // the CI machine has no GPU: "Off" reads better there than "no GPU in this PC can encode H.264"
    select(w.accelCombo_, video.isEmpty() ? "auto" : "off");
    if (!video.isEmpty()) {
        select(w.acodecCombo_, "aac");
        select(w.keyintCombo_, 10);
        w.limitSpin_->setValue(25);
        w.autoUpdateCheck_->setChecked(true);   // as a new user sees it (this only changes the tests' settings)
        w.log_->clear();
        w.progress_->setValue(0);
        w.openBtn_->setEnabled(false);
    }
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    QVERIFY(QTest::qWaitFor([&] { return w.preview_->isVisible(); }, 30000));
    // all of it, without the scroll bar, even if the screen is shorter (the CI machine's is): a minimum height
    // lets the window be taller than the screen
    const auto grabAll = [&] {
        QTest::qWait(200);   // the layout adapts to the texts just set
        const int fullHeight = w.scroll_->widget()->heightForWidth(w.width());
        w.setMinimumHeight(fullHeight);
        w.resize(w.width(), fullHeight);
        QTest::qWait(200);
        const QPixmap shot = w.grab();
        w.setMinimumHeight(0);
        return shot;
    };
    const QPixmap shot = grabAll();
    QVERIFY(!shot.isNull());
    const QString dir = qEnvironmentVariable("VC_SCREENSHOT_DIR");
    if (!dir.isEmpty())
        QVERIFY(shot.save(dir + "/window.png"));

    // the Editor, with a part cut and one moved to the end (the second picture of the README)
    Editor &e = *w.editor_;
    w.tabs_->setCurrentWidget(&e);
    const double d = w.media_->duration;
    e.edits_.setPieces({{0, d * 0.3}, {d * 0.55, d}, {d * 0.3, d * 0.45}});
    e.editsUpdated();
    e.timeline_->setSelection(d * 0.75, d * 0.9);   // the moved part
    e.updateActions();
    e.seek(d * 0.8);
    QVERIFY(QTest::qWaitFor([&] { return e.keyframesReady() && e.thumbQueue_.empty() && !e.thumbProc_; }, 120000));
    if (!QTest::qWaitFor([&] { return !e.view_->frame().isNull(); }, 15000))
        qInfo() << "No video frame from Qt Multimedia for the picture";
    const QPixmap editorShot = grabAll();
    QVERIFY(!editorShot.isNull());
    if (!dir.isEmpty())
        QVERIFY(editorShot.save(dir + "/editor.png"));
    e.reset();
    w.tabs_->setCurrentIndex(0);
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("VideoCompressorTests");
    QApplication::setApplicationName("tst_video_compressor");
    QApplication::setApplicationVersion("9.9.9");
    // preferences in a temporary file, not the user's, with the automatic update check off
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().setValue("updates/autoCheck", false);
    TestVideoCompressor test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_video_compressor.moc"
