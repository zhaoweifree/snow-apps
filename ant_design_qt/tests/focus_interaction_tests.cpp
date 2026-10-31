#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/modal.h"
#include "widgets/popconfirm.h"
#include "widgets/radio.h"
#include "widgets/switch.h"
#include "widgets/input_internal.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_text_edit.h"

#include <QApplication>
#include <QFocusEvent>
#include <QImage>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>

#include <iostream>
#include <functional>
#include <type_traits>
#include <stdexcept>

namespace {

using namespace adqt::widgets;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents();
}

void scrollRestorationAllowsAreaRetirement() {
  using namespace adqt::widgets::detail::input_internal;
  QPointer<QScrollArea> first = new QScrollArea;
  QPointer<QScrollArea> sibling = new QScrollArea;
  for (const auto& area : {first, sibling}) {
    area->horizontalScrollBar()->setRange(0, 100);
    area->verticalScrollBar()->setRange(0, 100);
    area->horizontalScrollBar()->setValue(50);
    area->verticalScrollBar()->setValue(50);
  }
  // Qt 6 aliases QVector onto QList, whose initializer_list constructor accepts
  // a nested braced list for an aggregate element. Qt 5.15's QVector has an
  // initializer_list constructor too, but MSVC refuses the nested form there
  // (C2440: cannot convert from 'initializer list' to 'QVector<T>'), so each
  // element is built explicitly and the vector is initialised from them.
  const auto scrollPosition = [](const QPointer<QScrollArea>& area, int horizontal, int vertical) {
    PreservedScrollPosition position;
    position.area = area;
    position.horizontalValue = horizontal;
    position.verticalValue = vertical;
    return position;
  };
  const QVector<PreservedScrollPosition> positions{scrollPosition(first, 10, 20),
                                                   scrollPosition(sibling, 30, 40)};
  int notifications = 0;
  QObject::connect(first->horizontalScrollBar(), &QScrollBar::valueChanged, qApp, [&](int) {
    ++notifications;
    delete first.data();
    delete sibling.data();
  });
  restoreAncestorScrollPositions(positions);
  require(first.isNull() && sibling.isNull() && notifications == 1,
          "scroll restoration must stop using areas retired by scrollbar observers");
}

void textSetterNotificationsAllowEditorRetirement() {
  for (const bool clamp : {false, true}) {
    QPointer<AdTextEdit> editor = new AdTextEdit;
    if (clamp) {
      editor->setPlainText(QStringLiteral("abcdef"));
    }
    int plainNotifications = 0;
    int maxLengthNotifications = 0;
    int editNotifications = 0;
    QObject observer;
    QObject::connect(editor, &AdTextEdit::maxLengthChanged, &observer,
                     [&](int) { ++maxLengthNotifications; });
    QObject::connect(editor, &AdTextEdit::textEdited, &observer,
                     [&](const QString&) { ++editNotifications; });
    QObject::connect(editor, &AdTextEdit::plainTextChanged, &observer, [&](const QString& text) {
      ++plainNotifications;
      require(editor->plainText() == QStringLiteral("abc"),
              "plain-text publication must expose the committed text");
      delete editor.data();
      require(text == QStringLiteral("abc"),
              "plain-text notification payload must survive editor retirement");
    });
    if (clamp) {
      editor->setMaxLength(3);
    } else {
      editor->setPlainText(QStringLiteral("abc"));
    }
    require(editor.isNull() && plainNotifications == 1 && maxLengthNotifications == 0 &&
                editNotifications == 0,
            "public text setters must stop after notification retires the editor");
  }

  AdTextEdit editor;
  editor.setPlainText(QStringLiteral("abcdef"));
  QVector<int> limits;
  QObject::connect(&editor, &AdTextEdit::maxLengthChanged, &editor,
                   [&](int limit) { limits.push_back(limit); });
  QObject::connect(&editor, &AdTextEdit::plainTextChanged, &editor, [&](const QString& text) {
    if (text == QStringLiteral("abc")) {
      editor.setMaxLength(2);
    }
  });
  editor.setMaxLength(3);
  require(editor.maxLength() == 2 && editor.plainText() == QStringLiteral("ab") &&
              limits == QVector<int>{2},
          "nested text publication must supersede the outer character-limit notification");
}

class CallbackTextPolicy final : public AdInputTextPolicy {
 public:
  std::function<void()> callback;

