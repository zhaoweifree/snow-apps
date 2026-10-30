// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/collagepdfpagegrid.h"
#include "snow_shot/presentation/components/collagepdfsettingswidget.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input.h"
#include "widgets/input_number.h"
#include "widgets/segmented.h"
#include "widgets/select.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
using adqt::widgets::AdButton;
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdSegmented;
using adqt::widgets::AdSelect;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryApplicationStorage final {
  public:
    explicit TemporaryApplicationStorage(QApplication& application) {
        require(m_directory.isValid() &&
                    storage::ApplicationStorage::instance()
                        .initialize({m_directory.path(), m_directory.path(), 60000})
                        .success,
                "initialize temporary application storage without touching user settings");
        snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    }
    ~TemporaryApplicationStorage() {
        ScreenshotExportCoordinator::shared().shutdown();
        storage::ApplicationStorage::instance().shutdown();
    }

  private:
    QTemporaryDir m_directory;
};

template <class T> T* control(QWidget& owner, const char* name) {
    auto* result = owner.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, "the expected PDF settings control exists");
    return result;
}

AdFormItem* fieldFor(QWidget* controlWidget) {
    for (QWidget* parent = controlWidget->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* field = qobject_cast<AdFormItem*>(parent))
            return field;
    }
    throw std::runtime_error("the PDF numeric control belongs to a form field");
}

