#include "widgets/popup_interaction_host.h"
#include "widgets/popover.h"

#include <QApplication>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using adqt::widgets::AdPopover;
using adqt::widgets::detail::popupInteractionActive;
using adqt::widgets::detail::setPopupInteractionHostOpen;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void pendingRequestsKeepOnlyTheirOwnScopeActive() {
  QWidget host;
  QWidget unrelated;
  QPushButton trigger(&host);
  host.resize(400, 260);
  unrelated.resize(200, 160);
  trigger.setGeometry(80, 60, 80, 30);
  host.show();
  unrelated.show();
  QCoreApplication::processEvents();
  require(!popupInteractionActive(nullptr) && !popupInteractionActive(&host) &&
              !popupInteractionActive(&unrelated),
          "empty scopes and null scopes have no popup interaction");

  auto popup = std::make_unique<AdPopover>();
  popup->setSourceWidget(&trigger);
  popup->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popup->setTriggers({});
  popup->setText(QStringLiteral("Pending popup"));
  bool allow = false;
  popup->setSurfaceShowGuard([&allow](QWidget*) { return allow; });
  popup->show();
  require(popup->isVisible() && popup->surfaceWidget() && !popup->surfaceWidget()->isVisible(),
          "the popup request waits for native surface approval");
  require(popupInteractionActive(&host) && !popupInteractionActive(&unrelated),
          "a pending request keeps its own scope active without affecting other windows");

  allow = true;
  popup->refreshPopupLayout();
  require(popup->surfaceWidget()->isVisible() && popupInteractionActive(&host),
          "an approved popup keeps the same interaction active");
  popup->hide();
  require(!popupInteractionActive(&host), "closing a popup ends its interaction immediately");
  popup->show();
  require(popupInteractionActive(&host), "reopening a popup restores its interaction");
  popup.reset();
  require(!popupInteractionActive(&host), "destroying an open popup removes its interaction");
}

void nestedPopupInteractionsStayActiveUntilTheParentCloses() {
  for (const auto layer :
       {AdPopover::PopupLayerMode::InWindow, AdPopover::PopupLayerMode::QtTool}) {
    QWidget host;
    QPushButton trigger(&host);
    host.resize(640, 400);
    trigger.setGeometry(160, 160, 80, 30);
    host.show();
    QCoreApplication::processEvents();

    AdPopover parent;
    parent.setSourceWidget(&trigger);
    parent.setPopupLayerMode(layer);
    parent.setTriggers({});
    auto* nestedTrigger = new QPushButton(QStringLiteral("Nested"));
    parent.setContentWidget(nestedTrigger);
    parent.show();
    require(parent.surfaceWidget() && parent.surfaceWidget()->isVisible() &&
                popupInteractionActive(&host),
            "the parent popup activates its scope");

    AdPopover nested;
    nested.setSourceWidget(nestedTrigger);
    nested.setPopupLayerMode(layer);
    nested.setTriggers({});
    nested.setText(QStringLiteral("Nested popup"));
    nested.show();
    require(parent.isVisible() && nested.isVisible() && popupInteractionActive(&host),
            "a nested popup preserves the parent scope interaction");
    nested.hide();
    require(parent.isVisible() && popupInteractionActive(&host),
            "closing the nested popup preserves the parent interaction");
    parent.hide();
    require(!popupInteractionActive(&host), "closing the parent ends the scope interaction");
  }
}

class InteractionOwner final : public QObject, public adqt::widgets::detail::PopupInteractionOwner {
 public:
  InteractionOwner(QWidget& scope, QWidget& anchor, QWidget& surface)
      : scope_(&scope), anchor_(&anchor), surface_(&surface) {}

  QObject* popupOwnerObject() const override { return const_cast<InteractionOwner*>(this); }
  QWidget* popupAnchorWidget() const override { return anchor_; }
  QWidget* popupScopeWindow() const override { return scope_; }
  QWidget* popupSurfaceWidget() const override { return surface_; }
  bool popupIsVisible() const override { return requestedVisible; }
  bool popupWantsHostFrameRelayout() const override { return false; }
  bool popupContainsGlobalPos(const QPoint& position) const override {
    return surface_ && surface_->rect().contains(surface_->mapFromGlobal(position));
  }
  void popupCloseFromHost(adqt::widgets::detail::PopupCloseReason) override {
    requestedVisible = false;
    setPopupInteractionHostOpen(this, false);
  }
  void popupRelayoutFromHost() override {}

  bool requestedVisible = true;

 private:
  QPointer<QWidget> scope_;
  QPointer<QWidget> anchor_;
  QPointer<QWidget> surface_;
};

void suspendedOwnersRemainSafeAcrossVisibilityAndDestruction() {
  QWidget host;
  QPushButton trigger(&host);
  QWidget parentSurface(&host);
  QPushButton nestedTrigger(&parentSurface);
  QWidget nestedSurface(&host);
  host.resize(500, 300);
  trigger.setGeometry(20, 20, 80, 30);
  parentSurface.setGeometry(100, 80, 160, 100);
  nestedTrigger.setGeometry(20, 20, 80, 30);
  nestedSurface.setGeometry(280, 80, 120, 100);
  host.show();
  QCoreApplication::processEvents();

  auto parent = std::make_unique<InteractionOwner>(host, trigger, parentSurface);
  auto nested = std::make_unique<InteractionOwner>(host, nestedTrigger, nestedSurface);
  setPopupInteractionHostOpen(parent.get(), true);
  setPopupInteractionHostOpen(nested.get(), true);
  require(parent->requestedVisible && nested->requestedVisible && popupInteractionActive(&host),
          "opening inside a parent popup suspends that parent's interaction");

  nested->requestedVisible = false;
  require(popupInteractionActive(&host),
          "a visible suspended parent keeps the scope active when its active child closes");
  parent->requestedVisible = false;
  require(!popupInteractionActive(&host),
          "registered owners with closed requests do not keep the scope active");
  parent->requestedVisible = true;
  parent.reset();
  require(!popupInteractionActive(&host),
          "destroyed suspended owners cannot keep the scope active or be dereferenced");
  nested->requestedVisible = true;
  require(popupInteractionActive(&host), "the remaining active owner still owns its interaction");
  nested.reset();
  require(!popupInteractionActive(&host),
          "destroying the active owner safely skips its destroyed suspended parent");
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  pendingRequestsKeepOnlyTheirOwnScopeActive();
  nestedPopupInteractionsStayActiveUntilTheParentCloses();
  suspendedOwnersRemainSafeAcrossVisibilityAndDestruction();
  return 0;
}
