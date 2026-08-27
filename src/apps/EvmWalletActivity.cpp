#include "EvmWalletActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalBleEvmWallet.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "wallet/EvmKeyVault.h"

namespace {

constexpr size_t PERSONAL_MESSAGE_LINE_LENGTH = 36;
constexpr size_t PERSONAL_MESSAGE_LINES = 3;
constexpr size_t PERSONAL_MESSAGE_PAGE_LENGTH = PERSONAL_MESSAGE_LINE_LENGTH * PERSONAL_MESSAGE_LINES;
constexpr uint32_t BLE_RESTART_BASE_DELAY_MS = 1000;
constexpr uint32_t BLE_RESTART_MAX_DELAY_MS = 8000;

size_t escapedLength(const uint8_t value) {
  if (value == '\\' || value == '\n' || value == '\r' || value == '\t') return 2;
  return value >= 0x20 && value <= 0x7e ? 1 : 4;
}

uint8_t personalMessagePageCount(const evm_wallet::ByteSpan message) {
  size_t offset = 0;
  uint8_t pages = 0;
  while (offset < message.length) {
    size_t used = 0;
    while (offset < message.length && used + escapedLength(message.data[offset]) <= PERSONAL_MESSAGE_PAGE_LENGTH) {
      used += escapedLength(message.data[offset++]);
    }
    ++pages;
  }
  return pages;
}

void formatPersonalMessagePage(const evm_wallet::ByteSpan message, const uint8_t targetPage,
                               char output[PERSONAL_MESSAGE_LINES][PERSONAL_MESSAGE_LINE_LENGTH + 1]) {
  static constexpr char HEX_CHARS[] = "0123456789abcdef";
  char escaped[PERSONAL_MESSAGE_PAGE_LENGTH + 1]{};
  size_t offset = 0;
  uint8_t page = 0;
  while (offset < message.length) {
    size_t used = 0;
    while (offset < message.length && used + escapedLength(message.data[offset]) <= PERSONAL_MESSAGE_PAGE_LENGTH) {
      const uint8_t value = message.data[offset++];
      if (page == targetPage) {
        if (value == '\\') {
          escaped[used++] = '\\';
          escaped[used++] = '\\';
        } else if (value == '\n' || value == '\r' || value == '\t') {
          escaped[used++] = '\\';
          escaped[used++] = value == '\n' ? 'n' : value == '\r' ? 'r' : 't';
        } else if (value >= 0x20 && value <= 0x7e) {
          escaped[used++] = static_cast<char>(value);
        } else {
          escaped[used++] = '\\';
          escaped[used++] = 'x';
          escaped[used++] = HEX_CHARS[value >> 4U];
          escaped[used++] = HEX_CHARS[value & 0x0fU];
        }
      } else {
        used += escapedLength(value);
      }
    }
    if (page == targetPage) {
      for (size_t line = 0; line < PERSONAL_MESSAGE_LINES; ++line) {
        const size_t start = line * PERSONAL_MESSAGE_LINE_LENGTH;
        const size_t remaining = used > start ? used - start : 0;
        const size_t count = remaining < PERSONAL_MESSAGE_LINE_LENGTH ? remaining : PERSONAL_MESSAGE_LINE_LENGTH;
        if (count > 0) std::memcpy(output[line], escaped + start, count);
        output[line][count] = '\0';
      }
      return;
    }
    ++page;
  }
}

}  // namespace

EvmWalletActivity::EvmWalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("EvmWallet", renderer, mappedInput) {}

void EvmWalletActivity::startRadio() {
  bleRestartAt = 0;
  const bool started = EVM_WALLET_BLE.begin(walletAddress);
  lastState = EVM_WALLET_BLE.state();
  lastPasskey = EVM_WALLET_BLE.pairingPasskey();
  if (started) {
    bleRestartAttempts = 0;
  } else {
    scheduleRadioRestart();
  }
}

