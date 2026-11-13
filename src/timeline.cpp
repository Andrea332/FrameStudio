#include "timeline.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int MARGIN = 8;          // left and right, so that the ends can be clicked
constexpr int RULER = 20;
constexpr int TRACK = 56;
constexpr int KEYFRAMES = 8;       // strip under the pieces with the keyframes
constexpr int HEIGHT = RULER + TRACK + KEYFRAMES + 6;
constexpr int SNAP_PIXELS = 6;
constexpr int EDGE_PIXELS = 5;     // how close to an edge of the selection grabs it
constexpr double MAX_ZOOM = 2000;
constexpr int OVERVIEW = 18;

QString rulerLabel(double t, double step)
{
    const int minutes = int(t) / 60;
    const double seconds = t - minutes * 60;
    const int decimals = step < 1 ? 1 : 0;
    return QString("%1:%2").arg(minutes).arg(seconds, decimals ? 4 : 2, 'f', decimals, QChar('0'));
}

} // namespace

Timeline::Timeline(QWidget *parent)
    : QWidget(parent), thumbTimer_(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    thumbTimer_->setSingleShot(true);
    thumbTimer_->setInterval(150);
    connect(thumbTimer_, &QTimer::timeout, this, &Timeline::findMissingThumbnails);
}

void Timeline::changed(bool view)
{
    update();
    emit viewChanged();
    if (view)
        thumbTimer_->start();
}

QSize Timeline::sizeHint() const
{
    return {600, HEIGHT};
}

QSize Timeline::minimumSizeHint() const
{
    return {200, HEIGHT};
}

void Timeline::setPieces(const std::vector<Piece> &pieces)
{
    pieces_ = pieces;
    playhead_ = std::min(playhead_, duration());
    selFrom_ = std::min(selFrom_, duration());
    selTo_ = std::min(selTo_, duration());
    clampScroll();
    changed(true);
}

void Timeline::setFrameDuration(double seconds)
{
    frame_ = seconds > 0 ? seconds : 1.0 / 30;
}

void Timeline::setKeyframes(const std::vector<double> &sourceTimes)
{
    keyframes_ = sourceTimes;
    update();
}

void Timeline::addThumbnail(double sourceTime, const QImage &image)
{
    thumbnails_.insert(sourceTime, image);
    update();
    thumbTimer_->start();   // what is still missing (the first one also tells the size of a thumbnail)
}

void Timeline::clearThumbnails()
{
    thumbnails_.clear();
    update();
}

double Timeline::duration() const
{
    double total = 0;
    for (const Piece &p : pieces_)
        total += p.length();
    return total;
}

double Timeline::pixelsPerSecond() const
{
    const double d = duration();
    return d > 0 ? (width() - 2 * MARGIN) / d * zoom_ : 1;
}

double Timeline::xAt(double t) const
{
    return MARGIN + (t - scroll_) * pixelsPerSecond();
}

double Timeline::timeAt(double x) const
{
    return std::clamp(scroll_ + (x - MARGIN) / pixelsPerSecond(), 0.0, duration());
}

double Timeline::snapped(double t, bool toPlayhead) const
{
    std::vector<double> magnets{0, duration()};
    if (toPlayhead)
        magnets.push_back(playhead_);
    double position = 0;
    for (const Piece &p : pieces_)
        magnets.push_back(position += p.length());
    for (double m : magnets)
        if (std::abs(xAt(m) - xAt(t)) <= SNAP_PIXELS)
            return m;
    return std::clamp(std::round(t / frame_) * frame_, 0.0, duration());
}

double Timeline::visibleDuration() const
{
    return duration() / zoom_;
}

void Timeline::clampScroll()
{
    scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, duration() - visibleDuration()));
}

void Timeline::setScroll(double t)
{
    scroll_ = t;
    clampScroll();
    changed(true);
}

void Timeline::setPlayhead(double t)
{
    playhead_ = std::clamp(t, 0.0, duration());
    const double x = xAt(playhead_);
    const bool scrolls = x < MARGIN || x > width() - MARGIN;
    if (scrolls) {   // keep it in view, a little after the left edge
        scroll_ = playhead_ - visibleDuration() * 0.1;
        clampScroll();
    }
    changed(scrolls);
}

