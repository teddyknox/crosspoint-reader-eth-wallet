#pragma once

#include <array>

#include "activities/UiListActivity.h"

class CustomAppsActivity final : public UiListActivity {
 public:
  static constexpr size_t APP_COUNT = 2;

  CustomAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

 private:
  std::array<freeink::ui::ListItem, APP_COUNT> rowItems{};
};
