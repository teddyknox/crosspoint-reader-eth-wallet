# X3 Companion

This iPhone app is the BLE central for the CrossPoint phone-dashboard fork. It
reads today's events with EventKit, encodes a bounded eight-event snapshot, and
keeps a service-filtered Core Bluetooth scan active in the background. The X3
is the BLE peripheral: it wakes every 15 minutes, advertises for 20 seconds,
accepts an encrypted snapshot, updates the e-ink display only when the calendar
changed, and returns to deep sleep.

## Build and run

1. Run `xcodegen generate` in this directory.
2. Open `X3Companion.xcodeproj` in Xcode.
3. Select your Apple Development team and your iPhone, then Run.
4. Allow Calendar and Bluetooth access.
5. On the X3, open **Apps > Daily Calendar** for the first pairing. Enter the
   six-digit passkey shown by the X3 in the iOS Bluetooth prompt.

The first successfully bonded phone becomes the X3's only trusted phone. Later
BLE connections are restricted to that bond. To replace it, open **Settings >
System > Forget paired phone** on the X3 and pair again after the restart.

After pairing, leave Bluetooth enabled. Background BLE on iOS is best-effort:
delivery is retried on the next X3 wake if iOS does not run the app during a
particular advertising window. Force-quitting the companion app generally
prevents ordinary Core Bluetooth state restoration until the app is opened
again.

## WalletConnect

The experimental wallet uses Reown WalletKit 2.3.1 (WalletConnect). Create a
free project ID at `dashboard.reown.com`, then open **Experimental Wallet >
Connect a dapp** and save it. Paste the dapp's `wc:` pairing URI and approve the
session, or tap **Scan QR** and scan its WalletConnect code. Keep **Ethereum
Wallet** open on the X3 when a transaction arrives.

This build advertises `eth_signTransaction`, `eth_sendTransaction`,
`eth_signTypedData_v4`, and `personal_sign`. It accepts EIP-1559 native/ERC-20
transfers and arbitrary contract calls, flat EIP-712 scalar messages, strict
Sign-In with Ethereum messages whose domain matches the WalletConnect peer, and
arbitrary personal messages only after every escaped message page is reviewed
on the X3. Unknown contract calls display their target, ETH value, selector,
calldata size, and calldata hash before signing. Type-2 access lists are
validated and summarized on a second X3 review page. Nested typed data,
contract creation, and legacy transactions remain unsupported.
Missing transaction fields and submission use Reown's Blockchain API, so a
chain must be supported there unless the dapp provides every signing field and
requests `eth_signTransaction`.

The wallet asks for a six-digit PIN on every entry. Its private key is stored as
an AES-256-GCM ciphertext in ESP32 NVS using a per-wallet salt and nonce; the
decrypted key is wiped from RAM when the wallet closes. This remains hackathon
firmware without a secure element, so use test funds only.