bool Timeline::hasSelection() const
{
    return selTo_ - selFrom_ > frame_ / 2;
}

void Timeline::setSelection(double from, double to)
{
    selFrom_ = std::clamp(std::min(from, to), 0.0, duration());
    selTo_ = std::clamp(std::max(from, to), 0.0, duration());
    changed(false);
}

void Timeline::zoomBy(double factor)
{
    const double x = xAt(playhead_);
    zoom_ = std::clamp(zoom_ * factor, 1.0, MAX_ZOOM);
    scroll_ = playhead_ - (x - MARGIN) / pixelsPerSecond();   // the playhead stays where it is
    clampScroll();
    changed(true);
}

void Timeline::zoomToFit()
{
    zoom_ = 1;
    scroll_ = 0;
    changed(true);
}

void Timeline::resizeEvent(QResizeEvent *)
{
    clampScroll();
    changed(true);
}

// ------------------------------------------------ thumbnails
double Timeline::tileWidth() const
{
    if (thumbnails_.isEmpty())
        return 0;
    const QImage &any = thumbnails_.first();
    return std::max(16.0, TRACK * double(any.width()) / std::max(1, any.height()));
}

QMap<double, QImage>::const_iterator Timeline::nearestThumbnail(double sourceTime) const
{
    auto it = thumbnails_.lowerBound(sourceTime);
    if (it == thumbnails_.cend() || (it != thumbnails_.cbegin() && sourceTime - std::prev(it).key() < it.key() - sourceTime))
        it = std::prev(it);
    return it;
}

// Each thumbnail on the strip stands for the frames under it. The first ones are read at the keyframes,
// spread over the video: zoomed in, a thumbnail can be far from what it stands for (seconds earlier). Ask for
// the frames that are on screen instead.
void Timeline::findMissingThumbnails()
{
    const double tile = tileWidth();
    if (tile <= 0)
        return;
    const double pps = pixelsPerSecond();
    QList<double> wanted;
    double position = 0;
    for (const Piece &piece : pieces_) {
        const double left = xAt(position), right = xAt(position + piece.length());
        position += piece.length();
        if (right < 0 || left > width())
            continue;
        for (double x = std::max(left, std::floor(-left / tile) * tile + left); x < std::min(right, double(width()));
             x += tile) {
            const double t = std::clamp(piece.start + (x + tile / 2 - left) / pps, piece.start,
                                        std::max(piece.start, piece.end - frame_));
            const double frameTime = std::round(t / frame_) * frame_;
            const double near = nearestThumbnail(frameTime).key();
            if (std::abs(near - frameTime) > std::max(tile / pps / 2, frame_ * 0.75))
                wanted << frameTime;
        }
    }
    if (!wanted.isEmpty())
        emit thumbnailsWanted(wanted);
}

