#pragma once

#include <QApplication>
#include "physical_key_test_support.h"
#include <QKeyEvent>
#include <QPointer>
#include <QWidget>

inline bool triggerWindowCloseShortcut(QWidget* window, QWidget* focus = nullptr) {
    window->activateWindow();
    QApplication::processEvents();
    if (!focus)
        focus = window;
    focus->setFocus();
    // Qt5's QKeySequence::operator[] still returns an int; Qt6 returns a
    // QKeyCombination. snowKeySequenceAt normalises both (rule R35).
    const auto key = snowKeySequenceAt(QKeySequence::keyBindings(QKeySequence::Close).front(), 0);
    QPointer<QWidget> target = focus;
    PhysicalKeyEvent press(QEvent::KeyPress, key.key(), key.keyboardModifiers());
    QApplication::sendEvent(target, &press);
    if (target) {
        PhysicalKeyEvent release(QEvent::KeyRelease, key.key(), key.keyboardModifiers());
        QApplication::sendEvent(target, &release);
    }
    return true;
}
