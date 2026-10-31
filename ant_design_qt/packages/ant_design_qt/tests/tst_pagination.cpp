#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QLabel>
#include <QMetaProperty>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTranslator>

#include <algorithm>
#include <limits>

#include "widgets/combo_box.h"
#include "widgets/input_line_edit.h"
#include "widgets/pagination.h"
#include "widgets/pagination_style.h"
#include "theme/theme_manager.h"

using adqt::widgets::AdComboBox;
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdPagination;

class PaginationTranslator final : public QTranslator {
 public:
  QString translate(const char* context, const char* sourceText, const char*, int) const override {
    if (qstrcmp(context, "adqt::widgets::AdPagination") == 0) {
      return QStringLiteral("translated:%1").arg(QString::fromUtf8(sourceText));
    }
    return {};
  }
};

class PaginationTest final : public QObject {
  Q_OBJECT

 private slots:
  void defaultsAndRange();
  void clampsWhenInputsChange();
  void stateIsCommittedBeforePropertyNotifications();
  void propertyNotificationsSurviveOwnerDeletion();
  void userNavigationSurvivesOwnerDeletion();
  void propertyNotificationsTrackNestedChanges();
  void userNotificationsTrackNestedChanges();
  void nestedNotificationsSurviveOwnerDeletion();
  void automaticSizeChangerBoundary();
  void pageSizeChangerUsesDesktopComboBehavior();
  void publicPropertyMetadataAndTokenResolution();
  void borderGeometryIsAlignedAtFractionalScale();
  void rightToLeftLayoutMirrorsNavigation();
  void pageWindowMatchesRcPagination();
  void lessItemsUsesSmallerWindow();
  void pageClickEmitsUserChange();
  void jumpAndQuickJumper();
  void sizeChangerClampsCurrentPage();
  void simpleAndSinglePageModes();
  void disabledBlocksNavigation();
  void standardEnabledStateNotifiesDisabledProperty();
  void navigationPreservesButtonIdentityAndFocus();
  void pooledButtonsClearInactiveState();
  void quickJumperPreservesFocus();
  void languageChangeRefreshesPersistentText();
  void maximumIntegerRangeDoesNotOverflow();
  void disabledPagerRefreshesOnThemeChange();
};

void PaginationTest::defaultsAndRange() {
  AdPagination pagination;
  QCOMPARE(pagination.currentPage(), 1);
  QCOMPARE(pagination.total(), 0);
  QCOMPARE(pagination.pageSize(), 10);
  QCOMPARE(pagination.pageCount(), 0);
  QCOMPARE(pagination.visibleItemRange().first, 0);
  QCOMPARE(pagination.visibleItemRange().last, 0);

  pagination.setTotal(85);
  pagination.setPageSize(20);
  pagination.setCurrentPage(3);
  QCOMPARE(pagination.pageCount(), 5);
  QCOMPARE(pagination.visibleItemRange().first, 41);
  QCOMPARE(pagination.visibleItemRange().last, 60);
}

void PaginationTest::clampsWhenInputsChange() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setCurrentPage(10);
  QSignalSpy currentSpy(&pagination, &AdPagination::currentPageChanged);
  QSignalSpy pageCountSpy(&pagination, &AdPagination::pageCountChanged);

  pagination.setPageSize(30);
  QCOMPARE(pagination.pageCount(), 4);
  QCOMPARE(pagination.currentPage(), 4);
  QCOMPARE(currentSpy.count(), 1);
  QCOMPARE(pageCountSpy.count(), 1);

  pagination.setTotal(-100);
  QCOMPARE(pagination.total(), 0);
  QCOMPARE(pagination.pageCount(), 0);
  QCOMPARE(pagination.currentPage(), 1);
}

void PaginationTest::stateIsCommittedBeforePropertyNotifications() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setCurrentPage(10);
  connect(&pagination, &AdPagination::pageSizeChanged, this, [&](int) {
    QCOMPARE(pagination.currentPage(), 4);
    auto* current = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage4"));
    QVERIFY(current);
    QVERIFY(current->isChecked());
  });
  pagination.setPageSize(30);
  connect(&pagination, &AdPagination::totalChanged, this, [&](int) {
    QCOMPARE(pagination.currentPage(), 1);
    QCOMPARE(pagination.pageCount(), 1);
  });
  pagination.setTotal(20);
}