  QString normalizeText(const QString& text, int) const override {
    const auto notify = callback;
    if (notify) {
      notify();
    }
    return text.toUpper();
  }
};

template <typename Editor>
void setEditorText(Editor* editor, const QString& text) {
  if constexpr (std::is_same_v<Editor, AdLineEdit>) {
    editor->setText(text);
  } else {
    editor->setPlainText(text);
  }
}

template <typename Editor>
QString editorText(Editor* editor) {
  if constexpr (std::is_same_v<Editor, AdLineEdit>) {
    return editor->text();
  } else {
    return editor->plainText();
  }
}

template <typename Editor>
void publicTextNormalizationAllowsRetirementAndSupersession() {
  CallbackTextPolicy retiringPolicy;
  QPointer<Editor> retired = new Editor;
  retired->setTextPolicy(&retiringPolicy);
  retiringPolicy.callback = [&] { delete retired.data(); };
  setEditorText(retired.data(), QStringLiteral("outer"));
  require(retired.isNull(), "public text setters must stop after normalization retires the editor");

  CallbackTextPolicy reentrantPolicy;
  Editor editor;
  editor.setTextPolicy(&reentrantPolicy);
  reentrantPolicy.callback = [&] {
    reentrantPolicy.callback = {};
    setEditorText(&editor, QStringLiteral("nested"));
  };
  setEditorText(&editor, QStringLiteral("outer"));
  require(editorText(&editor) == QStringLiteral("NESTED"),
          "normalizer reentry must let the nested text setter supersede the outer transaction");

  CallbackTextPolicy replacingPolicy;
  AdInputTextPolicy replacement;
  Editor replaced;
  replaced.setTextPolicy(&replacingPolicy);
  replacingPolicy.callback = [&] { replaced.setTextPolicy(&replacement); };
  setEditorText(&replaced, QStringLiteral("outer"));
  require(replaced.textPolicy() == &replacement && editorText(&replaced).isEmpty(),
          "policy replacement during normalization must cancel the stale normalized text");
}

template <typename Editor>
void textPolicyNotificationsAllowEditorRetirement() {
  AdInputTextPolicy policy;
  QPointer<Editor> retired = new Editor;
  int notifications = 0;
  QObject observer;
  QObject::connect(retired, &Editor::textPolicyChanged, &observer,
                   [&](AdInputTextPolicy* committed) {
                     ++notifications;
                     require(committed == &policy && retired->textPolicy() == &policy,
                             "policy notification must expose the committed policy");
                     delete retired.data();
                   });
  retired->setTextPolicy(&policy);
  require(retired.isNull() && notifications == 1,
          "policy setters must allow retirement from their own final notification");

  Editor editor;
  CallbackTextPolicy first;
  AdInputTextPolicy second;
  QVector<AdInputTextPolicy*> published;
  QObject::connect(&editor, &Editor::textPolicyChanged, &observer,
                   [&](AdInputTextPolicy* committed) { published.push_back(committed); });
  first.callback = [&] { editor.setTextPolicy(&second); };
  editor.setTextPolicy(&first);
  require(editor.textPolicy() == &second && published == QVector<AdInputTextPolicy*>{&second},
          "normalizer reentry must cancel the superseded outer policy publication");

  Editor nested;
  setEditorText(&nested, QStringLiteral("old"));
  CallbackTextPolicy nestedPolicy;
  int nestedPolicyNotifications = 0;
  QObject::connect(
      &nested, &Editor::textPolicyChanged, &observer, [&](AdInputTextPolicy* committed) {
        ++nestedPolicyNotifications;
        require(committed == &nestedPolicy && editorText(&nested) == QStringLiteral("NESTED"),
                "final policy publication must preserve the latest nested text");
      });
  nestedPolicy.callback = [&] {
    nestedPolicy.callback = {};
    setEditorText(&nested, QStringLiteral("nested"));
  };
  nested.setTextPolicy(&nestedPolicy);
  require(editorText(&nested) == QStringLiteral("NESTED") && nestedPolicyNotifications == 1,
          "policy normalization must not overwrite a newer nested text transaction");
}

