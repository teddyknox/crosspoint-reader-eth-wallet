import Foundation

enum EvmSignRequestError: LocalizedError {
    case malformedTypedData
    case unsupportedTypedData(String)
    case mismatchedChain
    case mismatchedAddress
    case invalidMessage
    case payloadTooLarge

    var errorDescription: String? {
        switch self {
        case .malformedTypedData: "The dapp supplied malformed EIP-712 typed data"
        case .unsupportedTypedData(let type): "The X3 cannot safely display EIP-712 type \(type)"
        case .mismatchedChain: "The EIP-712 domain chain does not match the WalletConnect chain"
        case .mismatchedAddress: "The signing address does not match the X3 wallet"
        case .invalidMessage: "Only valid Sign-In with Ethereum messages are supported"
        case .payloadTooLarge: "The signing request is too large for the X3"
        }
    }
}

enum WalletSigningResult {
    case transaction(Data)
    case signature(Data)
}

enum PreparedWalletRequest {
    case transaction(PreparedEvmTransaction)
    case signature(PreparedEvmSignature)

    var requestID: UInt32 {
        switch self {
        case .transaction(let value): value.requestID
        case .signature(let value): value.requestID
        }
    }

    var request: Data {
        switch self {
        case .transaction(let value): value.request
        case .signature(let value): value.request
        }
    }

    func complete(signature: Data) throws -> WalletSigningResult {
        switch self {
        case .transaction(let value):
            return .transaction(try value.signedTransaction(signature: signature))
        case .signature:
            guard signature.count == 65, signature[64] <= 1 else { throw EvmTransactionError.invalidSignature }
            var rpcSignature = signature
            rpcSignature[64] += 27
            return .signature(rpcSignature)
        }
    }
}

struct PreparedEvmSignature {
    let requestID: UInt32
    let request: Data
}

enum EvmSignRequestEncoder {
    static let wireSize = 2_068
    static let maxPayload = 2_048

    static func wrap(kind: UInt8, payload: Data, requestID: UInt32) throws -> Data {
        guard !payload.isEmpty, payload.count <= maxPayload else { throw EvmSignRequestError.payloadTooLarge }
        var writer = EvmSignWireWriter()
        writer.appendBytes([0x58, 0x33, 0x45, 0x53]) // X3ES
        writer.appendUInt8(2)
        writer.appendUInt8(kind)
        writer.appendUInt16(UInt16(wireSize))
        writer.appendUInt32(requestID)
        writer.appendUInt16(UInt16(payload.count))
        writer.appendUInt16(0)
        writer.data.append(payload)
        writer.appendZeroes(maxPayload - payload.count)
        writer.appendUInt32(CRC32.checksum(writer.data))
        precondition(writer.data.count == wireSize)
        return writer.data
    }

    static func typedData(_ value: Any, chainID: String, address: String, origin: String,
                          requestID: UInt32 = UInt32.random(in: 1...UInt32.max)) throws -> PreparedEvmSignature {
        guard address.hasPrefix("0x"), address.count == 42 else { throw EvmSignRequestError.mismatchedAddress }
        let root = try typedDataDictionary(value)
        guard let types = root["types"] as? [String: Any],
              let domain = root["domain"] as? [String: Any],
              let message = root["message"] as? [String: Any],
              let primaryType = root["primaryType"] as? String,
              isIdentifier(primaryType), primaryType != "EIP712Domain",
              let domainDefinitions = definitions(types["EIP712Domain"]),
              let messageDefinitions = definitions(types[primaryType]) else {
            throw EvmSignRequestError.malformedTypedData
        }
        let usedTypes = Set(["EIP712Domain", primaryType])
        guard Set(types.keys).isSubset(of: usedTypes) else {
            throw EvmSignRequestError.unsupportedTypedData("nested structs")
        }
        guard let sessionChain = UInt64(chainID), sessionChain > 0 else {
            throw EvmSignRequestError.mismatchedChain
        }

        var payload = EvmSignWireWriter()
        try payload.appendSized(origin, max: 192, lengthBytes: 2)
        payload.appendUInt64(sessionChain)
        try payload.appendSized(primaryType, max: 32, lengthBytes: 1)
        try appendFields(domainDefinitions, values: domain, writer: &payload, domain: true)
        try appendFields(messageDefinitions, values: message, writer: &payload, domain: false)

        guard let chainDefinition = domainDefinitions.first(where: { $0.name == "chainId" }),
              let chainValue = domain[chainDefinition.name],
              try encode(value: chainValue, type: chainDefinition.type) == uintBytes(String(sessionChain)) else {
            throw EvmSignRequestError.mismatchedChain
        }
        let wire = try wrap(kind: 1, payload: payload.data, requestID: requestID)
        return PreparedEvmSignature(requestID: requestID, request: wire)
    }