void EvmWalletActivity::scheduleRadioRestart() {
  if (bleRestartAt != 0) return;
  const uint8_t exponent = bleRestartAttempts > 3 ? 3 : bleRestartAttempts;
  uint32_t delayMs = BLE_RESTART_BASE_DELAY_MS << exponent;
  if (delayMs > BLE_RESTART_MAX_DELAY_MS) delayMs = BLE_RESTART_MAX_DELAY_MS;
  if (bleRestartAttempts < UINT8_MAX) ++bleRestartAttempts;
  bleRestartAt = millis() + delayMs;
  requestUpdate();
}

void EvmWalletActivity::restartRadioIfNeeded() {
  if (EVM_WALLET_BLE.state() == evm_wallet::WalletState::Error &&
      EVM_WALLET_BLE.error() == evm_wallet::WalletError::RadioFailure) {
    if (screen == Screen::Review) {
      request = {};
      screen = Screen::Waiting;
    }
    scheduleRadioRestart();
  }
  if (bleRestartAt == 0 || static_cast<int32_t>(millis() - bleRestartAt) < 0) return;

  bleRestartAt = 0;
  EVM_WALLET_BLE.end();
  startRadio();
}

void EvmWalletActivity::completeUnlock() {
  if (EVM_KEY_VAULT.address(walletAddress)) {
    failedPinAttempts = 0;
    pinLockedUntil = 0;
    pinError = false;
    pinChangeError = false;
    screen = Screen::Waiting;
    startRadio();
  } else {
    screen = Screen::Error;
  }
}

void EvmWalletActivity::resetPinEntry() {
  std::memset(pin, 0, sizeof(pin));
  pinIndex = 0;
}

void EvmWalletActivity::beginPinChange() {
  EVM_WALLET_BLE.end();
  bleRestartAt = 0;
  bleRestartAttempts = 0;
  std::memset(firstPin, 0, sizeof(firstPin));
  resetPinEntry();
  pinError = false;
  pinChangeError = false;
  screen = Screen::ChangeCurrentPin;
  requestUpdate();
}

void EvmWalletActivity::cancelPinChange() {
  std::memset(firstPin, 0, sizeof(firstPin));
  resetPinEntry();
  pinError = false;
  pinChangeError = false;
  screen = Screen::Waiting;
  startRadio();
  requestUpdate();
}

void EvmWalletActivity::recordFailedPinAttempt(const bool lockWallet) {
  ++failedPinAttempts;
  resetPinEntry();
  pinError = true;
  if (failedPinAttempts < 3) return;

  const uint8_t exponent = failedPinAttempts - 3U > 4U ? 4U : failedPinAttempts - 3U;
  uint32_t delaySeconds = 5U << exponent;
  if (delaySeconds > 60U) delaySeconds = 60U;
  pinLockedUntil = millis() + delaySeconds * 1000U;
  lastLockoutSeconds = delaySeconds;
  if (lockWallet) {
    EVM_KEY_VAULT.lock();
    std::memset(firstPin, 0, sizeof(firstPin));
    screen = Screen::UnlockPin;
  }
}

void EvmWalletActivity::onEnter() {
  Activity::onEnter();
  EVM_KEY_VAULT.lock();
  resetPinEntry();
  screen = EVM_KEY_VAULT.isEncrypted() ? Screen::UnlockPin
           : EVM_KEY_VAULT.exists()    ? Screen::SetPin
                                       : Screen::CreateWarning;
  requestUpdate();
}

void EvmWalletActivity::onExit() {
  EVM_WALLET_BLE.end();
  EVM_KEY_VAULT.lock();
  std::memset(pin, 0, sizeof(pin));
  std::memset(firstPin, 0, sizeof(firstPin));
  Activity::onExit();
}

bool EvmWalletActivity::preventAutoSleep() { return EVM_WALLET_BLE.isActive() || bleRestartAt != 0; }

