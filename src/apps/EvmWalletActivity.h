#pragma once

#include <EvmSignable.h>
#include <EvmTransaction.h>
#include <EvmWalletProtocol.h>

#include <cstdint>

#include "activities/Activity.h"
#include "wallet/EvmKeyVault.h"

class EvmWalletActivity final : public Activity {
 public:
  EvmWalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class Screen : uint8_t {
    CreateWarning,
    SetPin,
    ConfirmPin,
    UnlockPin,
    Waiting,
    ChangeCurrentPin,
    ChangeNewPin,
    ChangeConfirmPin,
    PinChanged,
    Review,
    Result,
    Error
  };

  Screen screen = Screen::CreateWarning;
  bool approved = false;
  uint32_t lastPasskey = 0;
  uint32_t bleRestartAt = 0;
  uint8_t bleRestartAttempts = 0;
  evm_wallet::WalletState lastState = evm_wallet::WalletState::Stopped;
  uint8_t walletAddress[20]{};
  evm_wallet::SignRequest request{};
  evm_wallet::ParsedTransaction transaction{};
  evm_wallet::ParsedTypedData typedData{};
  evm_wallet::ParsedPersonalMessage personalMessage{};
  evm_wallet::ParsedSiwe siwe{};
  bool personalMessageIsSiwe = false;
  bool personalMessageIsEthSign = false;
  uint8_t reviewPage = 0;
  uint8_t reviewPageCount = 1;
  uint8_t pin[EvmKeyVault::PIN_LENGTH]{};
  uint8_t firstPin[EvmKeyVault::PIN_LENGTH]{};
  uint8_t pinIndex = 0;
  uint8_t failedPinAttempts = 0;
  uint32_t pinLockedUntil = 0;
  uint32_t lastLockoutSeconds = UINT32_MAX;
  bool pinError = false;
  bool pinChangeError = false;

  void startRadio();
  void scheduleRadioRestart();
  void restartRadioIfNeeded();
  void completeUnlock();
  void resetPinEntry();
  void beginPinChange();
  void cancelPinChange();
  void recordFailedPinAttempt(bool lockWallet);
  void handlePinInput();
  void submitPin();
  void handleRequest();
  void approveRequest();
  const uint8_t* requestDigest() const;
  void renderTransactionReview(int y);
  void renderTypedDataReview(int y);
  void renderPersonalMessageReview(int y);
  void renderSiweReview(int y);
  void renderPinEntry(int y);
  const char* statusText() const;
};