QImage image() {
    QImage source({12, 8}, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    source.setColorSpace(QColorSpace::SRgb);
    return source;
}

void refresh() {
    QApplication::processEvents();
}

bool uniformMargins(const CollagePdfSettings& settings, qreal value) {
    const auto equal = [value](qreal margin) {
        return margin == value || (std::isnan(margin) && std::isnan(value));
    };
    return equal(settings.marginTopMm) && equal(settings.marginBottomMm) &&
           equal(settings.marginLeftMm) && equal(settings.marginRightMm);
}

void validSettingsUpdateAndValidation() {
    CollageDocument document;
    CollagePdfSettingsWidget widget(&document);
    widget.resize(360, 850);
    widget.show();
    refresh();
    auto* paper = control<AdSelect>(widget, "collagePdfPaperSize");
    auto* orientation = control<AdSegmented>(widget, "collagePdfOrientation");
    auto* exportMode = control<AdSegmented>(widget, "collagePdfExportMode");
    auto* width = control<AdLineEdit>(widget, "collagePdfCustomWidth");
    auto* height = control<AdLineEdit>(widget, "collagePdfCustomHeight");
    auto* margin = control<AdLineEdit>(widget, "collagePdfMargin");
    auto* custom = control<QWidget>(widget, "collagePdfCustomPaper");
    int marginInputs = 0;
    for (const auto* input : widget.findChildren<AdLineEdit*>()) {
        if (input->objectName().startsWith(QStringLiteral("collagePdfMargin")))
            ++marginInputs;
    }
    require(marginInputs == 1 &&
                margin->accessibleName() ==
                    QCoreApplication::translate("CollagePdfSettingsWidget", "Page margin") &&
                fieldFor(margin)->label() ==
                    QCoreApplication::translate("CollagePdfSettingsWidget", "Page margin"),
            "one singular, accessible Page margin field replaces the four directional inputs");
    require(!custom->isVisible() &&
                paper->currentValue().toInt() == static_cast<int>(CollagePdfPaperSize::A4) &&
                uniformMargins(document.pdfSettings(), 10),
            "A4 with one ten-millimeter margin is selected by default and custom dimensions stay "
            "hidden");
    QStringList fields;
    QObject::connect(&widget, &CollagePdfSettingsWidget::settingsEdited, &widget,
                     [&](const QString& field) { fields.append(field); });
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    width->setText(QStringLiteral("123.75"));
    height->setText(QStringLiteral("234.5"));
    orientation->setCurrentValue(static_cast<int>(CollagePdfOrientation::Landscape));
    int marginChanges = 0;
    const auto changed = QObject::connect(&document, &CollageDocument::changed, &document, [&] {
        ++marginChanges;
        require(uniformMargins(document.pdfSettings(), 4.125),
                "all four margins become visible in one atomic document update");
    });
    margin->setText(QStringLiteral("4.125"));
    QObject::disconnect(changed);
    require(marginChanges == 1, "one margin edit emits one document change");
    exportMode->setCurrentValue(static_cast<int>(CollagePdfExportMode::PerPage));
    const auto settings = document.pdfSettings();
    require(custom->isVisible() && settings.customWidthMm == 123.75 &&
                settings.customHeightMm == 234.5 && uniformMargins(settings, 4.125) &&
                settings.orientation == CollagePdfOrientation::Landscape &&
                settings.exportMode == CollagePdfExportMode::PerPage &&
                document.lastValidPdfSettings() == settings,
            "valid decimal fields promptly update custom paper and one margin on every side");
    const auto layout = calculateCollagePdfPageLayout({12, 8}, settings);
    require(layout.pageMillimeters == QSizeF(234.5, 123.75) &&
                layout.contentMillimeters == QRectF(4.125, 4.125, 226.25, 115.5) &&
                layout.imageMillimeters.center() == layout.contentMillimeters.center(),
            "landscape custom paper keeps the same margin and centers within its equal-sided "
            "content area");
    require(fields.contains(QStringLiteral("paper_size")) &&
                fields.contains(QStringLiteral("custom_width_mm")) &&
                fields.contains(QStringLiteral("custom_height_mm")) &&
                fields.count(QStringLiteral("margin_mm")) == 1 &&
                !fields.contains(QStringLiteral("margin_top_mm")) &&
                !fields.contains(QStringLiteral("margin_bottom_mm")) &&
                !fields.contains(QStringLiteral("margin_left_mm")) &&
                !fields.contains(QStringLiteral("margin_right_mm")) &&
                fields.contains(QStringLiteral("orientation")) &&
                fields.contains(QStringLiteral("export_mode")),
            "a uniform margin emits one persistence alias while other fields remain independent");

    width->setText(QStringLiteral("0"));
    require(width->text() == QStringLiteral("0") && document.pdfSettings().customWidthMm == 0 &&
                width->status() == AdLineEdit::Status::Error &&
                fieldFor(width)->validateStatus() == AdFormItem::ValidateStatus::Error &&
                !fieldFor(width)->helpText().isEmpty() &&
                !width->accessibleDescription().isEmpty() &&
                document.lastValidPdfSettings() == settings,
            "invalid dimensions retain raw input with visible and accessible field validation");
    width->setText(QStringLiteral("123.75"));
    require(width->status() == AdLineEdit::Status::None && fieldFor(width)->helpText().isEmpty() &&
                width->accessibleDescription().isEmpty(),
            "correcting a field clears its validation feedback");
    for (const auto& draft :
         {QStringLiteral(""), QStringLiteral("-0.25"), QStringLiteral("not-a-number")}) {
        margin->setText(draft);
        const auto value =
            draft == QStringLiteral("-0.25") ? -0.25 : std::numeric_limits<qreal>::quiet_NaN();
        require(margin->text() == draft && uniformMargins(document.pdfSettings(), value) &&
                    margin->status() == AdLineEdit::Status::Error &&
                    fieldFor(margin)->validateStatus() == AdFormItem::ValidateStatus::Error &&
                    !fieldFor(margin)->helpText().isEmpty() &&
                    !margin->accessibleDescription().isEmpty() &&
                    document.lastValidPdfSettings() == settings,
                "empty, negative and nonnumeric margin drafts stay visible and retain the last "
                "valid page");
    }
    margin->setText(QStringLiteral("61.875"));
    require(margin->status() == AdLineEdit::Status::Error &&
                !fieldFor(margin)->helpText().isEmpty() &&
                validateCollagePdfSettings(document.pdfSettings())
                    .contains(QStringLiteral("margin_top_mm")) &&
                document.lastValidPdfSettings() == settings,
            "the single margin identifies an unusable content height at the exact boundary");
    margin->setText(QStringLiteral("118"));
    require(margin->status() == AdLineEdit::Status::Error &&
                validateCollagePdfSettings(document.pdfSettings())
                    .contains(QStringLiteral("margin_left_mm")),
            "the single margin also identifies an unusable content width");
    margin->setText(QStringLiteral("0"));
    const auto borderless = calculateCollagePdfPageLayout({12, 8}, document.pdfSettings());
    require(
        uniformMargins(document.pdfSettings(), 0) && margin->status() == AdLineEdit::Status::None &&
            fieldFor(margin)->helpText().isEmpty() && margin->accessibleDescription().isEmpty() &&
            borderless.contentMillimeters == QRectF(QPointF{}, borderless.pageMillimeters),
        "zero margin clears errors and exposes the complete page content area");
    margin->setLocale(QLocale(QStringLiteral("de_DE")));
    margin->setText(QStringLiteral("12,5"));
    require(uniformMargins(document.pdfSettings(), 12.5) &&
                margin->text() == QStringLiteral("12,5"),
            "locale decimal separators update all margins without rewriting the user's text");
}

void imageSizeSelection() {
    CollageDocument document;
    auto settings = document.pdfSettings();
    settings.orientation = CollagePdfOrientation::Landscape;
    settings.marginTopMm = settings.marginBottomMm = settings.marginLeftMm =
        settings.marginRightMm = 12.5;
    document.setPdfSettings(settings);
    CollagePdfSettingsWidget widget(&document);
    widget.resize(360, 850);
    widget.show();
    refresh();
    auto* paper = control<AdSelect>(widget, "collagePdfPaperSize");
    auto* orientation = control<AdSegmented>(widget, "collagePdfOrientation");
    auto* margin = control<AdLineEdit>(widget, "collagePdfMargin");
    auto* custom = control<QWidget>(widget, "collagePdfCustomPaper");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::ImageSize));
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    require(paper->currentText() == QStringLiteral("Image Size") &&
                document.pdfSettings() == settings && !custom->isVisible() &&
                !orientation->isVisible() && !margin->isVisible(),
            "image size is selectable and hides inactive paper controls without changing values");
    const std::array<const char*, 3> locales{"en_US", "zh_CN", "zh_TW"};
    const std::array expected{QStringLiteral("Image Size"), QStringLiteral("图像大小"),
                              QStringLiteral("圖像大小")};
    for (std::size_t index = 0; index < locales.size(); ++index) {
        QTranslator translator;
        require(
            translator.load(
                QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locales[index]))),
            "load the image-size option translation");
        QApplication::installTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QApplication::sendEvent(&widget, &languageChange);
        refresh();
        if (paper->currentText() != expected[index])
            throw std::runtime_error(
                QStringLiteral("Image size label for %1: expected '%2', got '%3'")
                    .arg(QString::fromLatin1(locales[index]), expected[index], paper->currentText())
                    .toStdString());
        require(document.pdfSettings() == settings,
                "retranslating image size keeps its selection and inactive control values");
        require(!orientation->isVisible(),
                "retranslating image size keeps paper orientation hidden");
        require(!margin->isVisible(), "retranslating image size keeps the paper margin hidden");
        QApplication::removeTranslator(&translator);
    }
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::A4));
    settings.paperSize = CollagePdfPaperSize::A4;
    require(document.pdfSettings() == settings && orientation->isVisible() && margin->isVisible() &&
                orientation->currentValue().toInt() ==
                    static_cast<int>(CollagePdfOrientation::Landscape) &&
                margin->text() == QStringLiteral("12.5"),
            "returning to a paper size restores the saved orientation and margin controls");
}