void PaginationTest::propertyNotificationsSurviveOwnerDeletion() {
  for (const int signal : {0, 1, 2, 3}) {
    QPointer<AdPagination> pagination = new AdPagination;
    pagination->setTotal(100);
    pagination->setCurrentPage(10);
    const auto deleteOwner = [&] { delete pagination.data(); };
    if (signal == 0) {
      connect(pagination, &AdPagination::totalChanged, this, deleteOwner);
      pagination->setTotal(20);
    } else if (signal == 1) {
      connect(pagination, &AdPagination::pageSizeChanged, this, deleteOwner);
      pagination->setPageSize(30);
    } else if (signal == 2) {
      connect(pagination, &AdPagination::currentPageChanged, this, deleteOwner);
      pagination->setTotal(20);
    } else {
      connect(pagination, &AdPagination::pageCountChanged, this, deleteOwner);
      pagination->setPageSize(30);
    }
    QVERIFY(pagination.isNull());
  }
}

void PaginationTest::userNavigationSurvivesOwnerDeletion() {
  for (const int entry : {0, 1, 2}) {
    QPointer<AdPagination> pagination = new AdPagination;
    pagination->setTotal(100);
    connect(pagination, &AdPagination::currentPageChanged, this, [&] { delete pagination.data(); });
    if (entry == 0) {
      pagination->nextPage();
    } else if (entry == 1) {
      auto* button = pagination->findChild<QAbstractButton*>(QStringLiteral("paginationPage2"));
      QVERIFY(button);
      button->click();
    } else {
      pagination->setShowQuickJumper(true);
      auto* input = pagination->findChild<AdLineEdit*>(QStringLiteral("paginationQuickInput"));
      QVERIFY(input);
      input->setText(QStringLiteral("2"));
      emit input->editingFinished();
    }
    QVERIFY(pagination.isNull());
  }
}

void PaginationTest::propertyNotificationsTrackNestedChanges() {
  for (const bool changeTotal : {false, true}) {
    AdPagination pagination;
    pagination.setTotal(100);
    pagination.setCurrentPage(10);
    QList<int> pages;
    QList<int> counts;
    const auto resetPage = [&] { pagination.setCurrentPage(1); };
    if (changeTotal) {
      connect(&pagination, &AdPagination::totalChanged, this, resetPage);
    } else {
      connect(&pagination, &AdPagination::pageSizeChanged, this, resetPage);
    }
    connect(&pagination, &AdPagination::currentPageChanged, this, [&](int page) {
      QCOMPARE(page, pagination.currentPage());
      pages.append(page);
    });
    connect(&pagination, &AdPagination::pageCountChanged, this, [&](int count) {
      QCOMPARE(count, pagination.pageCount());
      counts.append(count);
    });
    if (changeTotal) {
      pagination.setTotal(20);
    } else {
      pagination.setPageSize(30);
    }
    QCOMPARE(pages, QList<int>({1}));
    QCOMPARE(counts, QList<int>({changeTotal ? 2 : 4}));
    QCOMPARE(pagination.currentPage(), 1);
  }
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setCurrentPage(10);
  QList<int> totals;
  QList<int> counts;
  connect(&pagination, &AdPagination::totalChanged, this, [&](int total) {
    QCOMPARE(total, pagination.total());
    totals.append(total);
    if (total == 20) {
      pagination.setTotal(50);
      pagination.setPageSize(20);
      pagination.setCurrentPage(1);
    }
  });
  connect(&pagination, &AdPagination::pageCountChanged, this, [&](int count) {
    QCOMPARE(count, pagination.pageCount());
    counts.append(count);
  });
  pagination.setTotal(20);
  QCOMPARE(totals, QList<int>({20, 50}));
  QCOMPARE(counts, QList<int>({3}));
  QCOMPARE(pagination.currentPage(), 1);
  QCOMPARE(pagination.pageSize(), 20);
}

