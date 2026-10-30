// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdfsettingswidget.h"

#include "snow_shot/presentation/collagedocument.h"
#include "widgets/form.h"
#include "widgets/input.h"
#include "widgets/segmented.h"
#include "widgets/select.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGridLayout>
#include <QLocale>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <limits>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdSegmented;
using adqt::widgets::AdSelect;

QString text(const char* source) {
    return QCoreApplication::translate("CollagePdfSettingsWidget", source);
}

[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Paper size"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Paper orientation"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Page margin"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Export mode"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A3"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A4"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A5"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "B5"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Letter"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Legal"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Image Size"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Portrait"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Landscape"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Width"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Height"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper width"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper height"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Combined export"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Per-page export"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "mm"),
};

struct NumberField final {
    const char* key;
    const char* name;
    const char* label;
    const char* accessibleName;
    double CollagePdfSettings::* member;
    AdFormItem* field = nullptr;
    AdLineEdit* input = nullptr;
};

double inputValue(const AdLineEdit* input) {
    bool ok = false;
    const QString entered = input->text().trimmed();
    double value = input->locale().toDouble(entered, &ok);
    if (!ok)
        value = QLocale::c().toDouble(entered, &ok);
    return ok && std::isfinite(value) ? value : std::numeric_limits<double>::quiet_NaN();
}
} // namespace

struct CollagePdfSettingsWidget::Impl final {
    CollagePdfSettingsWidget* owner;
    CollageDocument* document;
    AdForm* form;
    AdSelect* paper;
    AdSegmented* orientation;
    AdSegmented* exportMode;
    AdFormItem* paperField;
    AdFormItem* orientationField;
    AdFormItem* customField;
    AdFormItem* exportField;
    std::array<NumberField, 3> numbers{{
        {"custom_width_mm", "collagePdfCustomWidth", "Width", "Custom paper width",
         &CollagePdfSettings::customWidthMm},
        {"custom_height_mm", "collagePdfCustomHeight", "Height", "Custom paper height",
         &CollagePdfSettings::customHeightMm},
        {"margin_mm", "collagePdfMargin", "Page margin", "Page margin",
         &CollagePdfSettings::marginTopMm},
    }};
    bool syncing = false;

    void edit(const QString& field, const std::function<void(CollagePdfSettings&)>& change) {
        if (syncing || !owner->isEnabled())
            return;
        auto settings = document->pdfSettings();
        change(settings);
        document->setPdfSettings(settings);
        emit owner->settingsEdited(field);
    }

    void refresh() {
        syncing = true;
        const auto settings = document->pdfSettings();
        paper->setCurrentValue(static_cast<int>(settings.paperSize));
        orientation->setCurrentValue(static_cast<int>(settings.orientation));
        exportMode->setCurrentValue(static_cast<int>(settings.exportMode));
        customField->setItemHidden(settings.paperSize != CollagePdfPaperSize::Custom);
        const bool imageSize = settings.paperSize == CollagePdfPaperSize::ImageSize;
        orientationField->setItemHidden(imageSize);
        const auto errors = validateCollagePdfSettings(settings);
        const auto showError = [&errors](AdFormItem* field, const QString& key) {
            const auto error = errors.value(key);
            field->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                     : AdFormItem::ValidateStatus::Error);
            field->setHelpText(error);
        };
        showError(paperField, QStringLiteral("paper_size"));
        showError(orientationField, QStringLiteral("orientation"));
        showError(exportField, QStringLiteral("export_mode"));
        paper->setStatus(errors.contains(QStringLiteral("paper_size")) ? AdSelect::Status::Error
                                                                       : AdSelect::Status::None);
        for (auto& number : numbers) {
            const QSignalBlocker blocker(number.input);
            const double value = settings.*number.member;
            const double entered = inputValue(number.input);
            // Preserve exact user text, including an empty or incomplete numeric draft.
            if (std::isfinite(value) && (!std::isfinite(entered) || entered != value))
                number.input->setText(number.input->locale().toString(value, 'g', 12));
            else if (!std::isfinite(value) && std::isfinite(entered))
                number.input->clear();
            QString error = errors.value(QString::fromLatin1(number.key));
            if (number.member == &CollagePdfSettings::marginTopMm) {
                number.field->setItemHidden(imageSize);
                QStringList marginErrors;
                for (const auto* key :
                     {"margin_top_mm", "margin_bottom_mm", "margin_left_mm", "margin_right_mm"}) {
                    const auto message = errors.value(QString::fromLatin1(key));
                    if (!message.isEmpty() && !marginErrors.contains(message))
                        marginErrors.append(message);
                }
                error = marginErrors.join(QLatin1Char('\n'));
            }
            number.input->setStatus(error.isEmpty() ? AdLineEdit::Status::None
                                                    : AdLineEdit::Status::Error);
            number.field->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                            : AdFormItem::ValidateStatus::Error);
            number.field->setHelpText(error);
            number.input->setAccessibleDescription(error);
        }
        syncing = false;
    }
};

