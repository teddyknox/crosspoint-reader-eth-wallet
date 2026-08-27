#include "AppRegistry.h"

#include <Memory.h>

#include "EvmWalletActivity.h"
#include "PhoneDashboardActivity.h"
#include "WeatherActivity.h"

namespace custom_apps {
namespace {

constexpr AppDescriptor APPS[] = {
    {AppId::PhoneCalendar, StrId::STR_DAILY_CALENDAR, StrId::STR_DAILY_CALENDAR_DESC, UIIcon::Recent},
    {AppId::Weather, StrId::STR_WEATHER, StrId::STR_WEATHER_DESC, UIIcon::Wifi},
    {AppId::EvmWallet, StrId::STR_EVM_WALLET, StrId::STR_EVM_WALLET_DESC, UIIcon::Transfer},
};

}  // namespace

const AppDescriptor* descriptors() { return APPS; }

size_t count() { return sizeof(APPS) / sizeof(APPS[0]); }

std::unique_ptr<Activity> create(const AppId id, GfxRenderer& renderer, MappedInputManager& mappedInput) {
  switch (id) {
    case AppId::PhoneCalendar:
      return makeUniqueNoThrow<PhoneDashboardActivity>(renderer, mappedInput, false);
    case AppId::Weather:
      return makeUniqueNoThrow<WeatherActivity>(renderer, mappedInput);
    case AppId::EvmWallet:
      return makeUniqueNoThrow<EvmWalletActivity>(renderer, mappedInput);
  }
  return nullptr;
}

}  // namespace custom_apps
