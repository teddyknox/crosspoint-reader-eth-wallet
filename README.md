# CrossPoint X3 Ethereum Wallet

An experimental [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) fork that turns an Xteink X3 into a phone-connected Ethereum hardware signer and always-on calendar display.

The private key is generated, encrypted, stored, and used on the X3. The iOS companion handles WalletConnect, chain RPC calls, transaction preparation, and Bluetooth transport, but it never receives the private key.

> [!WARNING]
> This is hackathon firmware, not an audited hardware wallet. The X3 has no secure element, there is no seed backup or recovery flow, and unknown contract calls cannot be explained semantically on the device. Use test funds only.

## What this fork adds

### Ethereum wallet

- Generates a secp256k1 private key on the X3 from the ESP32 hardware RNG.
- Protects wallet entry with a six-digit PIN that can be changed on-device.
- Encrypts the private key in ESP32 NVS with AES-256-GCM and a PIN-derived PBKDF2-HMAC-SHA256 key.
- Keeps signing and address derivation on the X3; decrypted key material is wiped when the wallet closes.
- Displays the wallet address and an Ethereum receive QR code on both the X3 and iPhone.
- Reviews and signs EIP-1559 transactions, EIP-712 messages, Sign-In with Ethereum requests, personal messages, and explicitly labelled legacy `eth_sign` requests.
- Uses OS-level BLE bonding so the first paired phone is the only trusted companion until **Settings > System > Forget paired phone** is selected.

### iOS companion and WalletConnect

- Wallet home, send, receive, network, and WalletConnect screens.
- Configurable calendar selection for the X3's daily calendar, including all and none modes.
- Current conditions and a five-day forecast from Apple Weather, fetched at the phone's current location and cached on the X3.
- WalletConnect v2 pairing by camera QR scan, deep link, or pasted `wc:` URI.
- Automatic pending-nonce lookup, gas estimation, priority-fee lookup, and transaction submission.
- Configurable HTTPS JSON-RPC endpoint, stored in the iOS Keychain and checked against its chain ID.
- BLE transport for sending requests to the X3 and returning only the resulting signature.
- Automatic BLE restart after recoverable radio failures.

### Daily calendar

- Sends a bounded snapshot of today's iOS calendar events to the X3 over BLE.
- Wakes the X3 every 15 minutes and advertises for a 20-second refresh window.
- Updates the retained e-ink screen only when calendar data changes.
- Uses best-effort iOS background Bluetooth delivery; a missed window retries on the next wake.

### App sleep screens

- Daily Calendar, Weather, and Ethereum Wallet can each become the retained e-ink sleep screen.
- Open an app and press the button labelled **Set Sleep**, or choose it under **Settings > Display > Sleep Screen**.
- Calendar and Weather render their latest cached snapshot with Bluetooth off. Wallet remains locked and shows only its cached public address and receive QR code.
- Periodic phone-sync wakes repaint the selected app only when its cached data changed.

### Inherited reader

This fork retains CrossPoint's EPUB reader, file browser, custom fonts, Wi-Fi transfer, OPDS, Calibre, KOReader sync, themes, localization, and other reader features. The original [CrossPoint documentation](./USER_GUIDE.md) remains applicable outside the fork-specific apps.

## How the wallet is split

| Component | Responsibility | Private key access |
| --- | --- | --- |
| X3 firmware | PIN gate, encrypted vault, request parsing, on-screen review, hashing, signing | Yes, only while unlocked |
| iOS companion | WalletConnect, RPC requests, nonce and fee preparation, BLE relay, broadcast | No |
| Dapp / chain RPC | Creates requests and accepts or broadcasts signed results | No |

The iPhone is a network bridge and user interface, not the signer. Every supported signing request must be confirmed with the physical X3 buttons.

## Supported Ethereum requests

- EIP-1559/type-2 native transfers.
- ERC-20 `transfer` and `approve` calls with decoded recipient/spender and amount.
- Arbitrary type-2 smart-contract calldata, including swaps, bridges, staking, multicalls, and NFT transfers when expressed as ordinary contract calls.
- Non-empty EIP-1559 access lists. The X3 validates the RLP structure and shows address count, storage-key count, and a list fingerprint on a second review page.
- `eth_signTransaction` and `eth_sendTransaction`.
- `wallet_sendCalls` 2.0.0 as ordered, non-atomic batches of up to eight type-2 transactions. Every call is approved before the companion broadcasts any of them.
- Flat scalar EIP-712 data through `eth_signTypedData_v4`.
- Strict Sign-In with Ethereum messages and reviewed `personal_sign` and `eth_sign` messages.
- EVM L1s, L2s, and L3s identified by an `eip155` chain ID, subject to RPC availability.