CollagePdfSettingsWidget::CollagePdfSettingsWidget(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePdfSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    d.form = new AdForm(this);
    d.form->setFormLayout(AdForm::FormLayout::Vertical);
    d.form->setRequiredMark(AdForm::RequiredMark::Hidden);
    d.form->setColon(false);
    layout->addWidget(d.form);
    d.paper = new AdSelect(d.form);
    d.paper->setObjectName(QStringLiteral("collagePdfPaperSize"));
    d.paperField = d.form->addField({}, d.paper, QStringLiteral("paper_size"));
    d.paperField->setValidateOnChange(false);
    const auto segmented = [&d](const char* name) {
        auto* control = new AdSegmented(d.form);
        control->setObjectName(QString::fromLatin1(name));
        control->setDistribution(AdSegmented::Distribution::Fill);
        control->addOption({}, 0);
        control->addOption({}, 1);
        return control;
    };
    d.orientation = segmented("collagePdfOrientation");
    d.orientationField = d.form->addField({}, d.orientation, QStringLiteral("orientation"));
    d.orientationField->setValidateOnChange(false);
    const auto input = [&d](std::size_t index, QWidget* parent) {
        auto& number = d.numbers[index];
        number.input = new AdLineEdit(parent);
        number.input->setObjectName(QString::fromLatin1(number.name));
        number.input->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
        QObject::connect(number.input, &QLineEdit::textChanged, d.owner,
                         [&d, index](const QString&) {
                             auto& edited = d.numbers[index];
                             const double value = inputValue(edited.input);
                             d.edit(QString::fromLatin1(edited.key),
                                    [&edited, value](CollagePdfSettings& settings) {
                                        settings.*edited.member = value;
                                        if (edited.member == &CollagePdfSettings::marginTopMm) {
                                            settings.marginBottomMm = value;
                                            settings.marginLeftMm = value;
                                            settings.marginRightMm = value;
                                        }
                                    });
                         });
        return number.input;
    };
    const auto customPaper = [&d, &input] {
        auto* content = new QWidget(d.form);
        content->setObjectName(QStringLiteral("collagePdfCustomPaper"));
        auto* grid = new QGridLayout(content);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(0);
        for (int index = 0; index < 2; ++index) {
            auto& number = d.numbers[static_cast<std::size_t>(index)];
            number.field = new AdFormItem(content);
            number.field->setItemLayout(AdFormItem::ItemLayout::Vertical);
            number.field->setValidateOnChange(false);
            number.field->setControlWidget(input(static_cast<std::size_t>(index), number.field));
            grid->addWidget(number.field, 0, index);
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        return content;
    };
    d.customField = d.form->addField({}, customPaper(), QStringLiteral("custom"));
    auto& margin = d.numbers[2];
    margin.field = d.form->addField({}, input(2, d.form), QStringLiteral("margin_mm"));
    margin.field->setValidateOnChange(false);
    d.exportMode = segmented("collagePdfExportMode");
    d.exportField = d.form->addField({}, d.exportMode, QStringLiteral("export_mode"));
    d.exportField->setValidateOnChange(false);
    connect(d.paper, &AdSelect::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("paper_size"), [&value](CollagePdfSettings& settings) {
            settings.paperSize = static_cast<CollagePdfPaperSize>(value.toInt());
        });
    });
    connect(d.orientation, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("orientation"), [&value](CollagePdfSettings& settings) {
            settings.orientation = static_cast<CollagePdfOrientation>(value.toInt());
        });
    });
    connect(d.exportMode, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("export_mode"), [&value](CollagePdfSettings& settings) {
            settings.exportMode = static_cast<CollagePdfExportMode>(value.toInt());
        });
    });
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    retranslateUi();
}

CollagePdfSettingsWidget::~CollagePdfSettingsWidget() = default;

void CollagePdfSettingsWidget::retranslateUi() {
    auto& d = *m_impl;
    d.syncing = true;
    constexpr std::array<const char*, 8> papers{"A3",     "A4",    "A5",     "B5",
                                                "Letter", "Legal", "Custom", "Image Size"};
    QVector<AdSelect::Option> options;
    for (std::size_t index = 0; index < papers.size(); ++index)
        options.append({static_cast<int>(index), text(papers[index])});
    d.paper->setOptions(options);
    d.paperField->setLabel(text("Paper size"));
    d.paper->setAccessibleName(text("Paper size"));
    d.orientationField->setLabel(text("Paper orientation"));
    d.orientation->setAccessibleName(text("Paper orientation"));
    d.orientation->setOptionLabel(0, text("Portrait"));
    d.orientation->setOptionLabel(1, text("Landscape"));
    d.customField->setLabel(text("Custom paper"));
    d.exportField->setLabel(text("Export mode"));
    d.exportMode->setAccessibleName(text("Export mode"));
    d.exportMode->setOptionLabel(0, text("Combined export"));
    d.exportMode->setOptionLabel(1, text("Per-page export"));
    for (auto& number : d.numbers) {
        number.field->setLabel(text(number.label));
        number.input->setAccessibleName(text(number.accessibleName));
        number.input->setSuffixText(text("mm"));
    }
    d.syncing = false;
    d.refresh();
}

void CollagePdfSettingsWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

} // namespace snow_shot::presentation::components
