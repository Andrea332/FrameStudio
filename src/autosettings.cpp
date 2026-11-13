// The analyzer of the Export tab: it looks at the video and picks the settings by itself, for one of two goals.
//
// - Best quality within the limit: tries the original size and frame rate, then lower ones, each at the
//   lowest "good" quality, and keeps the first that fits under the limit; then raises the quality as far as
//   the limit allows (no further than "no visible loss"). So it lowers the resolution or the frame rate
//   instead of letting the quality drop below good.
// - Smallest file with no visible loss: the original size and frame rate at the "no visible loss" quality.
//
// The sound gets a bitrate that suits its channels (never more than the original). Format and codec change
// only if the user allows it: then the most efficient codec there is (AV1, otherwise H.265).

#include "mainwindow.h"

#include "editor.h"
#include "encodercheck.h"
#include "sizeestimator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

// positions on an encoder's quality scale (0 = best, 1 = worst)
constexpr double NO_VISIBLE_LOSS = 0.15;
constexpr double GOOD = 0.38;

// bits that H.264 would need for the same picture, compared with each codec (rough)
double h264Equivalent(const VideoCodec &codec)
{
    static const QMap<QString, double> efficiency = {{"h264", 1.0}, {"hevc", 1.5}, {"av1", 1.7},
                                                     {"vp9", 1.4},  {"vvc", 1.9},  {"mpeg4", 0.7}};
    return efficiency.value(codec.family, 1.0) * (codec.hardware() ? 0.85 : 1.0);
}

} // namespace

int MainWindow::qualityAt(double fraction) const
{
    const QualityScale &scale = codec_->quality;
    return int(std::lround(scale.best + fraction * (scale.worst - scale.best)));
}

void MainWindow::analyze()
{
    if (!media_ || !codec_ || encoding_)
        return;
    fit_.reset();
    Auto a;
    a.smallest = goalCombo_->currentData().toBool();
    programmatic_ = true;

    // a more efficient codec, if allowed: AV1, otherwise H.265 (in MP4 if the format can't hold it)
    if (allowCodecCheck_->isChecked()) {
        for (const char *family : {"av1", "hevc"}) {
            const bool usable = std::any_of(videoCodecs().begin(), videoCodecs().end(), [&](const VideoCodec &c) {
                return c.family == family && encoders_->available().contains(c.encoder);
            });
            if (!usable)
                continue;
            if (!fmt().video.contains(family))
                formatCombo_->setCurrentIndex(formatCombo_->findData("mp4"));
            vcodecCombo_->setCurrentIndex(vcodecCombo_->findData(family));
            break;
        }
    }
    rateCombo_->setCurrentIndex(rateCombo_->findData(int(RateMode::Quality)));

    // the sound: enough for its channels, never more than the original
    if (audioCodec()) {
        const int channels = media_->audioChannels > 0 ? media_->audioChannels : 2;
        int kbps = channels <= 1 ? 64 : channels <= 2 ? 128 : 192;
        if (media_->audioKbps > 0)
            kbps = std::min(kbps, media_->audioKbps);
        if (!a.smallest) {   // not more than a tenth of the limit
            const double tenth = limitSpin_->value() * MB / 10 / outputDuration() * 8 / 1000;
            kbps = std::min(kbps, std::max(64, int(tenth)));
        }
        const std::vector<int> &steps = audioBitrates();   // the closest one that isn't higher
        int chosen = steps.front();
        for (int step : steps)
            if (step <= kbps)
                chosen = step;
        abitrateCombo_->setCurrentIndex(abitrateCombo_->findData(chosen));
    }

    // what to try: the original size and frame rate first, then smaller ones (30 fps before a smaller size)
    QList<int> fpsSteps{0};
    if (fpsCombo_->findData(30) >= 0)
        fpsSteps << 30;
    for (int i = 0; i < resCombo_->count(); ++i)
        for (int fps : fpsSteps)
            a.candidates.emplace_back(resCombo_->itemData(i).toInt(), fps);
    if (a.smallest)
        a.candidates.resize(1);
    auto_ = a;
    programmatic_ = false;

    analyzeBtn_->setEnabled(false);
    autoResult_->setText(a.smallest ? "Analyzing: looking for the smallest file with no visible loss…"
                                    : "Analyzing: looking for the best quality within the limit…");
    applyAutoCandidate();
}

// The next size and frame rate to try, at the quality the goal asks for.
void MainWindow::applyAutoCandidate()
{
    Auto &a = *auto_;
    const auto [resolution, fps] = a.candidates[a.next++];
    programmatic_ = true;
    resCombo_->setCurrentIndex(resCombo_->findData(resolution));
    fpsCombo_->setCurrentIndex(fpsCombo_->findData(fps));
    qualitySlider_->setValue(qualityAt(a.smallest ? NO_VISIBLE_LOSS : GOOD));
    onSettingsChanged();   // estimates it (also if nothing changed)
    programmatic_ = false;
}

