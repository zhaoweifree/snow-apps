#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QRadioButton>
#include <QHBoxLayout>
#include <QtTest>

#include <type_traits>

#include "widgets/checkbox.h"
#include "widgets/checkbox_group.h"
#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/switch.h"

namespace {

// Qt 6 renamed `QCheckBox::stateChanged(int)` to `checkStateChanged(Qt::CheckState)`.
// This suite asserts the *native* signal: its name, its order relative to
// `toggled`, and that it honours `blockSignals`. Aliasing one spelling onto the
// other inside the test would assert the alias instead of Qt, so each version
// connects the name it actually ships, and the lambda always sees a CheckState.
//
// The Qt 5 branch has to forward by hand because the native signal carries an
// `int` there, and most slots in this file take no argument at all (Qt lets a
// connection drop trailing parameters; a hand-written call cannot). Dispatching
// on the slot's own arity keeps both spellings connectable.
template <typename Slot>
QMetaObject::Connection connectNativeCheckState(adqt::widgets::AdCheckbox* box,
                                                const QObject* context, Slot slot) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return QObject::connect(box, &adqt::widgets::AdCheckbox::checkStateChanged, context, slot);
#else
  return QObject::connect(box, &adqt::widgets::AdCheckbox::stateChanged, context,
                          [slot](int state) {
                            if constexpr (std::is_invocable_v<Slot&, Qt::CheckState>) {
                              slot(static_cast<Qt::CheckState>(state));
                            } else {
                              Q_UNUSED(state);
                              slot();
                            }
                          });
#endif
}

QAbstractButton* makeButton(int kind) {
  QAbstractButton* button = nullptr;
  if (kind == 0) {
    button = new adqt::widgets::AdSwitch;
  } else if (kind == 1) {
    button = new adqt::widgets::AdCheckbox;
  } else {
    button = new adqt::widgets::AdRadio;
  }
  button->setText(QStringLiteral("Control"));
  button->resize(button->sizeHint().expandedTo(QSize(44, 24)));
  return button;
}

void sendMouse(QWidget* widget, QEvent::Type type, const QPointF& point) {
  QMouseEvent event(type, point, point, Qt::LeftButton,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

class RetireSiblingOnEvent final : public QObject {
 public:
  QEvent::Type trigger = QEvent::None;
  QPointer<QObject> sibling;
  int notifications = 0;

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == trigger && sibling) {
      ++notifications;
      delete sibling.data();
    }
    return false;
  }
};

}  // namespace

class ButtonSignalLifetimeTests final : public QObject {
  Q_OBJECT

 private slots:
  void activationAllowsOwnerDeletion();
  void pressAllowsOwnerDeletion();
  void switchDragAllowsOwnerDeletion();
  void checkboxToggleUsesDeferredDeletion();
  void checkboxTransitionsPreserveNativeSignals();
  void checkboxGroupSelectionAllowsGroupDeletion();
  void nativeArrowNavigationMatchesQtBehavior();
  void radioGroupSelectionAllowsOwnerDeletion();
  void radioGroupNavigationAllowsOwnerDeletion();
  void radioGroupPropertyPropagationAllowsOwnerDeletion();
  void groupDestructionAllowsSiblingDeletion();
};

void ButtonSignalLifetimeTests::groupDestructionAllowsSiblingDeletion() {
  {
    adqt::widgets::AdCheckbox first;
    QPointer<adqt::widgets::AdCheckbox> second = new adqt::widgets::AdCheckbox;
    auto* group = new adqt::widgets::AdCheckboxGroup;
    group->addCheckbox(&first, 1);
    group->addCheckbox(second, 2);
    group->setEnabled(false);
    second->setChecked(true);
    RetireSiblingOnEvent filter;
    filter.trigger = QEvent::EnabledChange;
    filter.sibling = second;
    first.installEventFilter(&filter);
    int groupNotifications = 0;
    connect(group, &adqt::widgets::AdCheckboxGroup::valuesChanged, this,
            [&] { ++groupNotifications; });
    delete group;
    QVERIFY(second.isNull());
    QCOMPARE(filter.notifications, 1);
    QCOMPARE(groupNotifications, 0);
    QVERIFY(first.isEnabled());
  }
  {
    adqt::widgets::AdRadio first;
    QPointer<adqt::widgets::AdRadio> second = new adqt::widgets::AdRadio;
    auto* group = new adqt::widgets::AdRadioButtonGroup;
    group->addButton(&first, 1);
    group->addButton(second, 2);
    // The detached group's refresh must restore its managed cursor flag.
    first.setAttribute(Qt::WA_SetCursor, false);
    RetireSiblingOnEvent filter;
    filter.trigger = QEvent::CursorChange;
    filter.sibling = second;
    first.installEventFilter(&filter);
    delete group;
    QVERIFY(second.isNull());
    QCOMPARE(filter.notifications, 1);
    QVERIFY(first.autoExclusive());
  }
}