void textPolicyNestedPublicationAllowsEditorRetirement() {
  CallbackTextPolicy uppercase;
  QPointer<AdTextEdit> editor = new AdTextEdit;
  editor->setPlainText(QStringLiteral("abc"));
  int policies = 0;
  QObject observer;
  QObject::connect(editor, &AdTextEdit::textPolicyChanged, &observer,
                   [&](AdInputTextPolicy*) { ++policies; });
  QObject::connect(editor, &AdTextEdit::plainTextChanged, &observer, [&](const QString& text) {
    require(text == QStringLiteral("ABC"), "policy setter must publish normalized text");
    delete editor.data();
  });
  editor->setTextPolicy(&uppercase);
  require(editor.isNull() && policies == 0,
          "policy setters must stop when nested text publication retires the editor");
}

void visibleRowNotificationsAllowEditorRetirement() {
  QPointer<AdTextEdit> retired = new AdTextEdit;
  QObject observer;
  int minimumNotifications = 0;
  QObject::connect(retired, &AdTextEdit::minimumVisibleRowsChanged, &observer,
                   [&](int) { ++minimumNotifications; });
  QObject::connect(retired, &AdTextEdit::maximumVisibleRowsChanged, &observer, [&](int maximum) {
    require(
        maximum == 8 && retired->minimumVisibleRows() == 8 && retired->maximumVisibleRows() == 8,
        "row publication must expose both committed bounds");
    delete retired.data();
  });
  retired->setMinimumVisibleRows(8);
  require(retired.isNull() && minimumNotifications == 0,
          "row setters must stop between notifications when their editor is retired");

  AdTextEdit editor;
  QVector<int> minimums;
  QObject::connect(&editor, &AdTextEdit::minimumVisibleRowsChanged, &editor,
                   [&](int minimum) { minimums.push_back(minimum); });
  QObject::connect(&editor, &AdTextEdit::maximumVisibleRowsChanged, &editor, [&](int maximum) {
    if (maximum == 8) {
      editor.setMinimumVisibleRows(10);
    }
  });
  editor.setMinimumVisibleRows(8);
  require(editor.minimumVisibleRows() == 10 && editor.maximumVisibleRows() == 10 &&
              minimums == QVector<int>{10},
          "nested row publication must supersede the outer minimum notification");
}

template <typename Editor>
void focusScrollRestorationAllowsEditorRetirement() {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  auto* focusTarget = new QPushButton(QStringLiteral("Other"), &owner);
  QPointer<QScrollArea> area = new QScrollArea(&owner);
  auto* content = new QWidget;
  content->resize(600, 300);
  QPointer<Editor> editor = new Editor(content);
  editor->setGeometry(20, 20, 150, 50);
  area->setWidget(content);
  layout->addWidget(focusTarget);
  layout->addWidget(area);
  owner.resize(260, 220);
  owner.show();
  owner.activateWindow();
  settle();
  focusTarget->setFocus(Qt::OtherFocusReason);
  area->horizontalScrollBar()->setValue(0);

  QObject observer;
  bool scrolledDuringFocus = false;
  QObject::connect(qApp, &QApplication::focusChanged, &observer, [&](QWidget*, QWidget* focused) {
    if (editor && focused == editor.data()) {
      scrolledDuringFocus = true;
      area->horizontalScrollBar()->setValue(80);
    }
  });
  QObject::connect(area->horizontalScrollBar(), &QScrollBar::valueChanged, &observer,
                   [&](int value) {
                     if (scrolledDuringFocus && value == 0) {
                       delete area.data();
                     }
                   });
  editor->focusEditor(Editor::FocusSelection::Preserve, true);
  require(scrolledDuringFocus && area.isNull() && editor.isNull(),
          "focus helpers must stop after scroll restoration retires their owning area");
  settle();
}

template <typename Control>
void focusOutlineRequiresKeyboardInput() {
  QWidget owner;
  owner.resize(240, 140);
  Control control(&owner);
  control.setGeometry(40, 40, 120, 40);
  owner.show();
  owner.activateWindow();
  settle();
  const auto imageForFocus = [&](Qt::FocusReason reason) {
    control.clearFocus();
    control.setFocus(reason);
    settle();
    require(control.hasFocus(), "focus fixture must focus the control");
    return owner.grab().toImage();
  };
  const QImage mouse = imageForFocus(Qt::MouseFocusReason);
  for (const auto reason : {Qt::OtherFocusReason, Qt::PopupFocusReason, Qt::ActiveWindowFocusReason,
                            Qt::MenuBarFocusReason, Qt::NoFocusReason}) {
    require(imageForFocus(reason) == mouse,
            "programmatic, popup and window focus must not paint a keyboard outline");
  }
  for (const auto reason : {Qt::TabFocusReason, Qt::BacktabFocusReason, Qt::ShortcutFocusReason}) {
    require(imageForFocus(reason) != mouse,
            "Tab, Shift+Tab and shortcut focus must paint a keyboard outline");
  }
}

