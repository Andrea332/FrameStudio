#include "editor.h"

#include "timeline.h"
#include "videoview.h"

#include <QAudioOutput>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <cmath>

namespace {

constexpr int MAX_THUMBNAILS = 48;
constexpr int THUMBNAIL_HEIGHT = 112;   // pixels: sharp on the timeline also on high-density screens
constexpr int THUMBNAIL_PROCESSES = 2;

qint64 milliseconds(double seconds)
{
    return std::llround(seconds * 1000);
}

// A curved arrow, pointing left (undo) or right (redo)
QIcon arrowIcon(bool redo, const QColor &color)
{
    QIcon icon;
    for (int size : {16, 24, 32, 48}) {
        QPixmap pix(size, size);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        if (redo)
            p.setTransform(QTransform(-1, 0, 0, 1, size, 0));   // the same arrow, mirrored
        const double u = size / 16.0;
        p.setPen(QPen(color, 1.6 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath arc;   // from the head, around and down
        arc.moveTo(4 * u, 6 * u);
        arc.lineTo(10 * u, 6 * u);
        arc.cubicTo(13.5 * u, 6 * u, 13.5 * u, 13 * u, 10 * u, 13 * u);
        arc.lineTo(6 * u, 13 * u);
        p.drawPath(arc);
        p.drawPolyline(QPolygonF({{7 * u, 3 * u}, {4 * u, 6 * u}, {7 * u, 9 * u}}));
        p.end();
        icon.addPixmap(pix);
    }
    return icon;
}

double totalLength(const std::vector<Piece> &pieces)
{
    double total = 0;
    for (const Piece &p : pieces)
        total += p.length();
    return total;
}

} // namespace

QString fmtPrecise(double seconds)
{
    const long long hundredths = std::llround(std::max(0.0, seconds) * 100);
    const long long h = hundredths / 360000, m = hundredths / 6000 % 60, s = hundredths / 100 % 60, c = hundredths % 100;
    const QString rest = QString("%1.%2").arg(s, 2, 10, QChar('0')).arg(c, 2, 10, QChar('0'));
    return h ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(rest) : QString("%1:%2").arg(m).arg(rest);
}

Editor::Editor(const QString &ffmpeg, const QString &ffprobe, QWidget *parent)
    : QWidget(parent), ffmpeg_(ffmpeg), ffprobe_(ffprobe)
{
    player_ = new QMediaPlayer(this);
    audio_ = new QAudioOutput(this);
    player_->setAudioOutput(audio_);
    view_ = new VideoView;
    player_->setVideoSink(view_->sink());
    playTimer_ = new QTimer(this);
    playTimer_->setInterval(15);
    connect(playTimer_, &QTimer::timeout, this, &Editor::onPlayTick);
    // paused, a new position interrupts the frame the player is still decoding: while the playhead is dragged,
    // hardly any frame would appear. So one at a time, and the next (the latest position) when it is shown.
    seekTimer_ = new QTimer(this);
    seekTimer_->setSingleShot(true);
    seekTimer_->setInterval(400);   // in case no frame comes (e.g. the same position again)
    connect(seekTimer_, &QTimer::timeout, this, &Editor::seekDone);
    connect(view_, &VideoView::frameShown, this, [this] {
        if (seekTimer_->isActive())
            seekDone();
    });
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia)
            seek(playhead_);   // shows the frame at the playhead
        else if (status == QMediaPlayer::EndOfMedia) {
            playTimer_->stop();
            seek(edits_.duration());
        }
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        const bool playing = state == QMediaPlayer::PlayingState;
        playBtn_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        playBtn_->setToolTip(playing ? "Pause (Space)" : "Play (Space)");
    });

    // --- Transport ---
    const auto toolButton = [this](QStyle::StandardPixmap icon, const QString &tip, auto slot) {
        auto *button = new QToolButton;
        button->setIcon(style()->standardIcon(icon));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, this, slot);
        return button;
    };
    auto *startBtn = toolButton(QStyle::SP_MediaSkipBackward, "To the start (Home)", [this] { pause(); seek(0); });
    auto *backBtn = toolButton(QStyle::SP_MediaSeekBackward, "Previous frame (Left)", [this] { stepFrames(-1); });
    playBtn_ = toolButton(QStyle::SP_MediaPlay, "Play (Space)", [this] { togglePlay(); });
    auto *forwardBtn = toolButton(QStyle::SP_MediaSeekForward, "Next frame (Right)", [this] { stepFrames(1); });
    auto *endBtn = toolButton(QStyle::SP_MediaSkipForward, "To the end (End)", [this] { pause(); seek(edits_.duration()); });
    timeLabel_ = new QLabel;
    QFont mono = timeLabel_->font();
    mono.setStyleHint(QFont::Monospace);
    mono.setFamilies({"Consolas", "Menlo", "DejaVu Sans Mono", "monospace"});
    timeLabel_->setFont(mono);
    muteBtn_ = toolButton(QStyle::SP_MediaVolume, "Sound on/off", [this] {
        audio_->setMuted(!audio_->isMuted());
        muteBtn_->setIcon(style()->standardIcon(audio_->isMuted() ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    });
    const auto zoomButton = [this](const QString &text, const QString &tip, auto slot) {
        auto *button = new QToolButton;
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, this, slot);
        return button;
    };
    auto *transport = new QHBoxLayout;
    for (QWidget *w : std::initializer_list<QWidget *>{startBtn, backBtn, playBtn_, forwardBtn, endBtn})
        transport->addWidget(w);
    transport->addSpacing(8);
    transport->addWidget(timeLabel_);
    transport->addStretch();
    transport->addWidget(muteBtn_);
    transport->addSpacing(8);
    transport->addWidget(zoomButton("−", "Zoom out (Ctrl+-, or Ctrl+wheel on the timeline)", [this] { timeline_->zoomBy(1 / 1.5); }));
    transport->addWidget(zoomButton("+", "Zoom in (Ctrl++, or Ctrl+wheel on the timeline)", [this] { timeline_->zoomBy(1.5); }));
    transport->addWidget(zoomButton("Fit", "The whole video (Ctrl+0)", [this] { timeline_->zoomToFit(); }));

    // --- Timeline ---
    timeline_ = new Timeline;
    timeline_->setToolTip("Left button: click or drag to move the playhead, drag the selection to move it.\n"
                          "Right button: drag to select, click for the menu. Double-click selects a piece.\n"
                          "Ctrl+wheel zooms; zoomed in, the wheel moves along the video (so does the bar\n"
                          "that appears below). The marks under the pieces are keyframes: where a copy\n"
                          "without re-encoding can start.");
    connect(timeline_, &Timeline::playheadDragged, this, [this](double t) {
        pause();
        seek(t);
    });
    connect(timeline_, &Timeline::selectionChanged, this, &Editor::updateActions);
    connect(timeline_, &Timeline::moveRequested, this, &Editor::moveSelection);
    connect(timeline_, &Timeline::menuRequested, this, &Editor::showMenu);
    connect(timeline_, &Timeline::thumbnailsWanted, this, &Editor::wantThumbnails);
    // under it, only when it is zoomed in: where the part on screen is in the whole video
    overview_ = new TimelineOverview(timeline_);
    overview_->hide();
    connect(timeline_, &Timeline::viewChanged, this, [this] { overview_->setVisible(timeline_->zoom() > 1.001); });

    // --- Editing ---
    // tool buttons: as wide as their text (push buttons are at least 80 pixels wide on Windows)
    const auto editButton = [this](const QString &text, const QString &tip, auto slot) {
        auto *button = new QToolButton;
        button->setText(text);
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setMinimumWidth(48);
        connect(button, &QToolButton::clicked, this, slot);
        editButtons_ << button;
        return button;
    };
    startBtn_ = editButton("Start", "The selection starts at the playhead (I)", [this] { setStart(); });
    endBtn_ = editButton("End", "The selection ends at the playhead (O)", [this] { setEnd(); });
    splitBtn_ = editButton("Split", "Splits the piece at the playhead in two (S)", [this] { splitAtPlayhead(); });
    cutBtn_ = editButton("Cut", "Removes the selection and keeps it for Paste (Ctrl+X)", [this] { cut(); });
    copyBtn_ = editButton("Copy", "Keeps a copy of the selection for Paste (Ctrl+C)", [this] { copy(); });
    pasteBtn_ = editButton("Paste", "Inserts what was cut or copied at the playhead (Ctrl+V)", [this] { paste(); });
    deleteBtn_ = editButton("Delete", "Removes the selection (Del)", [this] { deleteSelection(); });
    undoBtn_ = editButton("Undo", "Undo (Ctrl+Z)", [this] { undo(); });
    redoBtn_ = editButton("Redo", "Redo (Ctrl+Y or Ctrl+Shift+Z)", [this] { redo(); });
    for (QToolButton *b : {undoBtn_, redoBtn_}) {
        b->setIcon(arrowIcon(b == redoBtn_, palette().color(QPalette::ButtonText)));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    // the buttons in groups: what works at the playhead, and what works on the selection
    const auto group = [](const QString &title, std::initializer_list<QToolButton *> buttons) {
        auto *box = new QGroupBox(title);
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(6, 2, 6, 4);
        row->setSpacing(4);
        for (QToolButton *b : buttons)
            row->addWidget(b);
        return box;
    };
    auto *playheadBox = group("At the playhead", {splitBtn_, pasteBtn_});
    selectionBox_ = group("Selection", {startBtn_, endBtn_, cutBtn_, copyBtn_, deleteBtn_});
    auto *tools = new QHBoxLayout;
    tools->addWidget(playheadBox);
    tools->addWidget(selectionBox_);
    tools->addStretch();
    tools->addWidget(undoBtn_, 0, Qt::AlignBottom);
    tools->addWidget(redoBtn_, 0, Qt::AlignBottom);

    status_ = new QLabel;
    status_->setWordWrap(true);
    resetBtn_ = new QPushButton("Undo all");
    resetBtn_->setToolTip("Back to the whole video, as it is");
    resetBtn_->setFocusPolicy(Qt::NoFocus);
    connect(resetBtn_, &QPushButton::clicked, this, [this] { reset(); });
    editButtons_ << resetBtn_;
    saveCopyBtn_ = new QPushButton("Save without re-encoding…");
    saveCopyBtn_->setFocusPolicy(Qt::NoFocus);
    saveCopyBtn_->setToolTip("Saves the edited video right away, in the original quality and format.\n"
                             "Without re-encoding, each piece can only start on a keyframe: one that starts\n"
                             "elsewhere starts at the keyframe before. To make it smaller, or to cut exactly\n"
                             "on any frame, use Export.");
    connect(saveCopyBtn_, &QPushButton::clicked, this, &Editor::saveCopyRequested);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(status_, 1);
    bottom->addWidget(resetBtn_);
    bottom->addWidget(saveCopyBtn_);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(view_, 1);
    layout->addLayout(transport);
    layout->addWidget(timeline_);
    layout->addWidget(overview_);
    layout->addLayout(tools);
    layout->addLayout(bottom);

    // keyboard: anywhere in the window while the Editor tab is shown (a text field keeps its own keys)
    const auto key = [this](const QKeySequence &keys, auto slot) {
        shortcuts_ << new QShortcut(keys, this, slot, Qt::WindowShortcut);
    };
    key(Qt::Key_Space, [this] { togglePlay(); });
    key(Qt::Key_Left, [this] { stepFrames(-1); });
    key(Qt::Key_Right, [this] { stepFrames(1); });
    key(Qt::SHIFT | Qt::Key_Left, [this] { pause(); seek(playhead_ - 1); });
    key(Qt::SHIFT | Qt::Key_Right, [this] { pause(); seek(playhead_ + 1); });
    key(Qt::Key_Home, [this] { pause(); seek(0); });
    key(Qt::Key_End, [this] { pause(); seek(edits_.duration()); });
    key(Qt::Key_I, [this] { setStart(); });
    key(Qt::Key_O, [this] { setEnd(); });
    key(Qt::Key_S, [this] { splitAtPlayhead(); });
    key(QKeySequence::Cut, [this] { cut(); });
    key(QKeySequence::Copy, [this] { copy(); });
    key(QKeySequence::Paste, [this] { paste(); });
    key(Qt::Key_Delete, [this] { deleteSelection(); });
    key(Qt::Key_Backspace, [this] { deleteSelection(); });
    key(QKeySequence::Undo, [this] { undo(); });
    key(Qt::CTRL | Qt::Key_Y, [this] { redo(); });
    key(Qt::CTRL | Qt::SHIFT | Qt::Key_Z, [this] { redo(); });
    key(QKeySequence::SelectAll, [this] { selectAll(); });
    key(Qt::Key_Escape, [this] { clearSelection(); });
    key(Qt::CTRL | Qt::Key_Plus, [this] { timeline_->zoomBy(1.5); });
    key(Qt::CTRL | Qt::Key_Equal, [this] { timeline_->zoomBy(1.5); });
    key(Qt::CTRL | Qt::Key_Minus, [this] { timeline_->zoomBy(1 / 1.5); });
    key(Qt::CTRL | Qt::Key_0, [this] { timeline_->zoomToFit(); });
    for (QShortcut *s : std::as_const(shortcuts_))
        s->setEnabled(false);   // until the tab is shown

    setMedia(std::nullopt);
}

Editor::~Editor()
{
    // the children are deleted after this: they must not call back into a half-destroyed editor
    for (QObject *child : findChildren<QObject *>())
        child->disconnect(this);
    player_->stop();
    for (QProcess *proc : QList<QProcess *>{keyframeProc_} + thumbProcs_)
        if (proc) {
            proc->kill();
            proc->waitForFinished(3000);
        }
}

bool Editor::edited() const
{
    return media_ && !edits_.isWhole(media_->duration);
}

void Editor::setMedia(const std::optional<MediaInfo> &media)
{
    ++gen_;
    pause();
    for (QProcess *proc : QList<QProcess *>{keyframeProc_} + thumbProcs_)
        if (proc) {
            proc->disconnect(this);
            proc->kill();
            proc->waitForFinished(1000);
            proc->deleteLater();
        }
    keyframeProc_ = nullptr;
    seekTimer_->stop();
    nextSeek_ = -1;
    thumbProcs_.clear();
    wantedThumbs_.clear();
    requestedThumbs_.clear();
    media_ = media;
    edits_ = EditList(media ? media->duration : 0);
    undo_.clear();
    redo_.clear();
    clipboard_.clear();
    keyframes_.clear();
    keyframesReady_ = false;
    thumbQueue_.clear();
    thumbsStarted_ = false;
    playhead_ = 0;
    view_->clear();
    view_->setRotation(media ? media->rotation : 0);
    timeline_->clearThumbnails();
    timeline_->setKeyframes({});
    timeline_->setSelection(0, 0);
    timeline_->zoomToFit();
    timeline_->setFrameDuration(media && media->fps > 0 ? 1 / media->fps : 1.0 / 30);
    player_->setSource(media ? QUrl::fromLocalFile(media->path) : QUrl());
    editsUpdated();
    if (media)
        readKeyframes();
}

void Editor::setBusy(bool busy)
{
    busy_ = busy;
    if (busy)
        pause();
    updateActions();
}

void Editor::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    for (QShortcut *s : std::as_const(shortcuts_))
        s->setEnabled(true);
    if (keyframesReady_ && !thumbsStarted_)
        nextThumbnail();
}