// ------------------------------------------------ painting
void Timeline::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Base));
    paintRuler(p);
    paintPieces(p);

    const QColor highlight = palette().color(QPalette::Highlight);
    if (hasSelection()) {
        const QRectF area(QPointF(xAt(selFrom_), 0), QPointF(xAt(selTo_), RULER + TRACK + KEYFRAMES));
        QColor fill = highlight;
        fill.setAlphaF(0.28f);
        p.fillRect(area, fill);
        p.setPen(QPen(highlight, 2));
        p.drawLine(area.topLeft(), area.bottomLeft());
        p.drawLine(area.topRight(), area.bottomRight());
    }
    if (drag_ == Drag::Move && dropAt_ >= 0) {   // where the selection would go
        p.setPen(QPen(highlight, 3));
        p.drawLine(QPointF(xAt(dropAt_), 0), QPointF(xAt(dropAt_), RULER + TRACK + KEYFRAMES));
    }
    const QColor red(229, 57, 53);
    const double x = xAt(playhead_);
    p.setPen(QPen(red, 2));
    p.drawLine(QPointF(x, 0), QPointF(x, RULER + TRACK + KEYFRAMES));
    QPainterPath head;   // the handle to grab it by
    head.moveTo(x - 7, 0);
    head.lineTo(x + 7, 0);
    head.lineTo(x + 7, 7);
    head.lineTo(x, 13);
    head.lineTo(x - 7, 7);
    head.closeSubpath();
    p.fillPath(head, red);
    if (hasFocus()) {
        p.setPen(QPen(highlight, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

void Timeline::paintRuler(QPainter &p)
{
    const double pps = pixelsPerSecond();
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    double step = steps[std::size(steps) - 1];
    for (double s : steps)
        if (s * pps >= 70) {   // room for a label
            step = s;
            break;
        }
    QColor text = palette().color(QPalette::Text);
    p.setFont(QFont(font().family(), std::max(7, font().pointSize() - 1)));
    const double first = std::floor(scroll_ / step) * step;
    const double last = std::min(duration(), timeAt(width()) + step);
    for (int i = 0; first + i * step / 2 <= last + 1e-9; ++i) {   // a major tick, then a minor one
        const double t = first + i * step / 2;
        const double x = xAt(t);
        if (x < 0 || x > width())
            continue;
        const bool major = i % 2 == 0;
        text.setAlphaF(major ? 0.7f : 0.35f);
        p.setPen(text);
        p.drawLine(QPointF(x, RULER - (major ? 7 : 4)), QPointF(x, RULER));
        if (major)
            p.drawText(QPointF(x + 3, RULER - 8), rulerLabel(t, step));
    }
}

void Timeline::paintPieces(QPainter &p)
{
    const double pps = pixelsPerSecond();
    const QRectF track(MARGIN, RULER, width() - 2 * MARGIN, TRACK);
    QColor keyColor = palette().color(QPalette::Text);
    keyColor.setAlphaF(0.55f);
    double position = 0;
    for (const Piece &piece : pieces_) {
        const QRectF r(QPointF(xAt(position), RULER), QPointF(xAt(position + piece.length()), RULER + TRACK));
        const double start = position;
        position += piece.length();
        if (r.right() < 0 || r.left() > width())
            continue;
        const QRectF shown = r.adjusted(1, 1, -1, -1);   // a gap between pieces shows where they were cut
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(shown, 4, 4);
        p.setClipPath(clip);
        p.fillRect(shown, palette().color(QPalette::Mid));
        if (const double tile = tileWidth(); tile > 0) {   // tiled as findMissingThumbnails() expects them
            for (double x = std::max(r.left(), std::floor(-r.left() / tile) * tile + r.left());
                 x < std::min(r.right(), double(width())); x += tile) {
                const double sourceTime = piece.start + (x + tile / 2 - r.left()) / pps;
                p.drawImage(QRectF(x, shown.top(), tile, shown.height()), nearestThumbnail(sourceTime).value());
            }
        }
        p.restore();
        p.setPen(QPen(palette().color(QPalette::Dark), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(shown, 4, 4);

        // keyframes of this piece (where a copy without re-encoding can start), under it
        p.setPen(QPen(keyColor, 1));
        const auto first = std::lower_bound(keyframes_.begin(), keyframes_.end(), piece.start);
        double lastX = -10;
        for (auto it = first; it != keyframes_.end() && *it < piece.end; ++it) {
            const double x = xAt(start + (*it - piece.start));
            if (x - lastX < 2 || x < 0 || x > width())
                continue;
            p.drawLine(QPointF(x, RULER + TRACK + 2), QPointF(x, RULER + TRACK + KEYFRAMES));
            lastX = x;
        }
    }
    if (pieces_.empty())
        p.fillRect(track, palette().color(QPalette::Mid));
}

// ------------------------------------------------ mouse
void Timeline::mousePressEvent(QMouseEvent *event)
{
    if (duration() <= 0)
        return;
    const double x = event->position().x();
    const double t = timeAt(x);
    pressX_ = x;
    moved_ = false;
    dropAt_ = -1;
    if (event->button() == Qt::RightButton) {   // a selection, or the menu if the mouse doesn't move
        drag_ = Drag::Select;
        anchor_ = snapped(t);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const bool nearPlayhead = std::abs(x - xAt(playhead_)) <= EDGE_PIXELS;
    if (!nearPlayhead && hasSelection() && std::abs(x - xAt(selFrom_)) <= EDGE_PIXELS) {
        drag_ = Drag::StartEdge;
    } else if (!nearPlayhead && hasSelection() && std::abs(x - xAt(selTo_)) <= EDGE_PIXELS) {
        drag_ = Drag::EndEdge;
    } else if (!nearPlayhead && hasSelection() && t > selFrom_ && t < selTo_ && event->position().y() >= RULER) {
        drag_ = Drag::Move;   // becomes a move once the mouse goes a few pixels
    } else if (event->modifiers() & Qt::ShiftModifier) {
        drag_ = Drag::Select;
        anchor_ = playhead_;
        moved_ = true;
        setSelection(anchor_, snapped(t));
        emit selectionChanged();
    } else {   // the playhead comes here and follows the mouse
        drag_ = Drag::Scrub;
        const double at = snapped(t, false);
        setPlayhead(at);
        emit playheadDragged(at);
    }
}

void Timeline::mouseMoveEvent(QMouseEvent *event)
{
    const double x = event->position().x();
    if (drag_ == Drag::None) {   // show what can be grabbed
        const bool playhead = std::abs(x - xAt(playhead_)) <= EDGE_PIXELS;
        const bool edge = hasSelection() && (std::abs(x - xAt(selFrom_)) <= EDGE_PIXELS
                                             || std::abs(x - xAt(selTo_)) <= EDGE_PIXELS);
        const double t = timeAt(x);
        const bool inside = hasSelection() && t > selFrom_ && t < selTo_ && event->position().y() >= RULER;
        setCursor(playhead || edge ? Qt::SizeHorCursor : inside ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    moved_ = moved_ || std::abs(x - pressX_) > 3;
    const double t = snapped(timeAt(x), drag_ != Drag::Scrub);
    switch (drag_) {
    case Drag::Scrub:
        setPlayhead(t);
        emit playheadDragged(t);
        break;
    case Drag::Select:
        if (!moved_)
            break;
        setSelection(anchor_, t);
        emit selectionChanged();
        break;
    case Drag::StartEdge:
        setSelection(t, selTo_);
        if (t > selTo_)
            drag_ = Drag::EndEdge;   // dragged past the other edge
        emit selectionChanged();
        break;
    case Drag::EndEdge:
        setSelection(selFrom_, t);
        if (t < selFrom_)
            drag_ = Drag::StartEdge;
        emit selectionChanged();
        break;
    case Drag::Move:
        if (std::abs(x - pressX_) > 4) {
            setCursor(Qt::ClosedHandCursor);
            dropAt_ = t;
            changed(false);
        }
        break;
    case Drag::None:
        break;
    }
}

void Timeline::mouseReleaseEvent(QMouseEvent *event)
{
    const Drag drag = drag_;
    drag_ = Drag::None;
    setCursor(Qt::ArrowCursor);
    if (event->button() == Qt::RightButton) {
        if (drag == Drag::Select && !moved_)
            emit menuRequested(timeAt(event->position().x()), event->globalPosition().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton || drag != Drag::Move)
        return;
    if (dropAt_ < 0) {   // a click inside the selection: move the playhead there
        const double t = snapped(timeAt(event->position().x()));
        setSelection(t, t);
        emit selectionChanged();
        setPlayhead(t);
        emit playheadDragged(t);
    } else if (dropAt_ < selFrom_ || dropAt_ > selTo_) {
        emit moveRequested(selFrom_, selTo_, dropAt_);
    }
    dropAt_ = -1;
    changed(false);
}

void Timeline::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const double t = timeAt(event->position().x());
    double position = 0;
    for (const Piece &piece : pieces_) {
        if (t >= position && t <= position + piece.length()) {
            setSelection(position, position + piece.length());
            emit selectionChanged();
            return;
        }
        position += piece.length();
    }
}

void Timeline::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (event->modifiers() & Qt::ControlModifier) {
        const double x = event->position().x();
        const double t = timeAt(x);
        zoom_ = std::clamp(zoom_ * std::pow(1.25, steps), 1.0, MAX_ZOOM);
        scroll_ = t - (x - MARGIN) / pixelsPerSecond();   // the time under the mouse stays there
    } else {
        const double steps2 = steps != 0 ? steps : event->angleDelta().x() / 120.0;
        scroll_ -= steps2 * visibleDuration() * 0.1;   // down: later in the video
    }
    clampScroll();
    changed(true);
    event->accept();
}

// ------------------------------------------------ overview
TimelineOverview::TimelineOverview(Timeline *timeline, QWidget *parent)
    : QWidget(parent), timeline_(timeline)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip("The whole video: the box is the part shown above. Drag it to move along the video.");
    connect(timeline, &Timeline::viewChanged, this, qOverload<>(&QWidget::update));
}

QSize TimelineOverview::sizeHint() const
{
    return {600, OVERVIEW};
}

QSize TimelineOverview::minimumSizeHint() const
{
    return {200, OVERVIEW};
}

double TimelineOverview::xAt(double t) const
{
    const double d = timeline_->duration();
    return MARGIN + (d > 0 ? t / d : 0) * (width() - 2 * MARGIN);
}

double TimelineOverview::timeAt(double x) const
{
    return std::clamp((x - MARGIN) / std::max(1, width() - 2 * MARGIN) * timeline_->duration(), 0.0,
                      timeline_->duration());
}

bool TimelineOverview::inBox(double x) const
{
    return x >= xAt(timeline_->scroll()) && x <= xAt(timeline_->scroll() + timeline_->visibleDuration());
}

void TimelineOverview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Base));
    const QRectF bar(MARGIN, 3, width() - 2 * MARGIN, height() - 6);
    QColor piece = palette().color(QPalette::Text);
    double position = 0;
    int index = 0;
    for (const Piece &pc : timeline_->pieces()) {   // the pieces, one shade each
        piece.setAlphaF(index++ % 2 ? 0.18f : 0.28f);
        p.fillRect(QRectF(QPointF(xAt(position), bar.top()), QPointF(xAt(position + pc.length()), bar.bottom())), piece);
        position += pc.length();
    }
    const QColor highlight = palette().color(QPalette::Highlight);
    if (timeline_->hasSelection()) {
        QColor fill = highlight;
        fill.setAlphaF(0.45f);
        p.fillRect(QRectF(QPointF(xAt(timeline_->selectionStart()), bar.top()),
                          QPointF(xAt(timeline_->selectionEnd()), bar.bottom())), fill);
    }
    p.setPen(QPen(QColor(229, 57, 53), 2));
    p.drawLine(QPointF(xAt(timeline_->playhead()), 0), QPointF(xAt(timeline_->playhead()), height()));
    // the part on screen
    const QRectF box(QPointF(xAt(timeline_->scroll()), 1),
                     QPointF(xAt(timeline_->scroll() + timeline_->visibleDuration()), height() - 1));
    QColor fill = highlight;
    fill.setAlphaF(0.18f);
    p.setBrush(fill);
    p.setPen(QPen(highlight, 2));
    p.drawRoundedRect(box.adjusted(1, 0, -1, 0).normalized(), 3, 3);
}

void TimelineOverview::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const double x = event->position().x();
    if (inBox(x)) {
        grab_ = timeAt(x) - timeline_->scroll();
    } else {   // brings the box here, centred
        grab_ = timeline_->visibleDuration() / 2;
        timeline_->setScroll(timeAt(x) - grab_);
    }
    setCursor(Qt::ClosedHandCursor);
}

void TimelineOverview::mouseMoveEvent(QMouseEvent *event)
{
    const double x = event->position().x();
    if (grab_ < 0) {
        setCursor(inBox(x) ? Qt::OpenHandCursor : Qt::PointingHandCursor);
        return;
    }
    timeline_->setScroll(timeAt(x) - grab_);
}

void TimelineOverview::mouseReleaseEvent(QMouseEvent *)
{
    grab_ = -1;
    setCursor(Qt::OpenHandCursor);
}
