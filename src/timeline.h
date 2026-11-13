#pragma once

#include "editlist.h"

#include <QImage>
#include <QMap>
#include <QWidget>

#include <vector>

// The edited video as a strip of pieces with frame thumbnails, the playhead, the selection and the keyframes
// of the source. Click to move the playhead, drag to select (Shift+click extends), drag the selection or its
// edges to move or resize it, double-click to select a piece, Ctrl+wheel to zoom, wheel to scroll.
class Timeline : public QWidget
{
    Q_OBJECT

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

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void playheadDragged(double t);                        // by the user
    void selectionChanged();
    void moveRequested(double from, double to, double at);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Drag { None, Select, StartEdge, EndEdge, Move };

    double duration() const;
    double pixelsPerSecond() const;
    double timeAt(double x) const;
    double xAt(double t) const;
    double snapped(double t) const;   // to piece edges and the playhead when close, otherwise to frames
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
    double dropAt_ = -1;                // where the selection being moved would go
};