void ButtonSignalLifetimeTests::activationAllowsOwnerDeletion() {
  for (const int kind : {0, 1, 2}) {
    // Checkbox toggled runs inside Qt's native transition and uses deferred deletion below.
    for (const int signal : (kind == 1 ? QList<int>{1, 2} : QList<int>{0, 1, 2})) {
      for (const bool keyboard : {false, true}) {
        QPointer<QAbstractButton> button = makeButton(kind);
        const auto deleteOwner = [&] { delete button.data(); };
        if (signal == 0) {
          connect(button, &QAbstractButton::toggled, this, deleteOwner);
        } else if (signal == 1) {
          connect(button, &QAbstractButton::released, this, deleteOwner);
        } else {
          connect(button, &QAbstractButton::clicked, this, deleteOwner);
        }
        if (keyboard) {
          QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
          QCoreApplication::sendEvent(button, &press);
          QVERIFY(button);
          QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
          QCoreApplication::sendEvent(button, &release);
        } else {
          const QPointF point = button->rect().center();
          sendMouse(button, QEvent::MouseButtonPress, point);
          QVERIFY(button);
          sendMouse(button, QEvent::MouseButtonRelease, point);
        }
        QVERIFY(button.isNull());
      }
    }
  }
  QPointer<adqt::widgets::AdSwitch> button = new adqt::widgets::AdSwitch;
  connect(button, &QAbstractButton::clicked, this, [&] { delete button.data(); });
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QCoreApplication::sendEvent(button, &press);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
  QCoreApplication::sendEvent(button, &release);
  QVERIFY(button.isNull());
}

void ButtonSignalLifetimeTests::pressAllowsOwnerDeletion() {
  for (const int kind : {0, 1, 2}) {
    QPointer<QAbstractButton> button = makeButton(kind);
    connect(button, &QAbstractButton::pressed, this, [&] { delete button.data(); });
    sendMouse(button, QEvent::MouseButtonPress, button->rect().center());
    QVERIFY(button.isNull());
  }
}

void ButtonSignalLifetimeTests::switchDragAllowsOwnerDeletion() {
  for (const int signal : {0, 1, 2}) {
    QPointer<adqt::widgets::AdSwitch> button = new adqt::widgets::AdSwitch;
    button->resize(button->sizeHint());
    const auto deleteOwner = [&] { delete button.data(); };
    if (signal == 0) {
      connect(button, &QAbstractButton::released, this, deleteOwner);
    } else if (signal == 1) {
      connect(button, &QAbstractButton::toggled, this, deleteOwner);
    } else {
      connect(button, &QAbstractButton::clicked, this, deleteOwner);
    }
    const QPointF start(8, button->height() / 2.0);
    const QPointF end(button->width() - 2, button->height() / 2.0);
    sendMouse(button, QEvent::MouseButtonPress, start);
    QVERIFY(button);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(button, &move);
    QVERIFY(button);
    sendMouse(button, QEvent::MouseButtonRelease, end);
    QVERIFY(button.isNull());
  }
}