Not currently supported:

- Legacy/type-0 and EIP-2930/type-1 transactions.
- EIP-4844/type-3 blob transactions or EIP-7702/type-4 authorizations.
- Contract deployment because an explicit `to` address is required.
- Nested or dynamic EIP-712 structures.
- Atomic wallet-call batches, ERC-4337 user operations, or Safe-specific flows.
- Non-EVM chains.

Unknown calldata is intentionally allowed for this hackathon build. The X3 shows the target, ETH value, selector, calldata length, and calldata hash, but it does not decode arbitrary contract behavior. See [Ethereum wallet architecture and security](./docs/ethereum-wallet.md) for the complete flow and limitations.

## Install from source

There is no supported binary release yet. Build and flash this fork from source.

### X3 firmware

Prerequisites: a data-capable USB-C cable, Python 3.8+, and [pioarduino](https://github.com/pioarduino/pioarduino) or its VS Code extension.

```bash
git clone --recursive git@github.com:teddyknox/crosspoint-reader-eth-wallet.git
cd crosspoint-reader-eth-wallet
pio run -e default
pio run -e default --target upload
```

If the serial port is not auto-detected, add `--upload-port /dev/cu.usbmodem101` with the actual port for your machine.

Some third-party Xteink units ship with USB flashing locked. Read the upstream [unlock and recovery guidance](https://crosspointreader.com/#unlock-tool) before attempting to flash one of those devices.

### iOS companion

Requirements: macOS, Xcode, an Apple Development team, and a physical iPhone running iOS 17 or later.

```bash
cd companion-ios
open X3Companion.xcodeproj
```

Select your development team and physical iPhone in Xcode, then Run. Allow Bluetooth, Calendar, and Camera access when prompted. The checked-in project already includes the Reown WalletConnect dependencies; `project.yml` is the XcodeGen source if the project needs to be regenerated.

## First setup

1. Open **Apps > Daily Calendar** on the X3.
2. Open X3 Companion on the iPhone and start calendar sync.
3. Enter the six-digit passkey shown by the X3 in the iOS Bluetooth prompt.
4. Open **Apps > Ethereum Wallet** on the X3.
5. Set and confirm a six-digit wallet PIN. The X3 generates and encrypts a new key.
6. Leave the wallet open while the companion reads its address.
7. In the companion, open **Connect a dapp** and scan or paste a WalletConnect pairing URI.
8. Review every request on the X3 and confirm it with the device button.

The BLE bond and wallet PIN protect different boundaries: bonding restricts which phone can connect, while the PIN encrypts the wallet key and gates signing. Forgetting the paired phone does not delete the wallet. Forgetting the PIN does make the wallet inaccessible; there is currently no recovery or export flow.

## Development and verification

Firmware:

```bash
./bin/clang-format-fix -g
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
pio run -e default
```

iOS:

```bash
xcodebuild test \
  -project companion-ios/X3Companion.xcodeproj \
  -scheme X3Companion \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO
```

Repository layout:

- `src/apps/` — Daily Calendar, Weather, and Ethereum Wallet activities.
- `src/wallet/` and `lib/EvmWallet/` — encrypted key vault, Ethereum parsing, hashing, and signing.
- `lib/hal/HalBle*` — bonded BLE services for calendar sync and wallet requests.
- `lib/PhoneSync/` and `src/phone/` — bounded calendar/weather wire protocols and snapshot persistence.
- `companion-ios/` — SwiftUI companion, WalletConnect bridge, RPC client, and iOS tests.
- `test/evm_wallet/` and `test/phone_sync/` — native protocol and cryptographic test coverage.

## Documentation

- [Ethereum wallet architecture and security](./docs/ethereum-wallet.md)
- [iOS companion guide](./companion-ios/README.md)
- [CrossPoint user guide](./USER_GUIDE.md)
- [Recovering a bricked Xteink](./docs/fix-bricked-xteink.md)
- [Firmware development guide](./docs/contributing/README.md)

## Upstream and status

This repository is an independent, experimental fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader). It is not an official CrossPoint release and is not affiliated with CrossPoint, Xteink, WalletConnect/Reown, Ledger, Trezor, or any device manufacturer.

The inherited firmware is licensed under the repository's existing [LICENSE](./LICENSE). Upstream reader issues should be reproduced against upstream CrossPoint before being reported there; fork-specific wallet, BLE, calendar, and companion issues belong in this repository.