void EvmWalletActivity::handleRequest() {
  bool valid = false;
  const auto kind = static_cast<evm_wallet::SignRequestKind>(request.kind);
  if (kind == evm_wallet::SignRequestKind::Eip1559Transaction) {
    valid =
        evm_wallet::parseEip1559(request.payload, request.payloadLength, transaction) == evm_wallet::ParseError::None;
    reviewPageCount = valid && transaction.hasAccessList ? 2 : 1;
  } else if (kind == evm_wallet::SignRequestKind::Eip712TypedData) {
    valid =
        evm_wallet::parseEip712(request.payload, request.payloadLength, typedData) == evm_wallet::SignableError::None;
    reviewPageCount = static_cast<uint8_t>(typedData.messageFieldCount + 1);
  } else if (kind == evm_wallet::SignRequestKind::PersonalMessage) {
    const auto personalError =
        evm_wallet::parsePersonalMessage(request.payload, request.payloadLength, personalMessage);
    const auto siweError = evm_wallet::parseSiwe(request.payload, request.payloadLength, walletAddress, siwe);
    personalMessageIsSiwe = siweError == evm_wallet::SignableError::None;
    valid =
        personalError == evm_wallet::SignableError::None && (personalMessageIsSiwe || !personalMessage.looksLikeSiwe);
    reviewPageCount =
        personalMessageIsSiwe ? 2 : static_cast<uint8_t>(personalMessagePageCount(personalMessage.message) + 1);
  }
  if (!valid) {
    EVM_WALLET_BLE.reject(request.requestId, evm_wallet::WalletError::UnsupportedTransaction);
    screen = Screen::Error;
    requestUpdate();
    return;
  }
  approved = false;
  reviewPage = 0;
  screen = Screen::Review;
  requestUpdate();
}

void EvmWalletActivity::approveRequest() {
  uint8_t signature[65]{};
  const uint8_t* digest = requestDigest();
  approved = digest && EVM_KEY_VAULT.signDigest(digest, signature);
  if (approved) {
    EVM_WALLET_BLE.approve(request.requestId, digest, signature);
    screen = Screen::Result;
  } else {
    EVM_WALLET_BLE.reject(request.requestId, evm_wallet::WalletError::SigningFailed);
    screen = Screen::Error;
  }
  std::memset(signature, 0, sizeof(signature));
  requestUpdate();
}

const uint8_t* EvmWalletActivity::requestDigest() const {
  const auto kind = static_cast<evm_wallet::SignRequestKind>(request.kind);
  if (kind == evm_wallet::SignRequestKind::Eip1559Transaction) return transaction.digest;
  if (kind == evm_wallet::SignRequestKind::Eip712TypedData) return typedData.digest;
  if (kind == evm_wallet::SignRequestKind::PersonalMessage) return personalMessage.digest;
  return nullptr;
}

void EvmWalletActivity::submitPin() {
  if (screen == Screen::SetPin) {
    std::memcpy(firstPin, pin, sizeof(firstPin));
    resetPinEntry();
    pinError = false;
    screen = Screen::ConfirmPin;
    requestUpdate();
    return;
  }
  if (screen == Screen::ConfirmPin) {
    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(pin); ++i) difference |= pin[i] ^ firstPin[i];
    if (difference != 0) {
      std::memset(firstPin, 0, sizeof(firstPin));
      resetPinEntry();
      pinError = true;
      screen = Screen::SetPin;
      requestUpdate();
      return;
    }
    const bool secured = EVM_KEY_VAULT.createOrEncrypt(pin);
    std::memset(firstPin, 0, sizeof(firstPin));
    resetPinEntry();
    if (secured)
      completeUnlock();
    else
      screen = Screen::Error;
    requestUpdate();
    return;
  }

  if (screen == Screen::ChangeCurrentPin) {
    if (EVM_KEY_VAULT.verifyPin(pin)) {
      failedPinAttempts = 0;
      pinLockedUntil = 0;
      pinError = false;
      resetPinEntry();
      screen = Screen::ChangeNewPin;
    } else {
      recordFailedPinAttempt(true);
    }
    requestUpdate();
    return;
  }

  if (screen == Screen::ChangeNewPin) {
    std::memcpy(firstPin, pin, sizeof(firstPin));
    resetPinEntry();
    pinError = false;
    screen = Screen::ChangeConfirmPin;
    requestUpdate();
    return;
  }

  if (screen == Screen::ChangeConfirmPin) {
    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(pin); ++i) difference |= pin[i] ^ firstPin[i];
    if (difference != 0) {
      std::memset(firstPin, 0, sizeof(firstPin));
      resetPinEntry();
      pinError = true;
      screen = Screen::ChangeNewPin;
      requestUpdate();
      return;
    }

    const bool changed = EVM_KEY_VAULT.changePin(pin);
    std::memset(firstPin, 0, sizeof(firstPin));
    resetPinEntry();
    pinError = false;
    pinChangeError = !changed;
    screen = changed ? Screen::PinChanged : EVM_KEY_VAULT.isUnlocked() ? Screen::Error : Screen::UnlockPin;
    if (EVM_KEY_VAULT.isUnlocked()) startRadio();
    requestUpdate();
    return;
  }

  if (screen != Screen::UnlockPin) return;
  if (EVM_KEY_VAULT.unlock(pin)) {
    resetPinEntry();
    completeUnlock();
  } else {
    recordFailedPinAttempt(false);
  }
  requestUpdate();
}