void invalidSelectionsHaveFieldErrors() {
    CollageDocument document;
    CollagePdfSettingsWidget widget(&document);
    widget.show();
    const auto invalid = [&](const char* name,
                             const std::function<void(CollagePdfSettings&)>& edit) {
        auto settings = CollagePdfSettings{};
        edit(settings);
        document.setPdfSettings(settings);
        refresh();
        auto* field = fieldFor(control<QWidget>(widget, name));
        require(field->validateStatus() == AdFormItem::ValidateStatus::Error &&
                    !field->helpText().isEmpty(),
                "an invalid selection has a visible error on its own form field");
        document.setPdfSettings({});
        refresh();
        require(field->validateStatus() == AdFormItem::ValidateStatus::None &&
                    field->helpText().isEmpty(),
                "correcting a selection clears its field error");
    };
    invalid("collagePdfPaperSize", [](CollagePdfSettings& settings) {
        settings.paperSize = static_cast<CollagePdfPaperSize>(-1);
    });
    invalid("collagePdfOrientation", [](CollagePdfSettings& settings) {
        settings.orientation = static_cast<CollagePdfOrientation>(-1);
    });
    invalid("collagePdfExportMode", [](CollagePdfSettings& settings) {
        settings.exportMode = static_cast<CollagePdfExportMode>(-1);
    });
}