void Editor::hideEvent(QHideEvent *event)
{
    for (QShortcut *s : std::as_const(shortcuts_))
        s->setEnabled(false);   // the other tab's keys are its own
    pause();
    QWidget::hideEvent(event);
}

// ------------------------------------------------ playback
// The player plays the source file: at the end of each piece it jumps to the start of the next one.
void Editor::togglePlay()
{
    if (!media_)
        return;
    if (player_->playbackState() == QMediaPlayer::PlayingState) {
        pause();
        return;
    }
    const double frame = 1 / std::max(1.0, media_->fps);
    seek(playhead_ >= edits_.duration() - frame / 2 ? 0 : playhead_);
    if (nextSeek_ >= 0)   // from where the playhead is, not from the frame still being looked for
        player_->setPosition(std::exchange(nextSeek_, -1));
    seekTimer_->stop();
    player_->play();
    playTimer_->start();
}

void Editor::pause()
{
    playTimer_->stop();
    if (player_->playbackState() == QMediaPlayer::PlayingState)
        player_->pause();
}

void Editor::seek(double t)
{
    playhead_ = std::clamp(t, 0.0, edits_.duration());
    timeline_->setPlayhead(playhead_);
    showTime();
    EditList played;
    played.setPieces(playPieces_);
    const auto [index, source] = played.locate(playhead_);
    playing_ = index;
    if (index < 0)
        return;
    if (player_->playbackState() == QMediaPlayer::StoppedState)
        player_->pause();   // a paused player shows the frame it is on
    jumpTarget_ = source;
    if (player_->playbackState() == QMediaPlayer::PlayingState) {
        player_->setPosition(milliseconds(source));
    } else if (seekTimer_->isActive()) {
        nextSeek_ = milliseconds(source);   // after the frame being looked for
    } else {
        player_->setPosition(milliseconds(source));
        seekTimer_->start();
    }
}