void PaginationTest::userNotificationsTrackNestedChanges() {
  {
    AdPagination pagination;
    pagination.setTotal(100);
    QSignalSpy changed(&pagination, &AdPagination::changed);
    connect(&pagination, &AdPagination::currentPageChanged, this, [&](int page) {
      QCOMPARE(page, pagination.currentPage());
      if (page == 2) {
        pagination.setPageSize(20);
        pagination.jumpToPage(3);
      }
    });
    connect(&pagination, &AdPagination::changed, this, [&](int page, int size) {
      QCOMPARE(page, pagination.currentPage());
      QCOMPARE(size, pagination.pageSize());
    });
    pagination.nextPage();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toInt(), 3);
    QCOMPARE(changed.at(0).at(1).toInt(), 20);
  }
  {
    AdPagination pagination;
    pagination.setTotal(100);
    pagination.setCurrentPage(10);
    pagination.setShowSizeChanger(true);
    QSignalSpy selected(&pagination, &AdPagination::pageSizeSelected);
    QSignalSpy changed(&pagination, &AdPagination::changed);
    QList<int> sizes;
    connect(&pagination, &AdPagination::pageSizeSelected, this, [&](int page, int size) {
      QCOMPARE(page, 10);
      QCOMPARE(size, 20);
      pagination.setPageSize(50);
      pagination.setCurrentPage(1);
    });
    connect(&pagination, &AdPagination::pageSizeChanged, this, [&](int size) {
      QCOMPARE(size, pagination.pageSize());
      sizes.append(size);
    });
    connect(&pagination, &AdPagination::changed, this, [&](int page, int size) {
      QCOMPARE(page, pagination.currentPage());
      QCOMPARE(size, pagination.pageSize());
    });
    auto* changer = pagination.findChild<AdComboBox*>(QStringLiteral("paginationSizeChanger"));
    QVERIFY(changer);
    changer->setCurrentValue(20);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(sizes, QList<int>({50}));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toInt(), 1);
    QCOMPARE(changed.at(0).at(1).toInt(), 50);
  }
}

void PaginationTest::nestedNotificationsSurviveOwnerDeletion() {
  for (const bool userAction : {false, true}) {
    QPointer<AdPagination> pagination = new AdPagination;
    pagination->setTotal(100);
    pagination->setCurrentPage(userAction ? 1 : 10);
    int trailingNotifications = 0;
    if (userAction) {
      connect(pagination, &AdPagination::currentPageChanged, this,
              [&] { pagination->setPageSize(20); });
      connect(pagination, &AdPagination::pageSizeChanged, this, [&] { delete pagination.data(); });
      connect(pagination, &AdPagination::changed, this, [&] { ++trailingNotifications; });
      pagination->nextPage();
    } else {
      connect(pagination, &AdPagination::totalChanged, this,
              [&] { pagination->setCurrentPage(1); });
      connect(pagination, &AdPagination::currentPageChanged, this,
              [&] { delete pagination.data(); });
      connect(pagination, &AdPagination::pageCountChanged, this, [&] { ++trailingNotifications; });
      pagination->setTotal(20);
    }
    QVERIFY(pagination.isNull());
    QCOMPARE(trailingNotifications, 0);
  }
}

void PaginationTest::automaticSizeChangerBoundary() {
  AdPagination pagination;
  pagination.setPageSizeOptions({50, 10, 20});
  QCOMPARE(pagination.pageSizeOptions(), QVector<int>({50, 10, 20}));
  pagination.setTotal(50);
  QVERIFY(!pagination.isSizeChangerVisible());
  pagination.setTotal(51);
  QVERIFY(pagination.isSizeChangerVisible());
  pagination.setSizeChangerMode(AdPagination::SizeChangerMode::Never);
  QVERIFY(!pagination.isSizeChangerVisible());
  pagination.setShowSizeChanger(true);
  QVERIFY(pagination.isSizeChangerVisible());
}

void PaginationTest::pageSizeChangerUsesDesktopComboBehavior() {
  AdPagination pagination;
  pagination.setTotal(100);
  auto* changer = pagination.findChild<AdComboBox*>(QStringLiteral("paginationSizeChanger"));
  QVERIFY(changer);
  QVERIFY(!changer->searchEnabled());
  QVERIFY(!changer->editable());
}

void PaginationTest::publicPropertyMetadataAndTokenResolution() {
  AdPagination pagination;
  const QMetaObject* metaObject = pagination.metaObject();
  const int propertyIndex = metaObject->indexOfProperty("pageSizeOptions");
  QVERIFY(propertyIndex >= 0);
  const QMetaProperty property = metaObject->property(propertyIndex);
  QVERIFY(property.isReadable());
  QVERIFY(property.isWritable());
  QVERIFY(property.hasNotifySignal());

  adqt::widgets::detail::PaginationStyleInput input;
  input.baseFont = pagination.font();
  const QColor itemText(QStringLiteral("#123456"));
  const QColor rootBorder(QStringLiteral("#654321"));
  input.componentTokens.colors.itemText = itemText;
  input.semanticStyles.root.borderColor = rootBorder;
  const auto style = adqt::widgets::detail::resolvePaginationVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(&pagination));
  QCOMPARE(style.text, itemText);
  QVERIFY(style.hasRootBorder);
  QCOMPARE(style.rootBorder, rootBorder);
}

