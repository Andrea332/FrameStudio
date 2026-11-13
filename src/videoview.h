#pragma once

#include <QImage>
#include <QWidget>

class QVideoFrame;
class QVideoSink;

// Shows the frames that a media player sends to its sink, scaled to fit on black. It paints them itself
// (no native video window), so it scrolls with the window, shows in screenshots and works offscreen.
class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    QVideoSink *sink() const { return sink_; }
    const QImage &frame() const { return frame_; }   // the last frame received
    void clear();
    void setRotation(int degreesClockwise) { rotation_ = degreesClockwise; }

signals:
    void frameShown();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void onFrame(const QVideoFrame &frame);

    QVideoSink *sink_;
    QImage frame_;
    int rotation_ = 0;
};