void Editor::seekDone()
{
    seekTimer_->stop();
    if (nextSeek_ < 0)
        return;
    player_->setPosition(std::exchange(nextSeek_, -1));
    seekTimer_->start();
}

void Editor::stepFrames(int frames)
{
    if (!media_)
        return;
    pause();
    const double frame = 1 / std::max(1.0, media_->fps);
    seek((std::round(playhead_ / frame) + frames) * frame);
}

void Editor::onPlayTick()
{
    if (player_->playbackState() != QMediaPlayer::PlayingState || playing_ < 0
        || playing_ >= int(playPieces_.size()))
        return;
    const double source = player_->position() / 1000.0;
    if (jumpTarget_ >= 0) {   // the player still reports where it was before the jump
        if (std::abs(source - jumpTarget_) > 0.25)
            return;
        jumpTarget_ = -1;
    }
    const Piece &piece = playPieces_[size_t(playing_)];
    const double frame = 1 / std::max(1.0, media_->fps);
    double start = 0;
    for (int i = 0; i < playing_; ++i)
        start += playPieces_[size_t(i)].length();
    if (source >= piece.end - frame / 2) {
        if (playing_ + 1 < int(playPieces_.size())) {
            ++playing_;
            jumpTarget_ = playPieces_[size_t(playing_)].start;
            player_->setPosition(milliseconds(jumpTarget_));
            playhead_ = start + piece.length();
        } else {
            pause();
            playhead_ = edits_.duration();
        }
    } else {
        playhead_ = start + std::max(0.0, source - piece.start);
    }
    timeline_->setPlayhead(playhead_);
    showTime();
}