// An estimate is ready: keep this size and frame rate, or try the next.
void MainWindow::autoStep()
{
    Auto &a = *auto_;
    if (a.content.isEmpty())
        a.content = describeContent();
    if (a.smallest) {
        finishAuto();
        return;
    }
    const double target = limitSpin_->value() * MB * LIMIT_MARGIN;
    const bool fits = estimatedBytes() <= target;
    if (!fits && a.next < a.candidates.size()) {
        applyAutoCandidate();
        return;
    }
    a.reachedGood = fits;
    a.fitting = true;
    fitToLimit(qualityAt(NO_VISIBLE_LOSS));   // as good as the limit allows, but no better than needed
    if (!fit_ && !estTimer_->isActive() && !estimator_->running())
        finishAuto();   // nothing to search (e.g. the sound alone is over the limit)
}

void MainWindow::finishAuto()
{
    const Auto a = *auto_;
    auto_.reset();
    analyzeBtn_->setEnabled(true);
    const MediaInfo &m = *media_;
    const int res = resCombo_->currentData().toInt(), fpsValue = fpsCombo_->currentData().toInt();
    const QString size = res ? QString("%1p").arg(res) : QString("%1p (original)").arg(std::min(m.width, m.height));
    const QString fps = fpsValue ? QString("%1 fps").arg(fpsValue) : QString("%1 fps (original)").arg(std::lround(m.fps));
    const AudioCodec *acodec = audioCodec();
    const QString sound = acodec ? QString("%1 %2 kbps").arg(acodec->label).arg(audioKbps()) : QString("no sound");
    const QString chosen = QString("%1, %2, %3 %4 %5 (%6 quality), %7")
                               .arg(size, fps, familyName(codec_->family), codec_->qName)
                               .arg(codec_->shownQuality(quality()))
                               .arg(qualityName(quality(), codec_->quality), sound);
    const double total = estimatedBytes();
    QString text = "<b>" + a.content.toHtmlEscaped() + "</b><br>";
    if (a.smallest) {
        text += QString("✔ Smallest with no visible loss: %1 → ≈ %2 (%3% of the original).")
                    .arg(chosen.toHtmlEscaped(), fmtMb(total))
                    .arg(total / double(media_->size) * 100, 0, 'f', 0);
        if (total > double(media_->size) * 0.9)
            text += " The video is already well compressed: exporting it again saves little. To keep it as it "
                    "is, use “Save without re-encoding” in the Editor.";
    } else if (a.reachedGood) {
        text += QString("✔ Best within %1 MB: %2 → ≈ %3.")
                    .arg(limitSpin_->value(), 0, 'f', 1)
                    .arg(chosen.toHtmlEscaped(), fmtMb(total));
    } else {
        text += QString("⚠ Even at the smallest size the video needs more than %1 MB at good quality: %2 → ≈ %3. "
                        "Cutting it in the Editor, or a higher limit, would look better.")
                    .arg(limitSpin_->value(), 0, 'f', 1)
                    .arg(chosen.toHtmlEscaped(), fmtMb(total));
    }
    autoResult_->setText(text);
}

void MainWindow::stopAuto(const QString &why)
{
    if (!auto_)
        return;
    auto_.reset();
    analyzeBtn_->setEnabled(media_.has_value() && !encoding_);
    autoResult_->setText(why);
}

// What the analysis found, from the source and from the first estimate (how many bits each pixel needs
// at good quality tells static pictures from a lot of motion).
QString MainWindow::describeContent() const
{
    const MediaInfo &m = *media_;
    QStringList parts;
    QString video = QString("%1×%2, %3 fps").arg(m.width).arg(m.height).arg(m.fps, 0, 'f', m.fps == std::floor(m.fps) ? 0 : 2);
    if (m.videoKbps > 0)
        video += QString(", %1 Mbps").arg(m.videoKbps / 1000.0, 0, 'f', 1);
    parts << video;
    if (estBps_) {
        const int res = resCombo_->currentData().toInt();
        const int shortSide = std::min(m.width, m.height);
        const double scale = res && res < shortSide ? double(res) / shortSide : 1.0;
        const double fps = fpsCombo_->currentData().toInt() ? fpsCombo_->currentData().toInt() : std::max(1.0, m.fps);
        const double pixels = m.width * scale * m.height * scale * fps;
        // bits per pixel, as H.264 at good quality would need
        const double bpp = *estBps_ * 8 / pixels * h264Equivalent(*codec_)
                           * std::pow(2.0, (quality() - qualityAt(GOOD)) / codec_->halving);
        parts << (bpp < 0.005 ? "very little motion or detail (like a screen recording)"
                  : bpp < 0.02 ? "little motion or detail"
                  : bpp < 0.08 ? "normal motion and detail"
                               : "a lot of motion or detail");
    }
    if (m.hasAudio) {
        const QString channels = m.audioChannels == 1 ? "mono" : m.audioChannels == 2 ? "stereo"
                                 : m.audioChannels > 2 ? QString("%1 channels").arg(m.audioChannels) : "sound";
        parts << (m.audioKbps > 0 ? QString("%1 sound at %2 kbps").arg(channels).arg(m.audioKbps) : channels + " sound");
    } else {
        parts << "no sound";
    }
    return "Analysis: " + parts.join(" · ");
}