void modalDismissalPreservesCurrentFocus(AdModal::Mode mode) {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  AdButton trigger(QStringLiteral("Open"));
  AdButton current(QStringLiteral("Current"));
  layout->addWidget(&trigger);
  layout->addWidget(&current);
  owner.show();
  owner.activateWindow();
  settle();
  trigger.setFocus(Qt::TabFocusReason);
  AdModal modal(&owner);
  modal.setMode(mode);
  modal.setWindowModality(Qt::NonModal);
  modal.open();
  settle();
  owner.activateWindow();
  current.setFocus(Qt::MouseFocusReason);
  settle();
  require(current.hasFocus(), "modal fixture must establish a new focus target");
  modal.reject();
  settle();
  require(current.hasFocus(), "closing a modal must not refocus its previous trigger");
}

void popconfirmDismissalPreservesCurrentFocus(bool action) {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  AdButton trigger(QStringLiteral("Open"));
  AdButton current(QStringLiteral("Current"));
  layout->addWidget(&trigger);
  layout->addWidget(&current);
  owner.show();
  owner.activateWindow();
  settle();
  AdPopconfirm popup(&owner);
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopconfirm::PopupLayerMode::InWindow);
  popup.setText(QStringLiteral("Confirm"));
  popup.setDefaultButton(AdPopconfirm::StandardButton::Cancel);
  popup.show();
  settle();
  auto* cancel = popup.button(AdPopconfirm::StandardButton::Cancel);
  require(cancel && cancel->hasFocus(), "popconfirm fixture must establish popup focus");
  current.setFocus(Qt::MouseFocusReason);
  require(current.hasFocus(), "popconfirm fixture must establish a new focus target");
  if (action) {
    cancel->click();
  } else {
    popup.hide();
  }
  settle();
  require(!popup.isVisible(), "dismissal must close the popconfirm");
  require(current.hasFocus(), "closing a popconfirm must not refocus its source button");
}

void modalTabNavigationRemainsAvailable() {
  QWidget owner;
  owner.show();
  AdModal modal(&owner);
  modal.open();
  settle();
  auto* initial = QApplication::focusWidget();
  require(initial, "an open modal must establish initial focus");
  QTest::keyClick(initial, Qt::Key_Tab);
  require(QApplication::focusWidget() != initial,
          "Tab must still move keyboard focus inside a modal");
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ShiftModifier);
  require(QApplication::focusWidget() == initial,
          "Shift+Tab must still return to the initial modal control");
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  try {
    scrollRestorationAllowsAreaRetirement();
    textSetterNotificationsAllowEditorRetirement();
    textPolicyNotificationsAllowEditorRetirement<AdLineEdit>();
    textPolicyNotificationsAllowEditorRetirement<AdTextEdit>();
    publicTextNormalizationAllowsRetirementAndSupersession<AdLineEdit>();
    publicTextNormalizationAllowsRetirementAndSupersession<AdTextEdit>();
    textPolicyNestedPublicationAllowsEditorRetirement();
    visibleRowNotificationsAllowEditorRetirement();
    focusScrollRestorationAllowsEditorRetirement<AdLineEdit>();
    focusScrollRestorationAllowsEditorRetirement<AdTextEdit>();
    // Check dismissal first so the original focus handoff is reproduced independently
    // of the focus outline classification.
    modalDismissalPreservesCurrentFocus(AdModal::Mode::Overlay);
    modalDismissalPreservesCurrentFocus(AdModal::Mode::Window);
    popconfirmDismissalPreservesCurrentFocus(false);
    popconfirmDismissalPreservesCurrentFocus(true);
    focusOutlineRequiresKeyboardInput<AdButton>();
    focusOutlineRequiresKeyboardInput<AdCheckbox>();
    focusOutlineRequiresKeyboardInput<AdRadio>();
    focusOutlineRequiresKeyboardInput<AdSwitch>();
    modalTabNavigationRemainsAvailable();
    std::cout << "Focus interaction tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