void Editor::showTime()
{
    timeLabel_->setText(fmtPrecise(playhead_) + " / " + fmtPrecise(edits_.duration()));
}

// ------------------------------------------------ editing
bool Editor::leavesNothing(double from, double to) const
{
    const double frame = media_ ? 1 / std::max(1.0, media_->fps) : 0.04;
    return to - from >= edits_.duration() - frame / 2;
}

void Editor::setStart()
{
    if (!media_)
        return;
    const double end = timeline_->hasSelection() && timeline_->selectionEnd() > playhead_ ? timeline_->selectionEnd()
                                                                                         : edits_.duration();
    timeline_->setSelection(playhead_, end);
    updateActions();
}

void Editor::setEnd()
{
    if (!media_)
        return;
    const double start = timeline_->hasSelection() && timeline_->selectionStart() < playhead_ ? timeline_->selectionStart()
                                                                                             : 0.0;
    timeline_->setSelection(start, playhead_);
    updateActions();
}

void Editor::selectAll()
{
    timeline_->setSelection(0, edits_.duration());
    updateActions();
}

void Editor::clearSelection()
{
    timeline_->setSelection(playhead_, playhead_);
    updateActions();
}

void Editor::splitAtPlayhead()
{
    if (busy_ || !media_)
        return;
    const std::vector<Piece> before = edits_.pieces();
    edits_.split(playhead_);
    change(before, "Split");
}

