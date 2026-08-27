import Foundation

enum EvmTransactionError: LocalizedError {
    case invalidNumber(String)
    case invalidAddress
    case invalidData
    case invalidAccessList
    case transactionTooLarge
    case invalidSignature

    var errorDescription: String? {
        switch self {
        case .invalidNumber(let field): "Invalid \(field)"
        case .invalidAddress: "Recipient must be a 20-byte 0x address"
        case .invalidData: "Transaction data must be even-length hexadecimal"
        case .invalidAccessList: "Transaction access list is malformed"
        case .transactionTooLarge: "Transaction is too large for the X3 review protocol"
        case .invalidSignature: "The X3 returned an invalid signature"
        }
    }
}

struct PreparedEvmTransaction {
    let requestID: UInt32
    let unsignedTransaction: Data
    fileprivate let unsignedFields: [Data]

    var request: Data {
        try! EvmSignRequestEncoder.wrap(kind: 0, payload: unsignedTransaction, requestID: requestID)
    }

    func signedTransaction(signature: Data) throws -> Data {
        guard signature.count == 65, signature[64] <= 1 else { throw EvmTransactionError.invalidSignature }
        let parity = signature[64] == 0 ? Data() : Data([1])
        let r = RLP.trimInteger(Data(signature[0..<32]))
        let s = RLP.trimInteger(Data(signature[32..<64]))
        return Data([2]) + RLP.listEncodedFields(unsignedFields + [RLP.bytes(parity), RLP.bytes(r), RLP.bytes(s)])
    }
}

enum EvmTransactionEncoder {
    static func prepare(chainID: String, nonce: String, recipient: String, amount: String,
                        gasLimit: String, maxPriorityFeeGwei: String, maxFeeGwei: String,
                        dataHex: String, requestID: UInt32 = UInt32.random(in: 1...UInt32.max)) throws -> PreparedEvmTransaction {
        guard let chain = UInt64(chainID), chain > 0 else { throw EvmTransactionError.invalidNumber("chain ID") }
        guard let nonceValue = UInt64(nonce) else { throw EvmTransactionError.invalidNumber("nonce") }
        guard let gas = UInt64(gasLimit), gas > 0 else { throw EvmTransactionError.invalidNumber("gas limit") }
        let priority = try DecimalUnits.encode(maxPriorityFeeGwei, decimals: 9, field: "priority fee")
        let maximum = try DecimalUnits.encode(maxFeeGwei, decimals: 9, field: "maximum fee")
        let to = try Hex.decode(recipient, exactBytes: 20, error: .invalidAddress)
        let payload = try Hex.decode(dataHex, exactBytes: nil, error: .invalidData)
        let value = try DecimalUnits.encode(amount, decimals: 18, field: "amount")
        let rawFields = [
            RLP.integer(chain), RLP.integer(nonceValue), priority,
            maximum, RLP.integer(gas), to, value, payload,
        ]
        let encodedFields = rawFields.map(RLP.bytes) + [RLP.encodedEmptyList]
        let unsigned = Data([2]) + RLP.listEncodedFields(encodedFields)
        guard unsigned.count <= EvmSignRequestEncoder.maxPayload else { throw EvmTransactionError.transactionTooLarge }
        return PreparedEvmTransaction(requestID: requestID, unsignedTransaction: unsigned, unsignedFields: encodedFields)
    }

    static func prepareHex(chainID: String, nonce: String, recipient: String, value: String,
                           gasLimit: String, maxPriorityFee: String, maxFee: String,
                           dataHex: String, accessList: Any? = nil,
                           requestID: UInt32 = UInt32.random(in: 1...UInt32.max)) throws
        -> PreparedEvmTransaction {
        guard let chain = Hex.quantityUInt64(chainID), chain > 0 else {
            throw EvmTransactionError.invalidNumber("chain ID")
        }
        guard let nonceValue = Hex.quantityUInt64(nonce) else { throw EvmTransactionError.invalidNumber("nonce") }
        guard let gas = Hex.quantityUInt64(gasLimit), gas > 0 else {
            throw EvmTransactionError.invalidNumber("gas limit")
        }
        let priority = try Hex.quantity(maxPriorityFee, field: "priority fee")
        let maximum = try Hex.quantity(maxFee, field: "maximum fee")
        let amount = try Hex.quantity(value, field: "value")
        let to = try Hex.decode(recipient, exactBytes: 20, error: .invalidAddress)
        let payload = try Hex.decode(dataHex, exactBytes: nil, error: .invalidData)
        let encodedAccessList = try encodeAccessList(accessList)
        let rawFields = [RLP.integer(chain), RLP.integer(nonceValue), priority, maximum,
                         RLP.integer(gas), to, amount, payload]
        let encodedFields = rawFields.map(RLP.bytes) + [encodedAccessList]
        let unsigned = Data([2]) + RLP.listEncodedFields(encodedFields)
        guard unsigned.count <= EvmSignRequestEncoder.maxPayload else { throw EvmTransactionError.transactionTooLarge }
        return PreparedEvmTransaction(requestID: requestID, unsignedTransaction: unsigned,
                                      unsignedFields: encodedFields)
    }

    private static func encodeAccessList(_ value: Any?) throws -> Data {
        guard let value, !(value is NSNull) else { return RLP.encodedEmptyList }
        guard let rawEntries = value as? [Any] else { throw EvmTransactionError.invalidAccessList }
        var encodedEntries: [Data] = []
        encodedEntries.reserveCapacity(rawEntries.count)
        for rawEntry in rawEntries {
            guard let entry = rawEntry as? [String: Any], let addressText = entry["address"] as? String,
                  let rawKeys = entry["storageKeys"] as? [Any] else {
                throw EvmTransactionError.invalidAccessList
            }
            let address = try Hex.decode(addressText, exactBytes: 20, error: .invalidAccessList)
            let encodedKeys = try rawKeys.map { rawKey -> Data in
                guard let keyText = rawKey as? String else { throw EvmTransactionError.invalidAccessList }
                return RLP.bytes(try Hex.decode(keyText, exactBytes: 32, error: .invalidAccessList))
            }
            encodedEntries.append(RLP.listEncodedFields([
                RLP.bytes(address), RLP.listEncodedFields(encodedKeys),
            ]))
        }
        return RLP.listEncodedFields(encodedEntries)
    }

