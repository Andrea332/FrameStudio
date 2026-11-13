#pragma once

#include "editlist.h"

#include <QImage>
#include <QList>
#include <QMap>
#include <QWidget>

class QTimer;

#include <vector>

// The edited video as a strip of pieces with frame thumbnails, the playhead, the selection and the keyframes
// of the source. Left button: click or drag to move the playhead (the video follows), drag the selection or its
// edges to move or resize it, Shift+click to extend it, double-click to select a piece. Right button: drag to
// select, click for the menu. Ctrl+wheel zooms, the wheel scrolls.
class Timeline : public QWidget
{
    Q_OBJECT
    friend class TestVideoCompressor;

public:
    explicit Timeline(QWidget *parent = nullptr);

    void setPieces(const std::vector<Piece> &pieces);
    void setFrameDuration(double seconds);                 // positions snap to frames
    void setKeyframes(const std::vector<double> &sourceTimes);
    void addThumbnail(double sourceTime, const QImage &image);
    void clearThumbnails();

    double playhead() const { return playhead_; }
    void setPlayhead(double t);                            // also scrolls to keep it in view
    bool hasSelection() const;
    double selectionStart() const { return selFrom_; }
    double selectionEnd() const { return selTo_; }
    void setSelection(double from, double to);             // from == to: no selection

    void zoomBy(double factor);                            // around the playhead
    void zoomToFit();
    double zoom() const { return zoom_; }

    // what is on screen, for the overview bar under the zoomed-in timeline
    const std::vector<Piece> &pieces() const { return pieces_; }
    double duration() const;                               // of the edited video
    double scroll() const { return scroll_; }              // time at the left edge
    double visibleDuration() const;
    void setScroll(double t);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void playheadDragged(double t);                        // by the user
    void selectionChanged();
    void moveRequested(double from, double to, double at);
    void menuRequested(double t, const QPoint &globalPos);  // right click without dragging
    void viewChanged();                                    // zoom, scroll, pieces, playhead or selection
    // the frames that the strip shows, zoomed in, but has no thumbnail for (source times)
    void thumbnailsWanted(const QList<double> &sourceTimes);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Drag { None, Scrub, Select, StartEdge, EndEdge, Move };

    void changed(bool view);   // repaints; view: zoom, scroll or pieces changed, which may need thumbnails
    void findMissingThumbnails();
    double tileWidth() const;  // of a thumbnail on the strip, 0 until there is one
    QMap<double, QImage>::const_iterator nearestThumbnail(double sourceTime) const;
    double pixelsPerSecond() const;
    double timeAt(double x) const;
    double xAt(double t) const;
    double snapped(double t, bool toPlayhead = true) const;   // to piece edges (and the playhead) when close,
                                                              // otherwise to frames
    void clampScroll();
    void paintRuler(QPainter &p);
    void paintPieces(QPainter &p);

    std::vector<Piece> pieces_;
    std::vector<double> keyframes_;
    QMap<double, QImage> thumbnails_;   // by source time
    double frame_ = 1.0 / 30;
    double playhead_ = 0;
    double selFrom_ = 0;
    double selTo_ = 0;
    double zoom_ = 1;                   // 1: the whole video fits
    double scroll_ = 0;                 // seconds hidden on the left
    Drag drag_ = Drag::None;
    double anchor_ = 0;                 // time where a selection drag started
    double pressX_ = 0;
    bool moved_ = false;                // the mouse went a few pixels since it was pressed
    double dropAt_ = -1;                // where the selection being moved would go
    QTimer *thumbTimer_;                // looks for missing thumbnails once the view stops changing
};

// The whole edited video in a thin bar under the zoomed-in timeline: the part on screen is a box that can be
// dragged, and a click elsewhere brings it there. It shows the pieces, the selection and the playhead.
class TimelineOverview : public QWidget
{
    Q_OBJECT
    friend class TestVideoCompressor;

public:
    explicit TimelineOverview(Timeline *timeline, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    double timeAt(double x) const;
    double xAt(double t) const;
    bool inBox(double x) const;

    Timeline *timeline_;
    double grab_ = -1;   // where the box was grabbed, in seconds from its start
};