    static func siwe(messageValue: Any, chainID: String, address: String, origin: String,
                     requestID: UInt32 = UInt32.random(in: 1...UInt32.max)) throws -> PreparedEvmSignature {
        try personalMessage(
            messageValue: messageValue, chainID: chainID, address: address, origin: origin, requestID: requestID
        )
    }

    static func personalMessage(messageValue: Any, chainID: String, address: String, origin: String,
                                requestID: UInt32 = UInt32.random(in: 1...UInt32.max)) throws
        -> PreparedEvmSignature {
        let message: Data
        if let text = messageValue as? String {
            if text.hasPrefix("0x") {
                message = try decodeHex(text)
            } else {
                message = Data(text.utf8)
            }
        } else {
            throw EvmSignRequestError.invalidMessage
        }
        guard !message.isEmpty else { throw EvmSignRequestError.invalidMessage }
        guard address.hasPrefix("0x"), address.count == 42 else { throw EvmSignRequestError.mismatchedAddress }
        guard let sessionChain = UInt64(chainID), sessionChain > 0 else { throw EvmSignRequestError.mismatchedChain }
        var payload = EvmSignWireWriter()
        try payload.appendSized(origin, max: 192, lengthBytes: 2)
        payload.appendUInt64(sessionChain)
        guard message.count <= UInt16.max else { throw EvmSignRequestError.payloadTooLarge }
        payload.appendUInt16(UInt16(message.count))
        payload.data.append(message)
        let wire = try wrap(kind: 2, payload: payload.data, requestID: requestID)
        return PreparedEvmSignature(requestID: requestID, request: wire)
    }

    private struct FieldDefinition {
        let name: String
        let type: String
    }

    private static func typedDataDictionary(_ value: Any) throws -> [String: Any] {
        if let dictionary = value as? [String: Any] { return dictionary }
        if let text = value as? String, let data = text.data(using: .utf8),
           let dictionary = try JSONSerialization.jsonObject(with: data) as? [String: Any] {
            return dictionary
        }
        throw EvmSignRequestError.malformedTypedData
    }

    private static func definitions(_ value: Any?) -> [FieldDefinition]? {
        guard let array = value as? [[String: Any]], !array.isEmpty else { return nil }
        var names = Set<String>()
        var result: [FieldDefinition] = []
        for item in array {
            guard let name = item["name"] as? String, let type = item["type"] as? String,
                  isIdentifier(name), names.insert(name).inserted else { return nil }
            result.append(FieldDefinition(name: name, type: type))
        }
        return result
    }

    private static func appendFields(_ definitions: [FieldDefinition], values: [String: Any],
                                     writer: inout EvmSignWireWriter, domain: Bool) throws {
        let maximum = domain ? 5 : 12
        guard !definitions.isEmpty, definitions.count <= maximum,
              Set(values.keys) == Set(definitions.map(\.name)) else {
            throw EvmSignRequestError.malformedTypedData
        }
        writer.appendUInt8(UInt8(definitions.count))
        let domainNames = Set(["name", "version", "chainId", "verifyingContract", "salt"])
        for definition in definitions {
            guard !domain || domainNames.contains(definition.name), let value = values[definition.name] else {
                throw EvmSignRequestError.malformedTypedData
            }
            let encoded = try encode(value: value, type: definition.type)
            try writer.appendSized(definition.type, max: 32, lengthBytes: 1)
            try writer.appendSized(definition.name, max: 32, lengthBytes: 1)
            guard encoded.count <= 512 else { throw EvmSignRequestError.payloadTooLarge }
            writer.appendUInt16(UInt16(encoded.count))
            writer.data.append(encoded)
        }
    }

