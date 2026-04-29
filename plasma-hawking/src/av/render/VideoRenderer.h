#pragma once

#include "VideoFrameStore.h"

#include <QQuickFramebufferObject>

namespace av::render {

class VideoRenderer : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(QObject* frameSource READ frameSource WRITE setFrameSource NOTIFY frameSourceChanged)

public:
    explicit VideoRenderer(QQuickItem* parent = nullptr);

    Renderer* createRenderer() const override;

    QObject* frameSource() const;
    void setFrameSource(QObject* source);
    VideoFrameStore* frameStore() const;

signals:
    void frameSourceChanged();

private:
    QObject* m_frameSource{nullptr};
    QMetaObject::Connection m_frameChangedConnection;
    bool m_loggedFrameSourceAssigned{false};
    bool m_loggedFrameChangeObserved{false};
    bool m_loggedFrameSourceChange{false};
};

}  // namespace av::render