void PaginationTest::borderGeometryIsAlignedAtFractionalScale() {
  constexpr qreal dpr = 1.5;
  constexpr qreal borderWidth = 1.0;
  const QRectF bounds(0.0, 0.0, 32.0, 32.0);
  const QPointF origin(1.0, 1.0);
  const QRectF borderRect =
      adqt::widgets::detail::deviceAlignedPaginationBorderRect(bounds, borderWidth, dpr, origin);
  const qreal halfBorder = borderWidth / 2.0;

  const qreal outerLeft = (borderRect.left() - halfBorder + origin.x()) * dpr;
  const qreal outerTop = (borderRect.top() - halfBorder + origin.y()) * dpr;
  const qreal outerRight = (borderRect.right() + halfBorder + origin.x()) * dpr;
  const qreal outerBottom = (borderRect.bottom() + halfBorder + origin.y()) * dpr;

  QVERIFY(qFuzzyIsNull(outerLeft - qRound(outerLeft)));
  QVERIFY(qFuzzyIsNull(outerTop - qRound(outerTop)));
  QVERIFY(qFuzzyIsNull(outerRight - qRound(outerRight)));
  QVERIFY(qFuzzyIsNull(outerBottom - qRound(outerBottom)));
  QCOMPARE(qRound(outerLeft), qCeil((bounds.left() + origin.x()) * dpr));
  QCOMPARE(qRound(outerTop), qCeil((bounds.top() + origin.y()) * dpr));
  QCOMPARE(qRound(outerRight), qFloor((bounds.right() + origin.x()) * dpr));
  QCOMPARE(qRound(outerBottom), qFloor((bounds.bottom() + origin.y()) * dpr));
}

void PaginationTest::rightToLeftLayoutMirrorsNavigation() {
  AdPagination pagination;
  pagination.setTotal(30);
  pagination.setLayoutDirection(Qt::RightToLeft);
  pagination.show();
  QCoreApplication::processEvents();

  const auto* layout = qobject_cast<const QBoxLayout*>(pagination.layout());
  QVERIFY(layout);
  QCOMPARE(layout->direction(), QBoxLayout::LeftToRight);
  auto* previous = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation1"));
  auto* next = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation2"));
  QVERIFY(previous);
  QVERIFY(next);
  QVERIFY(previous->x() > next->x());
  QCOMPARE(previous->layoutDirection(), Qt::RightToLeft);
  QCOMPARE(next->layoutDirection(), Qt::RightToLeft);
}

