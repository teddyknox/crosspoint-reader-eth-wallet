#pragma once

class NimBLEAdvertising;
class NimBLEConnInfo;

namespace hal_ble_bond {

bool initialize();
bool authorizeOrAdopt(const NimBLEConnInfo& connection);
bool isAuthorized(const NimBLEConnInfo& connection);
bool hasTrustedPhone();
void restrictAdvertising(NimBLEAdvertising& advertising);
bool forgetTrustedPhone();

}  // namespace hal_ble_bond