void EvmWalletActivity::handlePinInput() {
  if (pinLockedUntil != 0) {
    const int32_t remainingMs = static_cast<int32_t>(pinLockedUntil - millis());
    if (remainingMs > 0) {
      const uint32_t remainingSeconds = (static_cast<uint32_t>(remainingMs) + 999U) / 1000U;
      if (remainingSeconds != lastLockoutSeconds) {
        lastLockoutSeconds = remainingSeconds;
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
      return;
    }
    pinLockedUntil = 0;
    lastLockoutSeconds = UINT32_MAX;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (screen == Screen::ConfirmPin) {
      std::memset(firstPin, 0, sizeof(firstPin));
      resetPinEntry();
      pinError = false;
      screen = Screen::SetPin;
      requestUpdate();
    } else if (screen == Screen::ChangeCurrentPin) {
      cancelPinChange();
    } else if (screen == Screen::ChangeNewPin) {
      std::memset(firstPin, 0, sizeof(firstPin));
      resetPinEntry();
      pinError = false;
      screen = Screen::ChangeCurrentPin;
      requestUpdate();
    } else if (screen == Screen::ChangeConfirmPin) {
      std::memset(firstPin, 0, sizeof(firstPin));
      resetPinEntry();
      pinError = false;
      screen = Screen::ChangeNewPin;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && pinIndex > 0) {
    --pinIndex;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && pinIndex + 1 < sizeof(pin)) {
    ++pinIndex;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    pin[pinIndex] = static_cast<uint8_t>((pin[pinIndex] + 1U) % 10U);
    pinError = false;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    pin[pinIndex] = static_cast<uint8_t>((pin[pinIndex] + 9U) % 10U);
    pinError = false;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (pinIndex + 1 < sizeof(pin)) {
      ++pinIndex;
      requestUpdate();
    } else {
      submitPin();
    }
  }
}

void EvmWalletActivity::loop() {
  if (screen == Screen::CreateWarning) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      screen = Screen::SetPin;
      pinError = false;
      resetPinEntry();
      requestUpdate();
    }
    return;
  }

  if (screen == Screen::SetPin || screen == Screen::ConfirmPin || screen == Screen::UnlockPin ||
      screen == Screen::ChangeCurrentPin || screen == Screen::ChangeNewPin || screen == Screen::ChangeConfirmPin) {
    handlePinInput();
    return;
  }

  if (screen == Screen::Waiting && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    beginPinChange();
    return;
  }

  restartRadioIfNeeded();

  if (EVM_WALLET_BLE.takeRequest(request)) handleRequest();
  if (screen == Screen::Review) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      EVM_WALLET_BLE.reject(request.requestId);
      screen = Screen::Waiting;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) && reviewPage > 0) {
      --reviewPage;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) && reviewPage + 1 < reviewPageCount) {
      ++reviewPage;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (reviewPage + 1 < reviewPageCount) {
        ++reviewPage;
        requestUpdate();
      } else {
        approveRequest();
      }
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (screen == Screen::Result || screen == Screen::PinChanged ||
        (screen == Screen::Error && EVM_KEY_VAULT.isUnlocked())) {
      screen = Screen::Waiting;
      pinChangeError = false;
    } else if (screen == Screen::Error) {
      screen = Screen::UnlockPin;
      resetPinEntry();
      pinChangeError = false;
    }
    requestUpdate();
  }

  const auto state = EVM_WALLET_BLE.state();
  const uint32_t passkey = EVM_WALLET_BLE.pairingPasskey();
  if (state != lastState || passkey != lastPasskey) {
    lastState = state;
    lastPasskey = passkey;
    requestUpdate();
  }
}