    static func rpcTransaction(sender: String, recipient: String, amount: String,
                               dataHex: String) throws -> [String: String] {
        _ = try Hex.decode(sender, exactBytes: 20, error: .invalidAddress)
        _ = try Hex.decode(recipient, exactBytes: 20, error: .invalidAddress)
        _ = try Hex.decode(dataHex, exactBytes: nil, error: .invalidData)
        let value = try DecimalUnits.encode(amount, decimals: 18, field: "amount")
        return [
            "from": sender,
            "to": recipient,
            "value": Hex.quantityString(value),
            "data": dataHex,
        ]
    }
}

private enum DecimalUnits {
    static func encode(_ text: String, decimals: Int, field: String) throws -> Data {
        let pieces = text.split(separator: ".", omittingEmptySubsequences: false)
        guard pieces.count <= 2, !pieces.isEmpty, pieces.contains(where: { !$0.isEmpty }),
              pieces.allSatisfy({ $0.allSatisfy({ $0.isASCII && $0.isNumber }) }) else {
            throw EvmTransactionError.invalidNumber(field)
        }
        let whole = pieces[0].isEmpty ? "0" : String(pieces[0])
        let fraction = pieces.count == 2 ? String(pieces[1]) : ""
        guard fraction.count <= decimals else { throw EvmTransactionError.invalidNumber(field) }
        let combined = (whole + fraction + String(repeating: "0", count: decimals - fraction.count))
            .drop(while: { $0 == "0" })
        if combined.isEmpty { return Data() }
        var bytes = [UInt8](repeating: 0, count: 32)
        for character in combined {
            guard let digit = character.wholeNumberValue else { throw EvmTransactionError.invalidNumber(field) }
            var carry = digit
            for index in stride(from: 31, through: 0, by: -1) {
                let value = Int(bytes[index]) * 10 + carry
                bytes[index] = UInt8(value & 0xff)
                carry = value >> 8
            }
            guard carry == 0 else { throw EvmTransactionError.invalidNumber(field) }
        }
        return RLP.trimInteger(Data(bytes))
    }
}

private enum Hex {
    static func decode(_ input: String, exactBytes: Int?, error: EvmTransactionError) throws -> Data {
        let text = input.hasPrefix("0x") ? String(input.dropFirst(2)) : input
        guard text.count.isMultiple(of: 2), text.allSatisfy({ $0.isHexDigit }) else { throw error }
        var data = Data(capacity: text.count / 2)
        var index = text.startIndex
        while index < text.endIndex {
            let next = text.index(index, offsetBy: 2)
            guard let byte = UInt8(text[index..<next], radix: 16) else { throw error }
            data.append(byte)
            index = next
        }
        if let exactBytes, data.count != exactBytes { throw error }
        return data
    }

    static func quantity(_ input: String, field: String) throws -> Data {
        guard input.hasPrefix("0x") else { throw EvmTransactionError.invalidNumber(field) }
        var text = String(input.dropFirst(2))
        if text.isEmpty { text = "0" }
        guard text.allSatisfy({ $0.isHexDigit }) else { throw EvmTransactionError.invalidNumber(field) }
        text = String(text.drop(while: { $0 == "0" }))
        if text.isEmpty { return Data() }
        if !text.count.isMultiple(of: 2) { text = "0" + text }
        let value = try decode(text, exactBytes: nil, error: .invalidNumber(field))
        guard value.count <= 32 else { throw EvmTransactionError.invalidNumber(field) }
        return value
    }

    static func quantityUInt64(_ input: String) -> UInt64? {
        if input.hasPrefix("0x") {
            let digits = input.dropFirst(2)
            return !digits.isEmpty && digits.allSatisfy({ $0.isHexDigit }) ? UInt64(digits, radix: 16) : nil
        }
        return UInt64(input)
    }

    static func quantityString(_ value: Data) -> String {
        guard !value.isEmpty else { return "0x0" }
        let encoded = value.map { String(format: "%02x", $0) }.joined()
        return "0x" + encoded.drop(while: { $0 == "0" })
    }
}

private enum RLP {
    static let encodedEmptyList = Data([0xc0])

    static func trimInteger(_ data: Data) -> Data {
        Data(data.drop(while: { $0 == 0 }))
    }

    static func integer(_ value: UInt64) -> Data {
        if value == 0 { return Data() }
        var bigEndian = value.bigEndian
        return withUnsafeBytes(of: &bigEndian) { trimInteger(Data($0)) }
    }

    static func bytes(_ payload: Data) -> Data {
        if payload.count == 1, payload[0] < 0x80 { return payload }
        if payload.count <= 55 { return Data([0x80 + UInt8(payload.count)]) + payload }
        let length = integer(UInt64(payload.count))
        return Data([0xb7 + UInt8(length.count)]) + length + payload
    }

    static func listEncodedFields(_ encodedFields: [Data]) -> Data {
        let payload = encodedFields.reduce(into: Data()) { $0.append($1) }
        if payload.count <= 55 { return Data([0xc0 + UInt8(payload.count)]) + payload }
        let length = integer(UInt64(payload.count))
        return Data([0xf7 + UInt8(length.count)]) + length + payload
    }
}