void rawInvalidDraftSurvivesRefresh() {
    CollageDocument document;
    CollageEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    auto* output = control<AdSegmented>(editor, "collageImageType");
    auto* paper = control<AdSelect>(editor, "collagePdfPaperSize");
    auto* width = control<AdLineEdit>(editor, "collagePdfCustomWidth");
    auto* margin = control<AdLineEdit>(editor, "collagePdfMargin");
    output->setCurrentValue(QStringLiteral("pdf"));
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    document.append(image(), QStringLiteral("original"));
    width->setText(QStringLiteral("not-a-number"));
    const auto valid = document.lastValidPdfSettings();
    for (const auto& draft :
         {QStringLiteral("-4.5"), QStringLiteral(""), QStringLiteral("unfinished")}) {
        margin->setText(draft);
        const auto value =
            draft == QStringLiteral("-4.5") ? -4.5 : std::numeric_limits<qreal>::quiet_NaN();
        output->setCurrentValue(QStringLiteral("static"));
        output->setCurrentValue(QStringLiteral("pdf"));
        QEvent languageChange(QEvent::LanguageChange);
        QApplication::sendEvent(&editor, &languageChange);
        require(width->text() == QStringLiteral("not-a-number") && margin->text() == draft &&
                    std::isnan(document.pdfSettings().customWidthMm) &&
                    uniformMargins(document.pdfSettings(), value) &&
                    document.lastValidPdfSettings() == valid,
                "mode and language refreshes preserve the exact shared margin draft and last valid "
                "layout");
        document.clear();
        require(width->text() == QStringLiteral("not-a-number") && margin->text() == draft,
                "clearing images preserves incomplete paper input and the shared margin draft");
    }
    width->clear();
    require(width->text().isEmpty() && std::isnan(document.pdfSettings().customWidthMm),
            "empty dimensions remain incomplete instead of clamping to a minimum");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::A4));
    require(width->text().isEmpty(),
            "switching to a standard paper retains the hidden custom draft");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    require(width->text().isEmpty() && width->status() == AdLineEdit::Status::Error,
            "returning to custom paper restores validation of the incomplete custom draft");
    width->setText(QStringLiteral("100.25"));
    margin->setText(QStringLiteral("0"));
    require(document.lastValidPdfSettings() == document.pdfSettings() &&
                uniformMargins(document.pdfSettings(), 0),
            "corrected settings resume the live preview without losing independent fields");
}

