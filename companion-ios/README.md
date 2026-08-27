# X3 Companion for CrossPoint Ethereum Wallet

X3 Companion is the iOS half of this fork. It connects an iPhone to the Xteink X3 over bonded Bluetooth Low Energy and provides two services:

- A background calendar bridge for the X3's Daily Calendar app.
- A WalletConnect and JSON-RPC bridge for the X3's on-device Ethereum signer.

The companion never stores or receives the wallet private key. Requests are signed only after they are reviewed and physically confirmed on the X3.

## Requirements

- macOS with Xcode.
- A physical iPhone running iOS 17 or later.
- An Apple Development team for device signing.
- The matching fork firmware installed on an Xteink X3.
- Bluetooth enabled on both devices.

The app requests Calendar access for Daily Calendar, Camera access for WalletConnect QR scanning, and Bluetooth access for both services.

## Build and run

The Xcode project is checked in:

```bash
open X3Companion.xcodeproj
```

Select your Apple Development team and physical iPhone, then Run. To regenerate the project after changing `project.yml`, install [XcodeGen](https://github.com/yonaskolb/XcodeGen) and run:

```bash
xcodegen generate
```

Swift Package Manager resolves Reown WalletKit and its dependencies from the versions pinned in `Package.resolved`.

## Pair the phone

1. Open **Apps > Daily Calendar** on the X3.
2. Open X3 Companion and enable calendar sync.
3. Enter the six-digit passkey displayed by the X3 in the iOS Bluetooth prompt.
4. Leave Bluetooth enabled after pairing.

The first successfully bonded phone becomes the only trusted companion. To use another phone, select **Settings > System > Forget paired phone** on the X3 and pair again after Bluetooth restarts. Forgetting the phone does not erase the wallet.

## Daily Calendar behavior

The companion reads today's EventKit events, encodes at most eight entries in a bounded snapshot, and keeps a service-filtered Core Bluetooth scan available for state restoration. The X3 wakes every 15 minutes, advertises for 20 seconds, accepts an authenticated snapshot, and updates the e-ink display only when the calendar changed.

iOS background scheduling is best-effort. A missed advertising window is retried on the next X3 wake. Force-quitting the app generally prevents ordinary Core Bluetooth restoration until the companion is opened again.

## Ethereum wallet

Open **Apps > Ethereum Wallet** on the X3 and unlock it before preparing or receiving a signing request. Once the companion reads the X3 address, it enables send, receive, network, and WalletConnect actions.

The send flow obtains the pending nonce, gas estimate, priority fee, and current gas price from JSON-RPC before preparing an EIP-1559 transaction. The X3 reviews and signs it; the companion assembles the typed transaction and submits it.

The receive screen renders an EIP-681-compatible Ethereum address QR code. Always compare the displayed address with the X3 before funding it.

### RPC selection

The default provider is Reown's Blockchain API. Under wallet network settings, a user can provide one custom HTTPS endpoint and decimal chain ID. The companion verifies `eth_chainId` before saving it and stores the configuration in the iOS Keychain. The custom endpoint is used only for that exact chain.

### WalletConnect

The project includes a Reown project ID for the hackathon build. It can be replaced in the app's WalletConnect settings or in the `X3ReownProjectID` build setting.

Pairing supports:

- Camera scanning of a WalletConnect QR code.
- Pasting a raw `wc:` pairing URI.
- WalletConnect deep links containing a pairing URI.

The companion advertises these methods:

- `eth_signTransaction`
- `eth_sendTransaction`
- `eth_signTypedData_v4`
- `personal_sign`

It supports EIP-1559 native transfers, recognized ERC-20 transfers and approvals, arbitrary contract calldata, and type-2 access lists. Flat scalar EIP-712 messages, strict Sign-In with Ethereum messages, and paginated personal messages can also be reviewed and signed.

Legacy/type-0, EIP-2930/type-1, blob/type-3, EIP-7702/type-4, contract deployment, nested typed data, account-abstraction user operations, Safe-specific flows, `wallet_sendCalls`, `eth_sign`, and non-EVM namespaces remain unsupported.

See [Ethereum Wallet Architecture and Security](../docs/ethereum-wallet.md) for the complete trust model, review behavior, and limitations.

## Tests

Run the iOS unit tests against an installed simulator:

```bash
xcodebuild test \
  -project X3Companion.xcodeproj \
  -scheme X3Companion \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  CODE_SIGNING_ALLOWED=NO
```

The suite covers the calendar wire format and CRC, Ethereum transaction encoding, pending-nonce preparation, custom RPC routing, WalletConnect QR parsing, typed and personal signing payloads, access-list encoding, signature assembly, and receive QR rendering.