void PaginationTest::pageWindowMatchesRcPagination() {
  AdPagination pagination;
  pagination.setTotal(500);
  pagination.setCurrentPage(10);

  const QList<int> expectedPages = {1, 8, 9, 10, 11, 12, 50};
  for (int page : expectedPages) {
    QVERIFY2(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage%1").arg(page)),
             qPrintable(QStringLiteral("Missing page %1").arg(page)));
  }
  QVERIFY(!pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage7")));
  QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation3")));
  QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation4")));
}

void PaginationTest::lessItemsUsesSmallerWindow() {
  AdPagination pagination;
  pagination.setTotal(500);
  pagination.setCurrentPage(10);
  pagination.setShowLessItems(true);

  const QList<int> expectedPages = {1, 9, 10, 11, 50};
  for (int page : expectedPages) {
    QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage%1").arg(page)));
  }
  QVERIFY(!pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage8")));
}

void PaginationTest::pageClickEmitsUserChange() {
  AdPagination pagination;
  pagination.setTotal(100);
  QSignalSpy currentSpy(&pagination, &AdPagination::currentPageChanged);
  QSignalSpy changedSpy(&pagination, &AdPagination::changed);
  auto* page2 = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage2"));
  QVERIFY(page2);

  page2->click();
  QCOMPARE(pagination.currentPage(), 2);
  QCOMPARE(currentSpy.count(), 1);
  QCOMPARE(changedSpy.count(), 1);
  QCOMPARE(changedSpy.at(0).at(0).toInt(), 2);
  QCOMPARE(changedSpy.at(0).at(1).toInt(), 10);
}

void PaginationTest::jumpAndQuickJumper() {
  AdPagination pagination;
  pagination.setTotal(500);
  QSignalSpy changedSpy(&pagination, &AdPagination::changed);
  auto* jumpNext = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation4"));
  QVERIFY(jumpNext);
  jumpNext->click();
  QCOMPARE(pagination.currentPage(), 6);

  pagination.setShowQuickJumper(true);
  auto* input = pagination.findChild<AdLineEdit*>(QStringLiteral("paginationQuickInput"));
  QVERIFY(input);
  QVERIFY(pagination.findChild<QWidget*>(QStringLiteral("paginationQuickSuffix")));
  input->setText(QStringLiteral("999"));
  emit input->editingFinished();
  QCOMPARE(pagination.currentPage(), 50);
  QCOMPARE(changedSpy.count(), 2);
}

void PaginationTest::sizeChangerClampsCurrentPage() {
  AdPagination pagination;
  pagination.setTotal(95);
  pagination.setCurrentPage(10);
  pagination.setShowSizeChanger(true);
  QSignalSpy selectedSpy(&pagination, &AdPagination::pageSizeSelected);
  QSignalSpy changedSpy(&pagination, &AdPagination::changed);
  auto* changer = pagination.findChild<AdComboBox*>(QStringLiteral("paginationSizeChanger"));
  QVERIFY(changer);

  changer->setCurrentValue(20);
  QCOMPARE(pagination.pageSize(), 20);
  QCOMPARE(pagination.currentPage(), 5);
  QCOMPARE(selectedSpy.count(), 1);
  QCOMPARE(selectedSpy.at(0).at(0).toInt(), 10);
  QCOMPARE(selectedSpy.at(0).at(1).toInt(), 20);
  QCOMPARE(changedSpy.count(), 1);
}

void PaginationTest::simpleAndSinglePageModes() {
  AdPagination pagination;
  pagination.setTotal(20);
  pagination.setSimple(true);
  pagination.setSimpleReadOnly(true);
  QVERIFY(pagination.findChild<QWidget*>(QStringLiteral("paginationSimpleCurrent"))
              ->isVisibleTo(&pagination));

  pagination.setHideOnSinglePage(true);
  pagination.setTotal(5);
  QCOMPARE(pagination.sizeHint(), QSize(0, 0));
}

void PaginationTest::disabledBlocksNavigation() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setDisabled(true);
  QSignalSpy changedSpy(&pagination, &AdPagination::changed);
  pagination.nextPage();
  QCOMPARE(pagination.currentPage(), 1);
  QCOMPARE(changedSpy.count(), 0);
}

void PaginationTest::standardEnabledStateNotifiesDisabledProperty() {
  AdPagination pagination;
  QSignalSpy disabledSpy(&pagination, &AdPagination::disabledChanged);

  pagination.setEnabled(false);
  QVERIFY(pagination.disabled());
  QCOMPARE(disabledSpy.count(), 1);
  QCOMPARE(disabledSpy.at(0).at(0).toBool(), true);

  pagination.setEnabled(true);
  QVERIFY(!pagination.disabled());
  QCOMPARE(disabledSpy.count(), 2);
  QCOMPARE(disabledSpy.at(1).at(0).toBool(), false);

  pagination.setDisabled(true);
  QCOMPARE(disabledSpy.count(), 3);

  QWidget disabledParent;
  disabledParent.setDisabled(true);
  AdPagination child(&disabledParent);
  QVERIFY(child.disabled());
  QSignalSpy inheritedSpy(&child, &AdPagination::disabledChanged);
  disabledParent.setEnabled(true);
  QVERIFY(!child.disabled());
  QCOMPARE(inheritedSpy.count(), 1);
  QCOMPARE(inheritedSpy.at(0).at(0).toBool(), false);
}

void PaginationTest::navigationPreservesButtonIdentityAndFocus() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.show();
  QCoreApplication::processEvents();

  auto* page2 = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage2"));
  QVERIFY(page2);
  page2->setFocus(Qt::TabFocusReason);
  QVERIFY(page2->hasFocus());
  page2->click();

  QCOMPARE(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage2")), page2);
  QVERIFY(page2->hasFocus());
  QCOMPARE(page2->accessibleDescription(), QStringLiteral("Current page"));
  QVERIFY(page2->isCheckable());
  QVERIFY(page2->isChecked());
}

void PaginationTest::pooledButtonsClearInactiveState() {
  AdPagination pagination;
  pagination.setTotal(500);
  QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage50")));
  QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation4")));

  pagination.setTotal(10);
  QVERIFY(!pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage50")));
  QVERIFY(!pagination.findChild<QAbstractButton*>(QStringLiteral("paginationNavigation4")));

  pagination.setSimple(true);
  QVERIFY(!pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage1")));
  const auto buttons =
      pagination.findChildren<QAbstractButton*>(QString(), Qt::FindDirectChildrenOnly);
  for (const QAbstractButton* button : buttons) {
    if (button->isHidden()) {
      QVERIFY(button->objectName().isEmpty());
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
      // QWidget::accessibleIdentifier() is Qt 6.9; Qt 5.15 has no such property at
      // all, so there is nothing to be empty and the assertion would only restate that.
      QVERIFY(button->accessibleIdentifier().isEmpty());
#endif
      QVERIFY(button->accessibleName().isEmpty());
      QVERIFY(button->accessibleDescription().isEmpty());
    }
  }
}

void PaginationTest::quickJumperPreservesFocus() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setShowQuickJumper(true);
  pagination.show();
  QCoreApplication::processEvents();

  auto* input = pagination.findChild<AdLineEdit*>(QStringLiteral("paginationQuickInput"));
  QVERIFY(input);
  input->setFocus(Qt::TabFocusReason);
  QVERIFY(input->hasFocus());
  input->setText(QStringLiteral("4"));
  emit input->editingFinished();

  QCOMPARE(pagination.currentPage(), 4);
  QVERIFY(input->hasFocus());
}

void PaginationTest::languageChangeRefreshesPersistentText() {
  AdPagination pagination;
  pagination.setTotal(100);
  pagination.setShowQuickJumper(true);
  pagination.setShowSizeChanger(true);
  auto* quickLabel = pagination.findChild<QLabel*>(QStringLiteral("paginationQuickLabel"));
  auto* sizeChanger = pagination.findChild<AdComboBox*>(QStringLiteral("paginationSizeChanger"));
  QVERIFY(quickLabel);
  QVERIFY(sizeChanger);
  QCOMPARE(quickLabel->text(), QStringLiteral("Go to"));
  QCOMPARE(sizeChanger->currentText(), QStringLiteral("10 / page"));

  PaginationTranslator translator;
  QVERIFY(QCoreApplication::installTranslator(&translator));
  QEvent languageChange(QEvent::LanguageChange);
  QCoreApplication::sendEvent(&pagination, &languageChange);
  QCOMPARE(quickLabel->text(), QStringLiteral("translated:Go to"));
  QCOMPARE(sizeChanger->currentText(), QStringLiteral("translated:10 / page"));
  const auto translatedOptions = sizeChanger->options();
  const auto page50 =
      std::find_if(translatedOptions.cbegin(), translatedOptions.cend(),
                   [](const AdComboBox::Option& option) { return option.value.toInt() == 50; });
  QVERIFY(page50 != translatedOptions.cend());
  QCOMPARE(page50->label, QStringLiteral("translated:50 / page"));
  QCOMPARE(pagination.accessibleName(), QStringLiteral("translated:Pagination"));
  QCoreApplication::removeTranslator(&translator);
}

void PaginationTest::maximumIntegerRangeDoesNotOverflow() {
  AdPagination pagination;
  pagination.setPageSize(2);
  pagination.setTotal(std::numeric_limits<int>::max());
  pagination.setCurrentPage(pagination.pageCount());

  QCOMPARE(pagination.pageCount(), 1073741824);
  QCOMPARE(pagination.visibleItemRange().first, std::numeric_limits<int>::max());
  QCOMPARE(pagination.visibleItemRange().last, std::numeric_limits<int>::max());
  QVERIFY(pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage1073741824")));
}

void PaginationTest::disabledPagerRefreshesOnThemeChange() {
  auto& themes = adqt::theme::ThemeManager::instance();
  const adqt::theme::ThemeConfig original = themes.config();
  themes.setPreset(adqt::theme::ThemeScheme::Light);

  AdPagination pagination;
  pagination.setTotal(50);
  pagination.setDisabled(true);
  auto* lightPage = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage1"));
  QVERIFY(lightPage);
  const QColor lightCenter = lightPage->grab().toImage().pixelColor(lightPage->rect().center());

  themes.setPreset(adqt::theme::ThemeScheme::Dark);
  QCoreApplication::processEvents();
  auto* darkPage = pagination.findChild<QAbstractButton*>(QStringLiteral("paginationPage1"));
  QVERIFY(darkPage);
  const QColor darkCenter = darkPage->grab().toImage().pixelColor(darkPage->rect().center());
  QVERIFY2(lightCenter.lightness() > darkCenter.lightness(),
           "Disabled pager did not repaint with the dark theme");

  themes.setConfig(original);
}

QTEST_MAIN(PaginationTest)

#include "tst_pagination.moc"
