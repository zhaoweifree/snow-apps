// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_IMAGESPLITEDITORWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_IMAGESPLITEDITORWIDGET_H

#include "snow_shot/presentation/imagesplitdocument.h"

#include <QStringList>
#include <QWidget>
#include <memory>

namespace snow_shot::presentation::components {

class ImageSplitPreviewCanvas final : public QWidget {
    Q_OBJECT
  public:
    explicit ImageSplitPreviewCanvas(ImageSplitDocument* document, QWidget* parent = nullptr);
    ~ImageSplitPreviewCanvas() override;
    void fitToCanvas();
    [[nodiscard]] qreal zoom() const;
    [[nodiscard]] QPointF offset() const;
    [[nodiscard]] QPointF imageToViewport(QPointF point) const;
    [[nodiscard]] QPointF viewportToImage(QPointF point) const;
    void setEditingEnabled(bool enabled);
    void retranslateUi();

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void changeEvent(QEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class ImageSplitEditorWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit ImageSplitEditorWidget(ImageSplitDocument* document, QWidget* parent = nullptr);
    ~ImageSplitEditorWidget() override;
    [[nodiscard]] ImageSplitPreviewCanvas* canvas() const;
    [[nodiscard]] bool configurationValid() const;
    [[nodiscard]] bool commitPendingSettings();
    [[nodiscard]] QWidget* outputDescriptionWidget() const;
    void setLoading(bool loading);
    void setExportBusy(bool busy);
    void retranslateUi();

  signals:
    void reloadRequested();
    void imageFilesDropped(const QStringList& paths);
    void validationChanged();

  protected:
    void changeEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::components

#endif