void ButtonSignalLifetimeTests::radioGroupSelectionAllowsOwnerDeletion() {
  using adqt::widgets::AdRadio;
  using adqt::widgets::AdRadioButtonGroup;
  for (const bool programmatic : {false, true}) {
    QPointer<QWidget> owner = new QWidget;
    QPointer<AdRadioButtonGroup> group = new AdRadioButtonGroup(owner);
    QPointer<AdRadio> radio = new AdRadio(owner);
    group->addButton(radio, 1);
    if (programmatic) {
      connect(radio, &AdRadio::toggled, this, [&] { delete owner.data(); });
      group->setCheckedId(1);
    } else {
      connect(group, &AdRadioButtonGroup::checkedIdChanged, this, [&] { delete owner.data(); });
      radio->setChecked(true);
    }
    QVERIFY(owner.isNull());
    QVERIFY(group.isNull());
    QVERIFY(radio.isNull());
  }

  AdRadio radio;
  QPointer<AdRadioButtonGroup> group = new AdRadioButtonGroup;
  group->addButton(&radio, 1);
  connect(&radio, &AdRadio::toggled, this, [&] { delete group.data(); });
  group->setCheckedId(1);
  QVERIFY(group.isNull());
  QVERIFY(radio.group() == nullptr);
  QVERIFY(radio.autoExclusive());
  radio.setAutoExclusive(false);
  radio.setChecked(false);
  QVERIFY(!radio.isChecked());
}

void ButtonSignalLifetimeTests::checkboxToggleUsesDeferredDeletion() {
  for (const bool keyboard : {false, true}) {
    QPointer<QWidget> owner = new QWidget;
    QPointer<adqt::widgets::AdCheckbox> checkbox = new adqt::widgets::AdCheckbox(owner);
    checkbox->resize(checkbox->sizeHint());
    connect(checkbox, &QAbstractButton::toggled, owner, &QObject::deleteLater);
    if (keyboard) {
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
      QCoreApplication::sendEvent(checkbox, &press);
      QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
      QCoreApplication::sendEvent(checkbox, &release);
    } else {
      const QPointF point = checkbox->rect().center();
      sendMouse(checkbox, QEvent::MouseButtonPress, point);
      sendMouse(checkbox, QEvent::MouseButtonRelease, point);
    }
    QVERIFY(owner);
    QVERIFY(checkbox && checkbox->isChecked());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(owner.isNull());
    QVERIFY(checkbox.isNull());
  }
}

