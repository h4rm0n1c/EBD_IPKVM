//
//  SerialPacket.swift
//  MacFriends
//
//  Created by Bart Jakobs on 30/07/2025.
//
import Foundation
import BitByteData

let MAGIC_NUMBER: UInt8 = 123 // not too magic.

struct ADBInstruction {
    
    public enum ADBInstructionUpdateType: UInt8 {
        case mouse = 1
        case keyboard = 2
        case mouseAndKeyboard = 3
    }
    
    public var mouseIsDown: Bool
    public var mouseDelta: CGSize
    public var updateType: ADBInstructionUpdateType
    public var keyboardMessage: KeyboardMessage? = nil
    
    
    public func toData() -> Data {
        let writer = MsbBitWriter()
        writer.append(byte: MAGIC_NUMBER)
        writer.append(byte: updateType.rawValue) // mouse updateType
        writer.append(byte: mouseIsDown ? 1 : 0)
        writer.write(signedNumber: Int(mouseDelta.width), bitsCount: 8)
        writer.write(signedNumber: Int(mouseDelta.height), bitsCount: 8)
        
        if let key = keyboardMessage {
            writer.write(unsignedNumber: key.scanCode, bitsCount: 8)
            writer.write(unsignedNumber: key.isUp, bitsCount: 8)
            writer.write(unsignedNumber: key.modifierKeys, bitsCount: 8)
        } else {
            writer.write(unsignedNumber: 0, bitsCount: 8)
            writer.write(unsignedNumber: 0, bitsCount: 8)
            writer.write(unsignedNumber: 0, bitsCount: 8)
        }
        return writer.data
        
    }
}

