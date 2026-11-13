#pragma once

#include "editlist.h"
#include "media.h"

#include <QWidget>

#include <optional>
#include <vector>

class QAbstractButton;
class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QProcess;
class QPushButton;
class QShortcut;
class QTimer;
class QToolButton;
class Timeline;
class VideoView;

// The Editor tab: plays the source video and edits it as a list of pieces (cut, copy, paste, delete, split,
// move), with undo. The compression encodes the edited video; "Save without re-encoding" copies it.
class Editor : public QWidget
{
    Q_OBJECT
    friend class TestVideoCompressor;

public:
    Editor(const QString &ffmpeg, const QString &ffprobe, QWidget *parent = nullptr);
    ~Editor() override;

    void setMedia(const std::optional<MediaInfo> &media);   // a new video: nothing edited
    const EditList &edits() const { return edits_; }
    bool edited() const;
    // source times of the keyframes, where a copy without re-encoding can start (empty until read)
    const std::vector<double> &keyframes() const { return keyframes_; }
    bool keyframesReady() const { return keyframesReady_; }
    // where a copy without re-encoding has to stop, in decoding time, to end at `end` (a source time): the
    // first frame shown from `end` on can be decoded before frames shown earlier (B-frames)
    double copyStop(double end) const;
    void setBusy(bool busy);   // while FFmpeg writes a file: no editing

signals:
    void editsChanged();
    void saveCopyRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // playback
    void togglePlay();
    void pause();
    void seek(double t);          // timeline position
    void stepFrames(int frames);
    void onPlayTick();
    void showTime();

    // editing
    void setStart();
    void setEnd();
    void selectAll();
    void clearSelection();
    void splitAtPlayhead();
    void cut();
    void copy();
    void paste();
    void deleteSelection();
    void moveSelection(double from, double to, double at);
    void undo();
    void redo();
    void reset();
    void showMenu(double t, const QPoint &globalPos);
    bool change(const std::vector<Piece> &before, const QString &what);   // after an edit: undo, display
    bool leavesNothing(double from, double to) const;
    void editsUpdated();
    void updateActions();

    // thumbnails and keyframes, read with FFmpeg in the background
    void readKeyframes();
    void nextThumbnail();

    QString ffmpeg_;
    QString ffprobe_;
    std::optional<MediaInfo> media_;
    EditList edits_;
    std::vector<Piece> playPieces_;               // what is played: the pieces, joined where they continue
    std::vector<std::vector<Piece>> undo_;
    std::vector<std::vector<Piece>> redo_;
    std::vector<Piece> clipboard_;
    double playhead_ = 0;
    int playing_ = -1;                            // index in playPieces_ while playing
    double jumpTarget_ = -1;                      // source time being sought to, until the player gets there
    bool busy_ = false;

    std::vector<double> keyframes_;
    std::vector<std::pair<double, double>> packets_;   // presentation and decoding time of each frame, by the first
    bool keyframesReady_ = false;
    QProcess *keyframeProc_ = nullptr;
    QProcess *thumbProc_ = nullptr;
    std::vector<double> thumbQueue_;              // source times still to read
    bool thumbsStarted_ = false;
    int gen_ = 0;                                 // discards results for a video that is no longer loaded

    QMediaPlayer *player_;
    QAudioOutput *audio_;
    QTimer *playTimer_;
    VideoView *view_;
    Timeline *timeline_;
    QToolButton *playBtn_;
    QToolButton *muteBtn_;
    QLabel *timeLabel_;
    QLabel *status_;
    QList<QAbstractButton *> editButtons_;
    QList<QShortcut *> shortcuts_;
    QToolButton *startBtn_;
    QToolButton *endBtn_;
    QToolButton *splitBtn_;
    QToolButton *cutBtn_;
    QToolButton *copyBtn_;
    QToolButton *pasteBtn_;
    QToolButton *deleteBtn_;
    QToolButton *undoBtn_;
    QToolButton *redoBtn_;
    QPushButton *resetBtn_;
    QPushButton *saveCopyBtn_;
};

QString fmtPrecise(double seconds);   // 1:23.40