void ButtonSignalLifetimeTests::checkboxTransitionsPreserveNativeSignals() {
  using adqt::widgets::AdCheckbox;
  AdCheckbox checkbox;
  QStringList notifications;
  connect(&checkbox, &AdCheckbox::toggled, this,
          [&] { notifications.append(QStringLiteral("toggled")); });
  connectNativeCheckState(&checkbox, this,
          [&] { notifications.append(QStringLiteral("state")); });
  checkbox.setChecked(true);
  QCOMPARE(notifications, QStringList({QStringLiteral("state"), QStringLiteral("toggled")}));
  checkbox.setChecked(false);
  notifications.clear();
  checkbox.setTristate(true);
  checkbox.click();
  QCOMPARE(checkbox.checkState(), Qt::PartiallyChecked);
  QCOMPARE(notifications, QStringList({QStringLiteral("toggled"), QStringLiteral("state")}));

  QButtonGroup liveGroup;
  AdCheckbox blockedCheckbox;
  liveGroup.addButton(&blockedCheckbox, 1);
  blockedCheckbox.blockSignals(true);
  int ownNotifications = 0;
  int groupNotifications = 0;
  connect(&blockedCheckbox, &AdCheckbox::toggled, this, [&] { ++ownNotifications; });
  connectNativeCheckState(&blockedCheckbox, this, [&] { ++ownNotifications; });
  connect(&liveGroup, &QButtonGroup::idToggled, this, [&] { ++groupNotifications; });
  blockedCheckbox.setCheckState(Qt::Checked);
  QCOMPARE(ownNotifications, 0);
  QCOMPARE(groupNotifications, 1);
  QVERIFY(blockedCheckbox.signalsBlocked());
  liveGroup.blockSignals(true);
  blockedCheckbox.blockSignals(false);
  liveGroup.setExclusive(false);
  blockedCheckbox.setCheckState(Qt::Unchecked);
  QCOMPARE(ownNotifications, 2);
  QCOMPARE(groupNotifications, 1);
  QVERIFY(liveGroup.signalsBlocked());

  QButtonGroup exclusiveGroup;
  AdCheckbox oldChecked;
  AdCheckbox nextChecked;
  oldChecked.setTristate(true);
  exclusiveGroup.addButton(&oldChecked, 1);
  exclusiveGroup.addButton(&nextChecked, 2);
  oldChecked.setCheckState(Qt::Checked);
  QStringList peerNotifications;
  connect(&oldChecked, &AdCheckbox::toggled, this,
          [&] { peerNotifications.append(QStringLiteral("toggled")); });
  connectNativeCheckState(&oldChecked, this,
          [&] { peerNotifications.append(QStringLiteral("state")); });
  nextChecked.setCheckState(Qt::Checked);
  QCOMPARE(peerNotifications, QStringList({QStringLiteral("toggled"), QStringLiteral("state")}));

  QButtonGroup refusingGroup;
  AdCheckbox exclusivePartial;
  refusingGroup.addButton(&exclusivePartial, 1);
  exclusivePartial.setCheckState(Qt::PartiallyChecked);
  int refusedNotifications = 0;
  connectNativeCheckState(&exclusivePartial, this, [&] { ++refusedNotifications; });
  connect(&exclusivePartial, &AdCheckbox::toggled, this, [&] { ++refusedNotifications; });
  exclusivePartial.setChecked(false);
  QCOMPARE(exclusivePartial.checkState(), Qt::PartiallyChecked);
  QCOMPARE(refusedNotifications, 0);
  exclusivePartial.setChecked(true);
  QCOMPARE(exclusivePartial.checkState(), Qt::Checked);
  QCOMPARE(refusedNotifications, 1);
  exclusivePartial.setChecked(false);
  exclusivePartial.setChecked(true);
  QCOMPARE(exclusivePartial.checkState(), Qt::Checked);
  QCOMPARE(refusedNotifications, 1);

  AdCheckbox nonCheckable;
  nonCheckable.setCheckable(false);
  int nonCheckableNotifications = 0;
  connectNativeCheckState(&nonCheckable, this,
          [&] { ++nonCheckableNotifications; });
  connect(&nonCheckable, &AdCheckbox::toggled, this, [&] { ++nonCheckableNotifications; });
  nonCheckable.setChecked(true);
  QVERIFY(!nonCheckable.isChecked());
  QCOMPARE(nonCheckable.checkState(), Qt::Unchecked);
  QCOMPARE(nonCheckableNotifications, 0);
  nonCheckable.setCheckable(true);
  nonCheckable.setChecked(true);
  nonCheckableNotifications = 0;
  nonCheckable.setCheckable(false);
  nonCheckable.setChecked(false);
  QCOMPARE(nonCheckable.checkState(), Qt::Unchecked);
  QCOMPARE(nonCheckableNotifications, 1);

  AdCheckbox formerlyPartial;
  formerlyPartial.setCheckState(Qt::PartiallyChecked);
  int stateNotifications = 0;
  connectNativeCheckState(&formerlyPartial, this, [&] { ++stateNotifications; });
  formerlyPartial.setTristate(false);
  formerlyPartial.setChecked(true);
  QCOMPARE(formerlyPartial.checkState(), Qt::Checked);
  QCOMPARE(stateNotifications, 1);

  QPointer<AdCheckbox> retired = new AdCheckbox;
  QString deliveredValue;
  connect(retired, &AdCheckbox::valueChanged, this, [&](const QVariant& value) {
    delete retired.data();
    deliveredValue = value.toString();
  });
  retired->setValue(QStringLiteral("copied"));
  QVERIFY(retired.isNull());
  QCOMPARE(deliveredValue, QStringLiteral("copied"));
}