void Editor::cut()
{
    if (busy_ || !timeline_->hasSelection())
        return;
    const double from = timeline_->selectionStart(), to = timeline_->selectionEnd();
    if (leavesNothing(from, to)) {
        status_->setText("⚠ The video can't be left empty: select a part, not all of it.");
        return;
    }
    const std::vector<Piece> before = edits_.pieces();
    clipboard_ = edits_.remove(from, to);
    timeline_->setSelection(from, from);
    playhead_ = from;
    change(before, "Cut");
}

void Editor::copy()
{
    if (!timeline_->hasSelection())
        return;
    clipboard_ = edits_.copy(timeline_->selectionStart(), timeline_->selectionEnd());
    updateActions();
}

void Editor::paste()
{
    if (busy_ || clipboard_.empty())
        return;
    const std::vector<Piece> before = edits_.pieces();
    const double at = playhead_;
    edits_.insert(at, clipboard_);
    timeline_->setPieces(edits_.pieces());
    timeline_->setSelection(at, at + totalLength(clipboard_));   // what was pasted
    change(before, "Paste");
}

void Editor::deleteSelection()
{
    if (busy_ || !timeline_->hasSelection())
        return;
    const double from = timeline_->selectionStart(), to = timeline_->selectionEnd();
    if (leavesNothing(from, to)) {
        status_->setText("⚠ The video can't be left empty: select a part, not all of it.");
        return;
    }
    const std::vector<Piece> before = edits_.pieces();
    edits_.remove(from, to);
    timeline_->setSelection(from, from);
    playhead_ = from;
    change(before, "Delete");
}

