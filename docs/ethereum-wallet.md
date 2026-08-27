# Ethereum Wallet Architecture and Security

This document describes the experimental wallet added by the `crosspoint-reader-eth-wallet` fork. It is not part of upstream CrossPoint Reader.

> [!WARNING]
> Use test funds only. This project has not been audited, the ESP32-C3 is not a certified secure element, and the current build has no seed phrase, key export, or recovery workflow.

## Security model

The design keeps the Ethereum private key and signing operation on the X3. The iPhone can prepare a request, but it cannot sign one by itself.

1. The X3 generates a 32-byte secp256k1 private key with the ESP32 hardware random-number generator.
2. A six-digit PIN is processed with PBKDF2-HMAC-SHA256 and a random 16-byte salt.
3. The derived 256-bit key encrypts the private key with AES-256-GCM using a random 12-byte nonce and authenticated vault metadata.
4. The ciphertext, salt, nonce, authentication tag, version, and KDF cost are stored in ESP32 NVS.
5. Entering the wallet requires the PIN. Authenticated decryption loads the key only for the active wallet session.
6. The X3 parses and hashes supported requests, displays the review, and produces a canonical secp256k1 signature after physical confirmation.
7. Key material and intermediate secrets are explicitly wiped when they are no longer needed and when the wallet activity closes.

The PIN can be changed inside the wallet. A successful change verifies the current PIN and atomically re-encrypts the same private key with a new salt and nonce.

### BLE trust boundary

The calendar and wallet services use authenticated OS-level BLE bonding. The first successfully paired phone becomes the trusted companion. Connections from other phones are rejected until **Settings > System > Forget paired phone** removes the bond and restarts Bluetooth.

BLE bonding is not a substitute for the wallet PIN. It protects the transport from casual use by another nearby phone; the PIN protects the stored key and gates wallet access.

### What this does not protect against

- Physical extraction or invasive attacks against the ESP32 flash/NVS and runtime memory.
- Malicious firmware installed on the X3.
- A compromised phone or dapp presenting a harmful request that the user nevertheless confirms.
- Semantic deception inside arbitrary calldata. The device shows a selector and hash, not a decoded simulation.
- Loss of the device or forgotten PIN. There is no recovery mechanism in this build.

## Transaction lifecycle

```text
Dapp
  -> WalletConnect request on iPhone
  -> validate account, chain, method, and fields
  -> fill missing pending nonce, gas, and EIP-1559 fees through JSON-RPC
  -> encode unsigned type-2 transaction
  -> bonded BLE request to X3
  -> parse and review on X3
  -> physical confirmation
  -> sign on X3
  -> BLE signature response to iPhone
  -> assemble raw transaction
  -> return it to the dapp or broadcast with eth_sendRawTransaction
```

Only one signing request can be pending. The requested `from` account must match the address reported by the connected X3. Missing transaction network fields are fetched from the selected chain using the pending transaction count, which avoids reusing a nonce while an earlier transaction is awaiting inclusion.

## On-device review

For native transfers, the X3 shows the chain, recipient, amount, maximum network fee, and nonce.

For recognized ERC-20 calls, it decodes:

- `transfer(address,uint256)`
- `approve(address,uint256)`

For other calldata, it shows:

- Contract address.
- ETH value.
- Four-byte selector, when present.
- Calldata byte length.
- Keccak-256 calldata fingerprint.

For a non-empty access list, confirmation advances to a required second page showing the number of addresses, number of storage keys, and a Keccak-256 fingerprint of the canonical RLP-encoded list.

The device does not simulate execution or resolve token metadata. Raw token amounts are shown in contract units because decimals and symbols are not trusted inputs to the signer.

## Supported requests

