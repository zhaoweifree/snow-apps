#include "theme/theme.h"
#include "widgets/alert.h"
#include "widgets/form.h"
#include "widgets/navigation_menu.h"

#include <QAccessible>
#include <QApplication>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>

using namespace adqt::widgets;

namespace {
QHash<QObject*, int> publishedRows;

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

void recordAccessibilityUpdate(QAccessibleEvent* event) {
  if (event->type() != QAccessible::TableModelChanged) {
    return;
  }
  auto* interface = event->accessibleInterface();
  if (auto* table = interface ? interface->tableInterface() : nullptr) {
    publishedRows.insert(event->object(), table->rowCount());
  }
}

void requiredMarkUsesLabelFont() {
  AdFormItem item;
  item.setLabel(QStringLiteral("Name"));
  item.setRequired(true);
  auto* mark = item.findChild<QLabel*>(QStringLiteral("ad-form-item-required-mark"));
  auto* label = item.findChild<QLabel*>(QStringLiteral("ad-form-item-label"));
  require(mark && label, "Form label and required mark must exist");
  require(mark->font() == label->font(), "Required mark must use the theme label font");
  require(mark->text() == QStringLiteral("*"), "Required mark must remain an asterisk");
  require(!mark->isHidden(), "Required mark must remain visible for required fields");
}

void labelAccessoryUsesSharedHeader() {
  AdForm form;
  form.setFormLayout(AdForm::FormLayout::Vertical);
  form.setLabelAlign(AdForm::LabelAlign::Left);
  form.setRequiredMark(AdForm::RequiredMark::Hidden);
  auto* input = new QLineEdit;
  auto* item = form.addField(QStringLiteral("Image width"), input, QStringLiteral("width"));
  auto* label = item->findChild<QLabel*>(QStringLiteral("ad-form-item-label"));
  auto* accessory = new QWidget;
  accessory->setFixedSize(90, 24);
  item->setLabelAccessoryWidget(accessory);
  form.resize(360, 200);
  form.show();
  QApplication::processEvents();
  for (const auto layout : {AdForm::FormLayout::Vertical, AdForm::FormLayout::Horizontal}) {
    form.setFormLayout(layout);
    item->setErrorMessages({QStringLiteral("Enter a valid width")});
    item->refresh();
    QApplication::processEvents();
    require(item->labelAccessoryWidget() == accessory && accessory->isVisible() &&
                accessory->parentWidget() == label->parentWidget() &&
                label->geometry().right() < accessory->geometry().left() &&
                accessory->geometry().right() == accessory->parentWidget()->width() - 1,
            "A label accessory must stay at the right of the shared label after layout refresh");
    form.setDisabled(true);
    require(!accessory->isEnabled(), "A label accessory must inherit the form's disabled state");
    form.setDisabled(false);
  }
  item->setNoStyle(true);
  require(!accessory->isVisible(), "An unstyled field must hide its label accessory");
  item->setNoStyle(false);
  item->setLabel({});
  require(!accessory->isVisible(), "An unlabeled field must hide its label accessory");
  item->setLabel(QStringLiteral("Image width"));
  require(accessory->isVisible(), "Restoring the label must restore its accessory");
  auto* replacement = new QWidget;
  replacement->setFixedSize(80, 24);
  item->setLabelAccessoryWidget(replacement);
  require(accessory->parentWidget() == nullptr && accessory->isHidden() &&
              item->labelAccessoryWidget() == replacement,
          "Replacing a label accessory must detach the previous widget");
  delete accessory;
  delete replacement;
  require(item->labelAccessoryWidget() == nullptr,
          "Deleting an accessory must clear the form item's reference");
  auto* ownedAccessory = new QWidget;
  const QPointer<QWidget> ownedGuard = ownedAccessory;
  item->setLabelAccessoryWidget(ownedAccessory);
  delete item;
  require(ownedGuard.isNull(), "The form item must own its current label accessory");
}

void menuPublishesRowsBeforeSelection() {
  const bool wasActive = QAccessible::isActive();
  QAccessible::setActive(true);
  const auto previousHandler = QAccessible::installUpdateHandler(recordAccessibilityUpdate);
  for (auto mode : {AdNavigationMenu::Mode::Inline, AdNavigationMenu::Mode::Vertical,
                    AdNavigationMenu::Mode::Horizontal}) {
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QStringLiteral("First")));
    model.appendRow(new QStandardItem(QStringLiteral("Second")));
    AdNavigationMenu menu;
    menu.setMode(mode);
    publishedRows.clear();
    menu.setModel(&model);
    const auto trees = menu.findChildren<QTreeView*>();
    require(trees.size() == 2, "Menu must have inline and vertical tree views");
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 2,
              "Menu must publish populated rows before returning from setModel");
    }
    menu.selectionModel()->select(model.index(0, 0), QItemSelectionModel::ClearAndSelect);
    menu.setCurrentIndex(model.index(0, 0));
    model.appendRow(new QStandardItem(QStringLiteral("Third")));
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 3,
              "Inserted rows must reach accessibility before the next selection");
    }
    model.clear();
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 0, "Cleared menus must publish zero rows");
    }
  }
  QAccessible::installUpdateHandler(previousHandler);
  QAccessible::setActive(wasActive);
}

void ancestorPaletteDoesNotOverrideSemanticAlertStyle() {
  QWidget parent;
  auto* layout = new QVBoxLayout(&parent);
  QPalette palette = parent.palette();
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#ff00ff")));
  palette.setColor(QPalette::Mid, QColor(QStringLiteral("#00ff00")));
  palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#0000ff")));
  parent.setPalette(palette);

  auto* alert = new AdAlert(&parent);
  alert->setSeverity(AdAlert::Severity::Warning);
  alert->setText(QStringLiteral("Permissions needed"));
  alert->setInformativeText(QStringLiteral("Review access in System Settings."));
  layout->addWidget(alert);
  parent.resize(480, 160);
  parent.show();
  QApplication::processEvents();

  const QImage rendered = alert->grab().toImage();
  const qreal scale = rendered.devicePixelRatio();
  const QPoint sample(qRound((alert->width() - 8) * scale), qRound(alert->height() / 2.0 * scale));
  const QColor actual = rendered.pixelColor(sample);
  const QColor expected =
      adqt::theme::ThemeManager::instance().resolve(alert).theme.palette.colorWarningBg;
  require(actual == expected,
          "An ancestor palette must not replace an alert's semantic warning background");
}

void descriptiveAlertUsesBalancedPadding() {
  AdAlert alert;
  alert.setText(QStringLiteral("Permissions needed"));
  alert.setInformativeText(QStringLiteral("Review access in System Settings."));
  const QMargins margins = alert.layout()->contentsMargins();
  require(margins.left() == margins.top() && margins.top() == margins.right() &&
              margins.right() == margins.bottom(),
          "Alerts with informative text must use balanced padding on every edge");
}
}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  requiredMarkUsesLabelFont();
  labelAccessoryUsesSharedHeader();
  menuPublishesRowsBeforeSelection();
  ancestorPaletteDoesNotOverrideSemanticAlertStyle();
  descriptiveAlertUsesBalancedPadding();
  return EXIT_SUCCESS;
}