void Editor::moveSelection(double from, double to, double at)
{
    if (busy_)
        return;
    const std::vector<Piece> before = edits_.pieces();
    const double start = edits_.move(from, to, at);
    timeline_->setPieces(edits_.pieces());
    timeline_->setSelection(start, start + (to - from));   // the moved part stays selected
    playhead_ = start;
    change(before, "Move");
}

void Editor::undo()
{
    if (busy_ || undo_.empty())
        return;
    redo_.push_back(edits_.pieces());
    edits_.setPieces(undo_.back());
    undo_.pop_back();
    timeline_->setSelection(playhead_, playhead_);
    editsUpdated();
}

void Editor::redo()
{
    if (busy_ || redo_.empty())
        return;
    undo_.push_back(edits_.pieces());
    edits_.setPieces(redo_.back());
    redo_.pop_back();
    timeline_->setSelection(playhead_, playhead_);
    editsUpdated();
}

void Editor::reset()
{
    if (busy_ || !edited())
        return;
    const std::vector<Piece> before = edits_.pieces();
    edits_ = EditList(media_->duration);
    timeline_->setSelection(0, 0);
    change(before, "Undo all");
}

// Right click on the timeline: what can be done there
void Editor::showMenu(double t, const QPoint &globalPos)
{
    if (!media_)
        return;
    QMenu menu(this);
    const bool selected = timeline_->hasSelection();
    const bool editable = !busy_;
    const auto add = [&](const QString &text, const QKeySequence &keys, bool enabled, auto slot) {
        // the keys only shown (as shortcuts of the actions they would clash with the editor's own)
        QAction *action = menu.addAction(text + '	' + keys.toString(QKeySequence::NativeText), this, slot);
        action->setEnabled(enabled);
    };
    add("Cut", QKeySequence::Cut, editable && selected, [this] { cut(); });
    add("Copy", QKeySequence::Copy, selected, [this] { copy(); });
    add("Paste at the playhead", QKeySequence::Paste, editable && !clipboard_.empty(), [this] { paste(); });
    add("Delete", QKeySequence::Delete, editable && selected, [this] { deleteSelection(); });
    menu.addSeparator();
    add("Split here", Qt::Key_S, editable, [this, t] {
        pause();
        seek(t);
        splitAtPlayhead();
    });
    add("Select this piece", QKeySequence(), true, [this, t] {
        const auto [index, source] = edits_.locate(t);
        if (index < 0)
            return;
        const double start = edits_.pieceStart(index);
        timeline_->setSelection(start, start + edits_.pieces()[size_t(index)].length());
        updateActions();
    });
    add("Select all", QKeySequence::SelectAll, true, [this] { selectAll(); });
    add("Clear the selection", Qt::Key_Escape, selected, [this] { clearSelection(); });
    menu.addSeparator();
    add("Undo", QKeySequence::Undo, editable && !undo_.empty(), [this] { undo(); });
    add("Redo", QKeySequence(Qt::CTRL | Qt::Key_Y), editable && !redo_.empty(), [this] { redo(); });
    menu.exec(globalPos);
}

