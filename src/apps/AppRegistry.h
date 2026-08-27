#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <memory>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

namespace custom_apps {

enum class AppId { PhoneCalendar, EvmWallet };

struct AppDescriptor {
  AppId id;
  StrId title;
  StrId subtitle;
  UIIcon icon;
};

const AppDescriptor* descriptors();
size_t count();
std::unique_ptr<Activity> create(AppId id, GfxRenderer& renderer, MappedInputManager& mappedInput);

}  // namespace custom_apps