void sidebarModesAndSelectedTransforms() {
    CollageDocument document;
    const auto id = document.append(image(), QStringLiteral("original"));
    const qint64 originalKey = document.items().first().image.cacheKey();
    CollageEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    refresh();
    auto* form = control<AdForm>(editor, "collageSettingsForm");
    auto* output = control<AdSegmented>(editor, "collageImageType");
    auto* pdf = control<CollagePdfSettingsWidget>(editor, "collagePdfSettings");
    const auto formItems = form->items();
    require(formItems.indexOf(form->field(QStringLiteral("output_type"))) <
                formItems.indexOf(form->field(QStringLiteral("collageDirection"))),
            "Image Type is placed before Layout Direction");
    require(document.outputType() == CollageOutputType::StaticImage &&
                editor.canvas()->isVisible() && !editor.pdfGrid()->isVisible() && !pdf->isVisible(),
            "new editor windows retain the static canvas and default image output");
    auto* border = control<adqt::widgets::AdInputNumber>(editor, "collageOuterBorder");
    border->setValue(17);
    const auto rasterSettings = document.settings();
    output->setCurrentValue(QStringLiteral("pdf"));
    refresh();
    require(document.outputType() == CollageOutputType::PdfDocument &&
                !editor.canvas()->isVisible() && editor.pdfGrid()->isVisible() &&
                pdf->isVisible() && !control<QWidget>(editor, "collageThumbnailRow")->isVisible(),
            "PDF mode displays the grid and PDF form and hides static canvas and thumbnails");
    for (const auto* name : {"collageDirection", "collageSizing", "collageOuterBorder",
                             "collageImageSpacing", "collageBackgroundColor"})
        require(!control<QWidget>(editor, name)->isVisible(),
                "PDF mode hides every static-only layout field");
    auto* toolbar = control<QWidget>(editor, "collagePdfImageToolbar");
    require(toolbar->parentWidget() == editor.pdfGrid() && toolbar->isVisible() &&
                pdf->findChildren<AdButton*>().isEmpty() &&
                !pdf->findChild<AdForm*>()->field(QStringLiteral("transforms")),
            "selected-page actions float over the PDF grid and leave the settings sidebar");
    const std::array<const char*, 6> pdfActions{
        "collagePdfRotateRight",  "collagePdfRotateLeft",     "collagePdfFlipHorizontal",
        "collagePdfFlipVertical", "collagePdfResetTransform", "collagePdfDeleteSelected"};
    const std::array<const char*, 6> staticActions{
        "collageRotateClockwise", "collageRotateCounterclockwise", "collageFlipHorizontal",
        "collageFlipVertical",    "collageResetTransform",         "collageDeleteSelected"};
    for (std::size_t index = 0; index < pdfActions.size(); ++index) {
        auto* pdfAction = control<AdButton>(editor, pdfActions[index]);
        auto* staticAction = control<AdButton>(editor, staticActions[index]);
        require(pdfAction->parentWidget() == toolbar && pdfAction->size() == staticAction->size() &&
                    pdfAction->buttonStyle() == staticAction->buttonStyle() &&
                    pdfAction->accentRole() == staticAction->accentRole() &&
                    pdfAction->toolTip() == staticAction->toolTip(),
                "PDF selection uses the same actions and button styling as the static toolbar");
    }
    const std::array<const char*, 4> transforms{"collagePdfRotateLeft", "collagePdfRotateRight",
                                                "collagePdfFlipHorizontal",
                                                "collagePdfFlipVertical"};
    for (const auto* name : transforms) {
        auto* button = control<AdButton>(editor, name);
        require(button->isEnabled() && !button->accessibleName().isEmpty() &&
                    !button->toolTip().isEmpty(),
                "selected-page transform controls are enabled and accessible");
        button->click();
    }
    require(document.items().first().id == id &&
                document.items().first().image.cacheKey() == originalKey &&
                document.items().first().revision == 5 &&
                document.items().first().transform == QTransform(-1, 0, 0, -1, 0, 0) &&
                document.settings() == rasterSettings,
            "PDF transform controls edit shared transformations without replacing originals or "
            "static settings");
    editor.setExportBusy(true);
    require(toolbar->isVisible() &&
                control<AdButton>(editor, "collagePdfPreviewImage")->isEnabled(),
            "export keeps the selected-page toolbar available for original-image preview");
    for (const auto* name : transforms)
        require(!control<AdButton>(editor, name)->isEnabled(),
                "export locks selected-page transformations");
    require(!output->isEnabled() && !control<AdLineEdit>(editor, "collagePdfMargin")->isEnabled(),
            "export locks both output mode and PDF form edits");
    editor.setExportBusy(false);
    require(toolbar->isVisible(), "the selected-page toolbar remains available after export");
    document.select(0);
    require(!toolbar->isVisible(), "the selected-page toolbar hides when selection is cleared");
    for (const auto* name : transforms)
        require(!control<AdButton>(editor, name)->isEnabled(),
                "transform controls are disabled when no page is selected");
    output->setCurrentValue(QStringLiteral("static"));
    require(document.settings() == rasterSettings && border->value() == 17 &&
                document.items().first().transform == QTransform(-1, 0, 0, -1, 0, 0) &&
                editor.canvas()->isVisible() && !editor.pdfGrid()->isVisible(),
            "returning to static output restores its form settings and shares edited image "
            "transformations");
}