void EvmWalletActivity::renderTransactionReview(const int y) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (reviewPage == 1 && transaction.hasAccessList) {
    char value[48];
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_EVM_ACCESS_LIST), true,
                      EpdFontFamily::BOLD);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(transaction.accessListAddressCount));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 48, tr(STR_EVM_ADDRESSES), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 120, y + 48, value, true);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(transaction.accessListStorageKeyCount));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 88, tr(STR_EVM_STORAGE_KEYS), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 120, y + 88, value, true);
    snprintf(value, sizeof(value), "0x%02x%02x%02x%02x...%02x%02x%02x%02x", transaction.accessListHash[0],
             transaction.accessListHash[1], transaction.accessListHash[2], transaction.accessListHash[3],
             transaction.accessListHash[28], transaction.accessListHash[29], transaction.accessListHash[30],
             transaction.accessListHash[31]);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 128, tr(STR_EVM_LIST_HASH), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 164, value, true);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 204, "2 / 2", true);
    return;
  }

  char chain[32];
  char recipient[14];
  char amount[96];
  const bool contractCall = transaction.kind == evm_wallet::TransactionKind::ContractCall;
  snprintf(chain, sizeof(chain), "Chain ID %llu", static_cast<unsigned long long>(transaction.chainId));
  evm_wallet::formatShortAddress(transaction.recipient, recipient);
  if (transaction.kind == evm_wallet::TransactionKind::NativeTransfer || contractCall) {
    evm_wallet::formatWei(transaction.value, amount, sizeof(amount));
  } else {
    evm_wallet::formatInteger(transaction.value, amount, sizeof(amount));
  }
  const char* action = transaction.kind == evm_wallet::TransactionKind::NativeTransfer  ? tr(STR_EVM_SEND_NATIVE)
                       : transaction.kind == evm_wallet::TransactionKind::Erc20Transfer ? tr(STR_EVM_SEND_TOKEN)
                       : transaction.kind == evm_wallet::TransactionKind::Erc20Approval ? tr(STR_EVM_APPROVE_TOKEN)
                                                                                        : tr(STR_EVM_CONTRACT_CALL);
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, action, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 38, chain, true);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 70,
                    contractCall ? tr(STR_EVM_CONTRACT) : tr(STR_EVM_TO), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 90, y + 70, recipient, true);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 102,
                    contractCall ? tr(STR_EVM_ETH_VALUE) : tr(STR_EVM_AMOUNT), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 90, y + 102, amount, true);
  if (contractCall) {
    char selector[11];
    if (transaction.hasSelector) {
      snprintf(selector, sizeof(selector), "0x%02x%02x%02x%02x", transaction.selector[0], transaction.selector[1],
               transaction.selector[2], transaction.selector[3]);
    } else {
      snprintf(selector, sizeof(selector), "%s", tr(STR_EVM_NO_SELECTOR));
    }
    char calldata[48];
    snprintf(calldata, sizeof(calldata), "%u bytes 0x%02x%02x%02x%02x...%02x%02x%02x%02x",
             static_cast<unsigned>(transaction.calldataLength), transaction.calldataHash[0],
             transaction.calldataHash[1], transaction.calldataHash[2], transaction.calldataHash[3],
             transaction.calldataHash[28], transaction.calldataHash[29], transaction.calldataHash[30],
             transaction.calldataHash[31]);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 142, tr(STR_EVM_SELECTOR), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 90, y + 142, selector, true);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 172, tr(STR_EVM_CALLDATA), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 90, y + 172, calldata, true);
  } else if (transaction.kind != evm_wallet::TransactionKind::NativeTransfer) {
    char contract[14];
    evm_wallet::formatShortAddress(transaction.contract, contract);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 142, tr(STR_EVM_CONTRACT), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 90, y + 142, contract, true);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 168, tr(STR_EVM_RAW_UNITS), true);
  }
  if (transaction.hasAccessList) {
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 204, "1 / 2", true);
  }
}