    private static func encode(value: Any, type: String) throws -> Data {
        if type == "address" {
            guard let text = value as? String else { throw EvmSignRequestError.malformedTypedData }
            let data = try decodeHex(text)
            guard data.count == 20 else { throw EvmSignRequestError.malformedTypedData }
            return data
        }
        if type == "bool" {
            guard let boolean = value as? Bool else { throw EvmSignRequestError.malformedTypedData }
            return Data([boolean ? 1 : 0])
        }
        if type == "string" {
            guard let text = value as? String else { throw EvmSignRequestError.malformedTypedData }
            return Data(text.utf8)
        }
        if type == "bytes" {
            guard let text = value as? String else { throw EvmSignRequestError.malformedTypedData }
            return try decodeHex(text)
        }
        if type.hasPrefix("bytes"), let width = Int(type.dropFirst(5)), (1...32).contains(width) {
            guard let text = value as? String else { throw EvmSignRequestError.malformedTypedData }
            let data = try decodeHex(text)
            guard data.count == width else { throw EvmSignRequestError.malformedTypedData }
            return data
        }
        if type == "uint" || type.hasPrefix("uint") {
            let widthText = String(type.dropFirst(4))
            let width = widthText.isEmpty ? 256 : Int(widthText)
            guard let width, width >= 8, width <= 256, width.isMultiple(of: 8) else {
                throw EvmSignRequestError.unsupportedTypedData(type)
            }
            let text: String
            if let string = value as? String {
                text = string
            } else if let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
                      !String(cString: number.objCType).contains(where: { "fd".contains($0) }) {
                text = number.stringValue
            } else {
                throw EvmSignRequestError.malformedTypedData
            }
            let bytes = try uintBytes(text)
            guard bytes.count <= width / 8 else { throw EvmSignRequestError.malformedTypedData }
            return bytes
        }
        throw EvmSignRequestError.unsupportedTypedData(type)
    }

    private static func uintBytes(_ text: String) throws -> Data {
        if text.hasPrefix("0x") { return try trimInteger(decodeHex(text)) }
        guard !text.isEmpty, text.allSatisfy({ $0.isASCII && $0.isNumber }) else {
            throw EvmSignRequestError.malformedTypedData
        }
        var bytes = [UInt8](repeating: 0, count: 32)
        for character in text {
            guard let digit = character.wholeNumberValue else { throw EvmSignRequestError.malformedTypedData }
            var carry = digit
            for index in stride(from: 31, through: 0, by: -1) {
                let next = Int(bytes[index]) * 10 + carry
                bytes[index] = UInt8(next & 0xff)
                carry = next >> 8
            }
            guard carry == 0 else { throw EvmSignRequestError.malformedTypedData }
        }
        return trimInteger(Data(bytes))
    }

    private static func trimInteger(_ data: Data) -> Data { Data(data.drop(while: { $0 == 0 })) }

    private static func decodeHex(_ text: String) throws -> Data {
        guard text.hasPrefix("0x") else { throw EvmSignRequestError.malformedTypedData }
        let digits = text.dropFirst(2)
        guard digits.count.isMultiple(of: 2), digits.allSatisfy(\.isHexDigit) else {
            throw EvmSignRequestError.malformedTypedData
        }
        var result = Data(capacity: digits.count / 2)
        var index = digits.startIndex
        while index < digits.endIndex {
            let end = digits.index(index, offsetBy: 2)
            guard let byte = UInt8(digits[index..<end], radix: 16) else {
                throw EvmSignRequestError.malformedTypedData
            }
            result.append(byte)
            index = end
        }
        return result
    }

    private static func isIdentifier(_ value: String) -> Bool {
        guard let first = value.first, first.isASCII, first.isLetter || first == "_" else { return false }
        return value.dropFirst().allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_") }
    }
}

struct EvmSignWireWriter {
    var data = Data()

    mutating func appendUInt8(_ value: UInt8) { data.append(value) }
    mutating func appendBytes(_ bytes: [UInt8]) { data.append(contentsOf: bytes) }
    mutating func appendZeroes(_ count: Int) { data.append(contentsOf: repeatElement(0, count: count)) }

    mutating func appendUInt16(_ value: UInt16) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
    }

    mutating func appendUInt32(_ value: UInt32) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }

    mutating func appendUInt64(_ value: UInt64) {
        for shift in stride(from: 0, through: 56, by: 8) {
            data.append(UInt8(truncatingIfNeeded: value >> UInt64(shift)))
        }
    }

    mutating func appendSized(_ value: String, max: Int, lengthBytes: Int) throws {
        let bytes = Data(value.utf8)
        guard !bytes.isEmpty, bytes.count <= max else { throw EvmSignRequestError.payloadTooLarge }
        if lengthBytes == 1 {
            appendUInt8(UInt8(bytes.count))
        } else {
            appendUInt16(UInt16(bytes.count))
        }
        data.append(bytes)
    }
}
