#include "videoview.h"

#include <QPainter>
#include <QVideoFrame>
#include <QVideoSink>

VideoView::VideoView(QWidget *parent)
    : QWidget(parent), sink_(new QVideoSink(this))
{
    setMinimumSize(240, 135);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(sink_, &QVideoSink::videoFrameChanged, this, &VideoView::onFrame);
}

void VideoView::clear()
{
    frame_ = QImage();
    update();
}

void VideoView::onFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    frame_ = frame.toImage();   // as displayed: the rotation of phone videos is applied
    update();
    emit frameShown();
}

void VideoView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull())
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSize size = frame_.size().scaled(this->size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    p.drawImage(target, frame_);
}