| Request | Status | Notes |
| --- | --- | --- |
| EIP-1559/type-2 transaction | Supported | Native transfers and arbitrary calldata |
| Type-2 access list | Supported | Exact 20-byte addresses and 32-byte storage keys are required |
| `eth_signTransaction` | Supported | Returns the signed raw transaction |
| `eth_sendTransaction` | Supported | Companion broadcasts the signed raw transaction |
| `eth_signTypedData_v4` | Limited | Flat scalar primary types only; every field is reviewed |
| `personal_sign` | Supported | Printable/escaped message pages are reviewed on the X3 |
| `eth_sign` | Supported | Uses Ethereum's prefixed-message digest and an explicit legacy warning on the X3 |
| Sign-In with Ethereum | Supported | Strict parsing, wallet-address match, nonce validation, and dapp-origin match |
| `wallet_sendCalls` 2.0.0 | Limited | Up to eight ordered calls become non-atomic type-2 EOA transactions |
| `wallet_getCallsStatus` | Supported | Returns pending, confirmed, failed, or partially failed status and receipts |
| `wallet_showCallsStatus` | Supported | Surfaces the known batch status in the companion |
| `wallet_getCapabilities` | Supported | Reports atomic execution as unsupported |

The companion accepts `eip155` namespaces, so the same signer can be used across Ethereum and EVM-compatible L2/L3 networks. A network must be reachable through either Reown's RPC service or the configured custom HTTPS RPC endpoint.

## Unsupported requests

- Legacy/type-0 transactions.
- EIP-2930/type-1 access-list envelopes. Access lists are supported only inside type-2 transactions.
- EIP-4844/type-3 blob transactions.
- EIP-7702/type-4 authorization transactions.
- Contract deployment with an empty `to` field.
- Nested arrays, structs, and other dynamic EIP-712 data.
- ERC-4337 user operations and bundler-specific methods.
- Safe/multisig-specific workflows.
- Atomic wallet-call batches and required EIP-5792 capabilities.
- Non-EVM chains.

Some higher-level actions such as swaps, bridges, staking, multicalls, and NFT transfers can pass through as arbitrary calldata in an ordinary type-2 transaction. This is transport compatibility, not first-class decoding: the X3 cannot explain the resulting state changes.

## RPC selection

By default, the companion uses Reown's Blockchain API for missing transaction fields and submission. The wallet settings can instead store one custom HTTPS RPC URL and its expected decimal chain ID.

Before using a custom endpoint, the companion calls `eth_chainId` and rejects a chain mismatch. The URL is stored in the iOS Keychain with `AfterFirstUnlockThisDeviceOnly` accessibility. A custom endpoint is used only for its configured chain; other chains continue through the default provider.

RPC operators can observe account addresses, transaction queries, and submitted raw transactions. They never receive the private key.

## WalletConnect pairing

The companion uses Reown WalletKit and advertises only:

- `eth_signTransaction`
- `eth_sendTransaction`
- `eth_signTypedData_v4`
- `personal_sign`
- `eth_sign`
- `wallet_sendCalls`
- `wallet_getCallsStatus`
- `wallet_showCallsStatus`
- `wallet_getCapabilities`

A session proposal requesting unsupported required methods, events, namespaces, or no EVM chain is rejected. Pairing is available through the camera scanner, a pasted `wc:` URI, or a compatible deep link.

Keep **Ethereum Wallet** open and unlocked on the X3 while connecting or signing. Closing the wallet stops the wallet BLE service and wipes the decrypted key.

For `wallet_sendCalls`, the companion prepares sequential pending nonces and the X3 shows the call's position in the batch. All calls must be individually approved before the first signed transaction is submitted. The companion retains call-status records for 24 hours while the app process remains alive; this hackathon build does not yet persist those records across app termination.

## Recovery and reset

There is currently no supported way to display, export, import, or recover the private key. Treat a wallet created by this firmware as disposable.

- **Forget paired phone** removes the BLE bond but leaves the encrypted wallet intact.
- **Change PIN** re-encrypts the existing key and does not change the address.
- Reflashing or erasing NVS may permanently destroy the wallet.
- Losing the PIN permanently locks the wallet in the current build.

## Verification

The repository includes native vectors for transaction parsing, access lists, Keccak-256, EIP-712, SIWE, message-signing kinds, batch review metadata, and the BLE wire protocol, plus iOS tests for canonical transaction encoding, RPC preparation, WalletConnect parsing, wallet-call batches, and QR generation.

After changing the wallet, verify the host tests, iOS tests, firmware build, a real X3 boot with heap logging, and a complete testnet signing flow. A successful compile alone does not validate the BLE or e-ink interaction.