void EvmWalletActivity::renderTypedDataReview(const int y) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char label[40];
  char value[128];
  if (reviewPage == 0) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_EVM_REVIEW_TYPED), true,
                      EpdFontFamily::BOLD);
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(typedData.chainId));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 38, tr(STR_EVM_CHAIN), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 38, value, true);
    evm_wallet::copySpan(typedData.origin, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 70, tr(STR_EVM_DAPP), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 70, value, true);
    evm_wallet::copySpan(typedData.primaryType, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 102, tr(STR_EVM_TYPE), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 102, value, true);
    if (typedData.hasVerifyingContract) {
      evm_wallet::formatShortAddress(typedData.verifyingContract, value);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 134, tr(STR_EVM_CONTRACT), true,
                        EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 134, value, true);
    }
  } else {
    const auto& field = typedData.messageFields[reviewPage - 1];
    evm_wallet::copySpan(field.name, label, sizeof(label));
    evm_wallet::formatTypedValue(field, value, sizeof(value));
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_EVM_REVIEW_FIELD), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 52, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 92, value, true);
  }
  snprintf(label, sizeof(label), "%u / %u", static_cast<unsigned>(reviewPage + 1),
           static_cast<unsigned>(reviewPageCount));
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 178, label, true);
}

void EvmWalletActivity::renderSiweReview(const int y) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char value[192];
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_EVM_SIGN_IN), true, EpdFontFamily::BOLD);
  if (reviewPage == 0) {
    evm_wallet::copySpan(siwe.domain, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 42, tr(STR_EVM_DAPP), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 42, value, true);
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(siwe.chainId));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 74, tr(STR_EVM_CHAIN), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 74, value, true);
    evm_wallet::copySpan(siwe.uri, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 106, tr(STR_EVM_URI), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 106, value, true);
  } else {
    evm_wallet::copySpan(siwe.statement, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 42, tr(STR_EVM_STATEMENT), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 72,
                      siwe.statement.length == 0 ? tr(STR_EVM_NONE) : value, true);
    evm_wallet::copySpan(siwe.nonce, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 112, tr(STR_EVM_NONCE), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 112, value, true);
  }
}

void EvmWalletActivity::renderPersonalMessageReview(const int y) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char value[128]{};
  char pageLabel[32];
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_EVM_SIGN_MESSAGE), true, EpdFontFamily::BOLD);
  if (reviewPage == 0) {
    evm_wallet::copySpan(personalMessage.origin, value, sizeof(value));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 42, tr(STR_EVM_DAPP), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 42, value, true);
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(personalMessage.chainId));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 74, tr(STR_EVM_CHAIN), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 74, value, true);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(personalMessage.message.length));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 106, tr(STR_EVM_MESSAGE_BYTES), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 100, y + 106, value, true);
  } else {
    char lines[PERSONAL_MESSAGE_LINES][PERSONAL_MESSAGE_LINE_LENGTH + 1]{};
    formatPersonalMessagePage(personalMessage.message, static_cast<uint8_t>(reviewPage - 1), lines);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 38, tr(STR_EVM_MESSAGE), true,
                      EpdFontFamily::BOLD);
    for (size_t line = 0; line < PERSONAL_MESSAGE_LINES; ++line) {
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 68 + static_cast<int>(line) * 30, lines[line],
                        true);
    }
  }
  snprintf(pageLabel, sizeof(pageLabel), "%u / %u", static_cast<unsigned>(reviewPage + 1),
           static_cast<unsigned>(reviewPageCount));
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 178, pageLabel, true);
}