void ButtonSignalLifetimeTests::checkboxGroupSelectionAllowsGroupDeletion() {
  using adqt::widgets::AdCheckbox;
  using adqt::widgets::AdCheckboxGroup;
  {
    QWidget owner;
    QPointer<AdCheckboxGroup> group = new AdCheckboxGroup(&owner);
    auto* first = new AdCheckbox(&owner);
    auto* second = new AdCheckbox(&owner);
    group->addCheckbox(first, QStringLiteral("first"));
    group->addCheckbox(second, QStringLiteral("second"));
    connectNativeCheckState(first, this, [&] { delete group.data(); });
    group->setValues({QStringLiteral("first"), QStringLiteral("second")});
    QVERIFY(group.isNull());
    QVERIFY(first->isChecked());
    QVERIFY(!second->isChecked());
  }

  QPointer<AdCheckboxGroup> group = new AdCheckboxGroup;
  AdCheckbox checkbox;
  checkbox.setChecked(true);
  connect(&checkbox, &AdCheckbox::valueChanged, this, [&] { delete group.data(); });
  group->addCheckbox(&checkbox, QStringLiteral("choice"));
  QVERIFY(group.isNull());
  checkbox.setChecked(false);
  QVERIFY(!checkbox.isChecked());

  AdCheckbox first;
  AdCheckbox second;
  AdCheckboxGroup survivingGroup;
  survivingGroup.addCheckbox(&first, QStringLiteral("first"));
  survivingGroup.addCheckbox(&second, QStringLiteral("second"));
  int selections = 0;
  connect(&survivingGroup, &AdCheckboxGroup::valuesChanged, this, [&] { ++selections; });
  connectNativeCheckState(&first, this, [&](Qt::CheckState state) {
    if (state == Qt::Checked) survivingGroup.setValues({});
  });
  survivingGroup.setValues({QStringLiteral("first"), QStringLiteral("second")});
  QVERIFY(survivingGroup.values().isEmpty());
  QVERIFY(!first.isChecked());
  QVERIFY(!second.isChecked());
  QCOMPARE(selections, 0);
  survivingGroup.setValues({QStringLiteral("second")});
  QCOMPARE(survivingGroup.values(), QVariantList{QStringLiteral("second")});
  QCOMPARE(selections, 1);
}

void ButtonSignalLifetimeTests::radioGroupPropertyPropagationAllowsOwnerDeletion() {
  using adqt::widgets::AdRadio;
  using adqt::widgets::AdRadioButtonGroup;
  for (const int property : {0, 1, 2, 3}) {
    QPointer<QWidget> owner = new QWidget;
    QPointer<AdRadioButtonGroup> group = new AdRadioButtonGroup(owner);
    QPointer<AdRadio> radio = new AdRadio(owner);
    group->addButton(radio, 1);
    int groupNotifications = 0;
    const auto deleteOwner = [&] { delete owner.data(); };
    const auto countGroupNotification = [&] { ++groupNotifications; };
    if (property == 0) {
      connect(radio, &AdRadio::controlSizeChanged, this, deleteOwner);
      connect(group, &AdRadioButtonGroup::controlSizeChanged, this, countGroupNotification);
      group->setControlSize(AdRadio::ControlSize::Large);
    } else if (property == 1) {
      connect(radio, &AdRadio::variantChanged, this, deleteOwner);
      connect(group, &AdRadioButtonGroup::variantChanged, this, countGroupNotification);
      group->setVariant(AdRadio::Variant::Button);
    } else if (property == 2) {
      connect(radio, &AdRadio::buttonStyleChanged, this, deleteOwner);
      connect(group, &AdRadioButtonGroup::buttonStyleChanged, this, countGroupNotification);
      group->setButtonStyle(AdRadio::ButtonStyle::Solid);
    } else {
      connect(radio, &AdRadio::componentTokensChanged, this, deleteOwner);
      connect(group, &AdRadioButtonGroup::componentTokensChanged, this, countGroupNotification);
      AdRadio::ComponentTokens tokens;
      tokens.metrics.radioSize = 31;
      group->setComponentTokens(tokens);
    }
    QVERIFY(owner.isNull());
    QVERIFY(group.isNull());
    QVERIFY(radio.isNull());
    QCOMPARE(groupNotifications, 0);
  }

  AdRadio first;
  QPointer<AdRadio> second = new AdRadio;
  AdRadioButtonGroup group;
  group.addButton(&first, 1);
  group.addButton(second, 2);
  connect(&first, &AdRadio::controlSizeChanged, this, [&] { delete second.data(); });
  group.setControlSize(AdRadio::ControlSize::Large);
  QVERIFY(second.isNull());
  QCOMPARE(first.controlSize(), AdRadio::ControlSize::Large);
  QCOMPARE(group.buttons().size(), 1);

  QPointer<AdRadioButtonGroup> retiredGroup = new AdRadioButtonGroup;
  retiredGroup->addButton(&first, 1);
  connect(&first, &AdRadio::variantChanged, this, [&] { delete retiredGroup.data(); });
  retiredGroup->setVariant(AdRadio::Variant::Button);
  QVERIFY(retiredGroup.isNull());
  QVERIFY(first.group() == nullptr);
  QCOMPARE(first.variant(), AdRadio::Variant::Default);
}