void fieldEditsMergeAndRetranslate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create a settings configuration fixture");
    storage::ConfigurationStore configuration(temporary.filePath(QStringLiteral("settings.json")),
                                              true, true, 60000);
    CollageSettingsStore store(configuration);
    CollageDocument first, second;
    CollageEditorWidget firstEditor(&first), secondEditor(&second);
    const auto connectStore = [&](CollageEditorWidget& editor, CollageDocument& document) {
        QObject::connect(&editor, &CollageEditorWidget::settingsEdited, &editor,
                         [&document, &store](const QString& field) {
                             if (field.startsWith(QStringLiteral("pdf/")))
                                 static_cast<void>(
                                     store.writePdf(field.mid(4), document.pdfSettings()));
                             else
                                 static_cast<void>(store.write(field, document.settings()));
                         });
    };
    connectStore(firstEditor, first);
    connectStore(secondEditor, second);
    control<AdSelect>(firstEditor, "collagePdfPaperSize")
        ->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    control<AdLineEdit>(firstEditor, "collagePdfCustomWidth")->setText(QStringLiteral("180.5"));
    control<AdLineEdit>(firstEditor, "collagePdfCustomHeight")->setText(QStringLiteral("260.25"));
    control<AdLineEdit>(firstEditor, "collagePdfMargin")->setText(QStringLiteral("12.5"));
    control<AdLineEdit>(secondEditor, "collagePdfMargin")->setText(QStringLiteral("20.25"));
    const auto merged = store.readPdf();
    require(uniformMargins(first.pdfSettings(), 12.5) &&
                uniformMargins(second.pdfSettings(), 20.25) &&
                second.pdfSettings().paperSize == CollagePdfPaperSize::A4 &&
                merged.paperSize == CollagePdfPaperSize::Custom && merged.customWidthMm == 180.5 &&
                merged.customHeightMm == 260.25 && uniformMargins(merged, 20.25),
            "one window's latest uniform margin merges without overwriting another window's paper "
            "dimensions");
    auto* margin = control<AdLineEdit>(firstEditor, "collagePdfMargin");
    margin->setText(QStringLiteral("-2.25"));
    const auto persisted = store.readPdf();
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "load every supported PDF settings translation catalog");
        QApplication::installTranslator(&translator);
        refresh();
        const auto label = QCoreApplication::translate("CollagePdfSettingsWidget", "Page margin");
        require(margin->text() == QStringLiteral("-2.25") &&
                    margin->status() == AdLineEdit::Status::Error &&
                    margin->accessibleName() == label && fieldFor(margin)->label() == label &&
                    fieldFor(margin)->helpText() ==
                        QCoreApplication::translate("CollageDocument",
                                                    "Enter a nonnegative page margin.") &&
                    store.readPdf() == persisted,
                "language changes retranslate the singular margin and errors without losing drafts "
                "or persisting invalid input");
        QApplication::removeTranslator(&translator);
        refresh();
    }
}

