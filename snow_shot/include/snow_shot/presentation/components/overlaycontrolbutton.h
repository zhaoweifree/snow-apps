// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_OVERLAYCONTROLBUTTON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_OVERLAYCONTROLBUTTON_H

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/detail/pointer_region.h"

#include <QPainter>
#include <cstdint>

namespace snow_shot::presentation::components {

class OverlayControlButton : public adqt::widgets::AdButton {
  public:
    enum class Intent : std::uint8_t { Primary, Destructive };
    static constexpr int controlSize = 32;

    explicit OverlayControlButton(Intent intent, const adqt::icons::IconRef& icon,
                                  QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent), m_intent(intent) {
        setFocusPolicy(Qt::NoFocus);
        setShape(Shape::Circle);
        setSizeClass(SizeClass::Medium);
        setFixedSize(controlSize, controlSize);
        setIconSize(QSize(18, 18));
        setIconRef(icon.withColors(adqt::icons::IconColors::primary(QColor(Qt::white))));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setButtonStyle(ButtonStyle::Text);
        setAccentRole(AccentRole::Neutral);
        setInteractionBackgroundVisible(false);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QColor background = theme.colorBgMask.isValid() ? theme.colorBgMask : QColor(0, 0, 0, 115);
        if (isEnabled() && isDown()) {
            background =
                m_intent == Intent::Destructive ? theme.colorErrorActive : theme.colorPrimaryActive;
        } else if (isEnabled() && adqt::widgets::detail::widgetHovered(this)) {
            background = m_intent == Intent::Destructive ? theme.colorError : theme.colorPrimary;
        }
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawEllipse(rect());
        }
        adqt::widgets::AdButton::paintEvent(event);
    }

  private:
    Intent m_intent;
};

} // namespace snow_shot::presentation::components

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_OVERLAYCONTROLBUTTON_H