void ButtonSignalLifetimeTests::radioGroupNavigationAllowsOwnerDeletion() {
  using adqt::widgets::AdRadio;
  using adqt::widgets::AdRadioButtonGroup;
  QPointer<QWidget> owner = new QWidget;
  auto* layout = new QHBoxLayout(owner);
  QPointer<AdRadioButtonGroup> group = new AdRadioButtonGroup(owner);
  QPointer<AdRadio> first = new AdRadio(owner);
  QPointer<AdRadio> second = new AdRadio(owner);
  layout->addWidget(first);
  layout->addWidget(second);
  group->setManagedLayout(layout);
  group->addButton(first, 1);
  group->addButton(second, 2);
  group->setCheckedId(1);
  owner->show();
  QCoreApplication::processEvents();
  connect(group, &AdRadioButtonGroup::checkedIdChanged, this, [&] { delete owner.data(); });
  QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  right.setAccepted(false);
  QCoreApplication::sendEvent(first, &right);
  QVERIFY(owner.isNull());
  QVERIFY(first.isNull());
  QVERIFY(second.isNull());
  QVERIFY(right.isAccepted());
}

void ButtonSignalLifetimeTests::nativeArrowNavigationMatchesQtBehavior() {
  for (const bool radio : {false, true}) {
    for (const bool explicitGroup : {false, true}) {
      bool nativeAccepted = false;
      for (const bool custom : {false, true}) {
        QPointer<QWidget> owner = new QWidget;
        auto* layout = new QHBoxLayout(owner);
        const auto createButton = [&](QWidget* parent) -> QAbstractButton* {
          if (custom) {
            return radio ? static_cast<QAbstractButton*>(new adqt::widgets::AdRadio(parent))
                         : new adqt::widgets::AdCheckbox(parent);
          }
          return radio ? static_cast<QAbstractButton*>(new QRadioButton(parent))
                       : new QCheckBox(parent);
        };
        QPointer<QAbstractButton> first = createButton(owner);
        QPointer<QAbstractButton> second = createButton(owner);
        first->setText(QStringLiteral("First"));
        second->setText(QStringLiteral("Second"));
        layout->addWidget(first);
        layout->addWidget(second);
        if (explicitGroup) {
          auto* group = new QButtonGroup(owner);
          group->addButton(first, 1);
          group->addButton(second, 2);
        } else {
          first->setAutoExclusive(true);
          second->setAutoExclusive(true);
        }
        first->setChecked(true);
        owner->show();
        first->setFocus();
        QCoreApplication::processEvents();
        QVERIFY(first->hasFocus());
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        right.setAccepted(false);
        QCoreApplication::sendEvent(first, &right);
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(!first->isChecked());
        QVERIFY(second->isChecked());
        QVERIFY(second->hasFocus());
        if (custom) {
          QCOMPARE(right.isAccepted(), nativeAccepted);
        } else {
          nativeAccepted = right.isAccepted();
        }
        delete owner.data();
      }
    }
  }
}

QTEST_MAIN(ButtonSignalLifetimeTests)

#include "button_signal_lifetime_tests.moc"