bool Editor::change(const std::vector<Piece> &before, const QString &)
{
    if (before == edits_.pieces())
        return false;   // nothing changed (e.g. a split on a boundary)
    undo_.push_back(before);
    redo_.clear();
    editsUpdated();
    return true;
}

void Editor::editsUpdated()
{
    playPieces_ = edits_.joined();
    timeline_->setPieces(edits_.pieces());
    seek(playhead_);
    updateActions();
    emit editsChanged();
}

void Editor::updateActions()
{
    const bool loaded = media_.has_value();
    const bool editable = loaded && !busy_;
    const bool selected = timeline_->hasSelection();
    for (QAbstractButton *b : std::as_const(editButtons_))
        b->setEnabled(editable);
    cutBtn_->setEnabled(editable && selected);
    copyBtn_->setEnabled(loaded && selected);
    deleteBtn_->setEnabled(editable && selected);
    pasteBtn_->setEnabled(editable && !clipboard_.empty());
    undoBtn_->setEnabled(editable && !undo_.empty());
    redoBtn_->setEnabled(editable && !redo_.empty());
    resetBtn_->setEnabled(editable && edited());
    saveCopyBtn_->setEnabled(editable && keyframesReady_);
    saveCopyBtn_->setText(loaded && !keyframesReady_ ? "Save without re-encoding (reading keyframes…)"
                                                     : "Save without re-encoding…");

    selectionBox_->setTitle(selected ? "Selection: " + fmtPrecise(timeline_->selectionEnd() - timeline_->selectionStart())
                                     : QString("Selection"));
    if (!loaded) {
        status_->setText("Load a video to edit it.");
        return;
    }
    // only what the rest doesn't show: the lengths are next to the play buttons, in the Selection title and at
    // the top, the pieces on the timeline
    status_->setText(clipboard_.empty() ? QString()
                                        : QString("%1 cut or copied: Paste puts it at the playhead.")
                                              .arg(fmtPrecise(totalLength(clipboard_))));
}

// ------------------------------------------------ keyframes and thumbnails
// Reading the packets (not decoding them) is fast even for long videos.
void Editor::readKeyframes()
{
    auto *proc = new QProcess(this);
    keyframeProc_ = proc;
    const int gen = gen_;
    connect(proc, &QProcess::finished, this, [this, proc, gen] {
        proc->deleteLater();
        if (proc == keyframeProc_)
            keyframeProc_ = nullptr;
        if (gen != gen_ || !media_)
            return;
        std::vector<double> times;
        packets_.clear();
        for (const QByteArray &line : proc->readAllStandardOutput().split('\n')) {
            const QList<QByteArray> fields = line.trimmed().split(',');   // pts_time,dts_time,flags
            bool okPts = false, okDts = false;
            const double pts = fields.value(0).toDouble(&okPts) - media_->startTime;
            const double dts = fields.value(1).toDouble(&okDts) - media_->startTime;
            if (!okPts || fields.size() < 3)
                continue;
            packets_.emplace_back(pts, okDts ? dts : pts);
            if (fields[2].contains('K'))
                times.push_back(std::max(0.0, pts));
        }
        std::sort(packets_.begin(), packets_.end());
        std::sort(times.begin(), times.end());
        if (times.empty())
            times.push_back(0);
        keyframes_ = times;
        keyframesReady_ = true;
        timeline_->setKeyframes(keyframes_);
        updateActions();
        // thumbnails at keyframes, which are decoded on their own (fast), spread over the video
        const double spacing = media_->duration / MAX_THUMBNAILS;
        std::vector<double> picked;
        for (double k : keyframes_)
            if (picked.empty() || k - picked.back() >= spacing * 0.9)
                picked.push_back(k);
        // every other one first, so that the strip fills evenly
        std::vector<double> order;
        for (size_t first : {size_t(0), size_t(1)})
            for (size_t i = first; i < picked.size(); i += 2)
                order.push_back(picked[i]);
        thumbQueue_.assign(order.rbegin(), order.rend());   // taken from the back
        if (isVisible())
            nextThumbnail();
    });
    proc->start(ffprobe_, {"-v", "error", "-select_streams", QString::number(media_->videoIndex), "-show_entries",
                           "packet=pts_time,dts_time,flags", "-of", "csv=p=0", media_->path});
}

