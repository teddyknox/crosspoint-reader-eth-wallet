#include "CustomAppsActivity.h"

#include <I18n.h>

#include "AppRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

CustomAppsActivity::CustomAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("CustomApps", renderer, mappedInput) {
  static_assert(APP_COUNT == 2, "Keep APP_COUNT in sync with the static registry");
  const auto* apps = custom_apps::descriptors();
  for (size_t i = 0; i < custom_apps::count(); ++i) {
    fui::ListItem item;
    item.label = I18N.get(apps[i].title);
    item.subtitle = I18N.get(apps[i].subtitle);
    item.icon = listIconFor(apps[i].icon, 32);
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
  }
}

int CustomAppsActivity::listCount() const { return static_cast<int>(custom_apps::count()); }

const char* CustomAppsActivity::headerTitle() const { return tr(STR_APPS); }

void CustomAppsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto& descriptor = custom_apps::descriptors()[index];
  auto activity = custom_apps::create(descriptor.id, renderer, mappedInput);
  if (activity) activityManager.pushActivity(std::move(activity));
}

void CustomAppsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}