void renderPdfEditor(const QString& path) {
    CollageDocument document;
    document.setOutputType(CollageOutputType::PdfDocument);
    const std::array<QSize, 8> sizes{QSize(480, 650), QSize(180, 2500), QSize(960, 360),
                                     QSize(500, 500), QSize(600, 800),  QSize(400, 560),
                                     QSize(720, 400), QSize(380, 700)};
    const QStringList names{
        QStringLiteral("Travel journal cover.png"),
        QStringLiteral("Long scrolling capture.png"),
        QStringLiteral("Panoramic scene.jpg"),
        QStringLiteral("Transparent artwork.png"),
        QStringLiteral("An exceptionally long image name that should be elided while its complete "
                       "name remains available in a tooltip.png"),
        QStringLiteral("Invoice scan.png"),
        QStringLiteral("Wide illustration.webp"),
        QStringLiteral("Final page.png")};
    QVector<quint64> ids;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        const QSize size = sizes[index];
        QImage source(size, QImage::Format_ARGB32_Premultiplied);
        const bool transparent = index == 3;
        source.fill(transparent ? QColor(Qt::transparent) : QColor(Qt::white));
        QPainter painter(&source);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = QColor::fromHsv(static_cast<int>(index) * 37, 110, 220);
        QLinearGradient gradient(QPointF(0, 0), QPointF(size.width(), size.height()));
        gradient.setColorAt(0, color.lighter(125));
        gradient.setColorAt(1, color.darker(110));
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        if (transparent)
            painter.drawEllipse(QRectF(35, 35, size.width() - 70, size.height() - 70));
        else
            painter.drawRect(source.rect());
        const int rows = index == 1 ? 28 : 7;
        for (int row = 0; row < rows; ++row) {
            const qreal top = qreal(size.height()) * (row + 1) / (rows + 2);
            painter.setBrush(QColor(255, 255, 255, row % 2 == 0 ? 210 : 120));
            painter.drawRoundedRect(QRectF(size.width() * 0.15, top, size.width() * 0.7,
                                           qreal(size.height()) / (rows + 2) * 0.4),
                                    8, 8);
        }
        painter.end();
        source.setColorSpace(QColorSpace::SRgb);
        const auto id = document.append(std::move(source), names[static_cast<qsizetype>(index)]);
        require(id != 0, "admit the PDF editor visual fixture");
        ids.append(id);
    }
    require(document.rotate(ids[2], true) && document.flip(ids[4], true),
            "visual fixture includes shared rotation and flip transformations");
    document.select(ids.first());
    CollageEditorWidget editor(&document);
    editor.resize(1200, 850);
    editor.show();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 250) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (auto* card = editor.pdfGrid()->findChild<QWidget*>(
            QStringLiteral("collagePdfPage_%1").arg(ids.first())))
        card->setFocus(Qt::OtherFocusReason);
    refresh();
    require(editor.grab().save(path), "save the PDF editor visual QA snapshot");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        TemporaryApplicationStorage temporaryStorage(application);
        const QStringList arguments = application.arguments();
        const auto renderArgument = arguments.indexOf(QStringLiteral("--render-path"));
        if (renderArgument >= 0 && renderArgument + 1 < arguments.size()) {
            renderPdfEditor(arguments[renderArgument + 1]);
            return 0;
        }
        validSettingsUpdateAndValidation();
        imageSizeSelection();
        invalidSelectionsHaveFieldErrors();
        rawInvalidDraftSurvivesRefresh();
        sidebarModesAndSelectedTransforms();
        fieldEditsMergeAndRetranslate();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