double Editor::copyStop(double end) const
{
    // frames are decoded at most a few frames before they are shown: look at the next second
    auto it = std::lower_bound(packets_.begin(), packets_.end(), std::pair{end - 1e-4, -1e9});
    double stop = end;
    for (; it != packets_.end() && it->first < end + 1; ++it)
        stop = std::min(stop, it->second);
    return stop;
}

// The frames the zoomed-in timeline shows: they replace the ones it wanted before (it moved on since).
void Editor::wantThumbnails(const QList<double> &sourceTimes)
{
    // not zoomed in, the ones at the keyframes come first: they fill the whole strip sooner
    if (!media_ || !keyframesReady_ || !isVisible() || (timeline_->zoom() <= 1.001 && !thumbQueue_.empty()))
        return;
    wantedThumbs_.clear();
    for (double t : sourceTimes)
        if (!requestedThumbs_.contains(qint64(std::llround(t * 1000))))
            wantedThumbs_.push_back(t);
    nextThumbnail();
}

void Editor::nextThumbnail()
{
    thumbsStarted_ = true;
    while (media_ && thumbProcs_.size() < THUMBNAIL_PROCESSES) {
        if (!wantedThumbs_.empty()) {   // what is on screen first
            const double t = wantedThumbs_.front();
            wantedThumbs_.erase(wantedThumbs_.begin());
            startThumbnail(t, true);
        } else if (!thumbQueue_.empty()) {
            const double t = thumbQueue_.back();
            thumbQueue_.pop_back();
            startThumbnail(t, false);
        } else {
            return;
        }
    }
}

// exact: that very frame (decoded from the keyframe before it); otherwise t is a keyframe, and only it is decoded
void Editor::startThumbnail(double t, bool exact)
{
    requestedThumbs_.insert(qint64(std::llround(t * 1000)));
    auto *proc = new QProcess(this);
    thumbProcs_ << proc;
    const int gen = gen_;
    connect(proc, &QProcess::finished, this, [this, proc, gen, t] {
        proc->deleteLater();
        thumbProcs_.removeOne(proc);
        if (gen != gen_)
            return;
        QImage image;
        if (image.loadFromData(proc->readAllStandardOutput(), "JPG"))
            timeline_->addThumbnail(t, image);
        nextThumbnail();
    });
    QStringList args{"-v", "error"};
    if (exact)   // half a frame before: rounded to milliseconds, t itself may fall after the frame and give the next one
        args << "-ss" << QString::number(std::max(0.0, t - 0.5 / std::max(1.0, media_->fps)), 'f', 3);
    else   // just after the keyframe: the seek lands on it
        args << "-ss" << QString::number(t + 0.001, 'f', 3) << "-noaccurate_seek";
    args << "-i" << media_->path << "-map" << QString("0:%1").arg(media_->videoIndex) << "-frames:v" << "1"
         << "-vf" << QString("scale=-2:%1").arg(THUMBNAIL_HEIGHT) << "-f" << "image2pipe" << "-c:v" << "mjpeg"
         << "-q:v" << "4" << "-";
    proc->start(ffmpeg_, args);
}