void EvmWalletActivity::renderPinEntry(const int y) {
  const int width = renderer.getScreenWidth();
  const char* title = screen == Screen::SetPin                ? tr(STR_EVM_SET_PIN)
                      : screen == Screen::ConfirmPin          ? tr(STR_EVM_CONFIRM_PIN)
                      : screen == Screen::ChangeCurrentPin    ? tr(STR_EVM_CURRENT_PIN)
                      : screen == Screen::ChangeNewPin        ? tr(STR_EVM_NEW_PIN)
                      : screen == Screen::ChangeConfirmPin    ? tr(STR_EVM_CONFIRM_NEW_PIN)
                                                              : tr(STR_EVM_ENTER_PIN);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 12, title, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, y + 50, tr(STR_EVM_PIN_HELP), true);

  constexpr int boxSize = 44;
  constexpr int boxGap = 10;
  const int totalWidth = EvmKeyVault::PIN_LENGTH * boxSize + (EvmKeyVault::PIN_LENGTH - 1) * boxGap;
  const int startX = (width - totalWidth) / 2;
  for (uint8_t index = 0; index < EvmKeyVault::PIN_LENGTH; ++index) {
    const int x = startX + index * (boxSize + boxGap);
    renderer.drawRect(x, y + 86, boxSize, boxSize, true);
    if (index == pinIndex) renderer.drawRect(x + 2, y + 88, boxSize - 4, boxSize - 4, true);
    char digit[2] = {index == pinIndex ? static_cast<char>('0' + pin[index]) : '*', '\0'};
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, digit, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (boxSize - textWidth) / 2, y + 93, digit, true, EpdFontFamily::BOLD);
  }

  if (pinLockedUntil != 0) {
    char message[64];
    snprintf(message, sizeof(message), tr(STR_EVM_PIN_WAIT), static_cast<unsigned long>(lastLockoutSeconds));
    renderer.drawCenteredText(SMALL_FONT_ID, y + 154, message, true, EpdFontFamily::BOLD);
  } else if (pinError) {
    renderer.drawCenteredText(SMALL_FONT_ID, y + 154,
                              screen == Screen::UnlockPin || screen == Screen::ChangeCurrentPin
                                  ? tr(STR_EVM_WRONG_PIN)
                                  : tr(STR_EVM_PIN_MISMATCH),
                              true,
                              EpdFontFamily::BOLD);
  }

  const char* action = pinIndex + 1 < EvmKeyVault::PIN_LENGTH ? tr(STR_EVM_NEXT)
                       : screen == Screen::UnlockPin          ? tr(STR_EVM_UNLOCK)
                       : screen == Screen::ChangeCurrentPin   ? tr(STR_EVM_NEXT)
                       : screen == Screen::ChangeNewPin       ? tr(STR_EVM_NEXT)
                                                              : tr(STR_CONFIRM);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), action, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

const char* EvmWalletActivity::statusText() const {
  switch (EVM_WALLET_BLE.state()) {
    case evm_wallet::WalletState::Advertising:
      return tr(STR_EVM_WAITING_PHONE);
    case evm_wallet::WalletState::Connected:
      return tr(STR_PHONE_SYNC_CONNECTED);
    case evm_wallet::WalletState::Pairing:
      return tr(STR_PHONE_SYNC_PAIRING);
    case evm_wallet::WalletState::Receiving:
      return tr(STR_EVM_RECEIVING);
    case evm_wallet::WalletState::Approved:
      return tr(STR_EVM_SIGNED);
    case evm_wallet::WalletState::Rejected:
      return tr(STR_EVM_REJECTED);
    case evm_wallet::WalletState::Error:
      return tr(STR_EVM_FAILED);
    default:
      return tr(STR_EVM_FAILED);
  }
}

void EvmWalletActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_EVM_WALLET));
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  if (screen == Screen::CreateWarning) {
    renderer.drawCenteredText(UI_12_FONT_ID, y + 20, tr(STR_EVM_EXPERIMENTAL), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, y + 62, tr(STR_EVM_PIN_PROTECTION), true);
    renderer.drawCenteredText(UI_10_FONT_ID, y + 94, tr(STR_EVM_TEST_FUNDS_ONLY), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_EVM_CREATE_WALLET), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (screen == Screen::SetPin || screen == Screen::ConfirmPin || screen == Screen::UnlockPin ||
             screen == Screen::ChangeCurrentPin || screen == Screen::ChangeNewPin ||
             screen == Screen::ChangeConfirmPin) {
    renderPinEntry(y);
  } else if (screen == Screen::Review) {
    const auto kind = static_cast<evm_wallet::SignRequestKind>(request.kind);
    if (kind == evm_wallet::SignRequestKind::Eip1559Transaction)
      renderTransactionReview(y);
    else if (kind == evm_wallet::SignRequestKind::Eip712TypedData)
      renderTypedDataReview(y);
    else if (personalMessageIsSiwe)
      renderSiweReview(y);
    else
      renderPersonalMessageReview(y);
    const auto labels = mappedInput.mapLabels(
        tr(STR_CANCEL), reviewPage + 1 < reviewPageCount ? tr(STR_EVM_NEXT) : tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    char address[43];
    evm_wallet::formatAddress(walletAddress, address);
    int passkeyY = y + 164;
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, width, metrics.tabBarHeight},
                      tr(STR_EVM_EXPERIMENTAL), screen == Screen::PinChanged ? tr(STR_EVM_PIN_CHANGED) : statusText());
    y += metrics.tabBarHeight;
    renderer.drawCenteredText(SMALL_FONT_ID, y + 28, tr(STR_EVM_ADDRESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, y + 58, address, true);
    if (screen == Screen::Result) {
      renderer.drawCenteredText(UI_12_FONT_ID, y + 116, tr(STR_EVM_SIGNED), true, EpdFontFamily::BOLD);
    } else if (screen == Screen::PinChanged) {
      renderer.drawCenteredText(UI_12_FONT_ID, y + 116, tr(STR_EVM_PIN_CHANGED), true, EpdFontFamily::BOLD);
    } else if (screen == Screen::Error) {
      renderer.drawCenteredText(UI_12_FONT_ID, y + 116,
                                pinChangeError ? tr(STR_EVM_PIN_CHANGE_FAILED) : tr(STR_EVM_FAILED), true,
                                EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y + 116, tr(STR_EVM_OPEN_COMPANION), true);
      constexpr int QR_SIZE = 220;
      constexpr int QR_TOP_OFFSET = 142;
      const Rect qrBounds{(width - QR_SIZE) / 2, y + QR_TOP_OFFSET, QR_SIZE, QR_SIZE};
      QrUtils::drawQrCode(renderer, qrBounds, std::string("ethereum:") + address);
      passkeyY = qrBounds.y + qrBounds.height + 24;
    }
    const uint32_t passkey = EVM_WALLET_BLE.pairingPasskey();
    if (passkey != 0) {
      char passkeyText[48];
      snprintf(passkeyText, sizeof(passkeyText), tr(STR_PHONE_SYNC_PASSKEY), static_cast<unsigned long>(passkey));
      renderer.drawCenteredText(UI_12_FONT_ID, passkeyY, passkeyText, true, EpdFontFamily::BOLD);
    }
    const char* confirmLabel = screen == Screen::Waiting ? "" : tr(STR_DONE);
    const char* changePinLabel = screen == Screen::Waiting ? tr(STR_EVM_CHANGE_PIN) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, changePinLabel, "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
