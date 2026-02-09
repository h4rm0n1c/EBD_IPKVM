//
//  Keyboard.swift
//  MacFriends
//
//  Created by Bart Jakobs on 12/05/2025.
//
import Foundation
import Cocoa

struct KeyboardMessage  {
    let scanCode: UInt
    let isUp: UInt
    let modifierKeys: UInt
    
    
    static func modifierKeys(forFlags flags: NSEvent.ModifierFlags) -> UInt{
        var keys = UInt()
//        #define kModCmd 1
//        #define kModOpt 2
//        #define kModShift 4
//        #define kModControl 8
//        #define kModReset 16
//        #define kModCaps 32
//        #define kModDelete 64
        if flags.contains(.command) {
            keys |= 1
        }
        
        if flags.contains(.option) {
            keys |= 2
        }
        if flags.contains(.shift) {
            keys |= 4
        }
        if flags.contains(.control) {
            keys |= 8
        }
        if flags.contains(.capsLock){
            keys |= 32
        }
        print("modkeys \(keys)")
        return keys
    }
    
    init(scanCode: UInt, isUp: UInt, modifierKeys: UInt) {
        if scanCode == 0x7E {
            self.scanCode = 62
        } else if scanCode == 0x7D {
            self.scanCode = 61
        } else if scanCode == 0x7B {
            self.scanCode = 59
        }
        else if scanCode == 0x7C {
            self.scanCode = 60
        } else {
            self.scanCode = scanCode
        }
        
        
        self.isUp = isUp
        self.modifierKeys = modifierKeys
    }
    
    static func ForModifierFlagChange(from oldFlags: UInt, to newFlags: UInt) -> [KeyboardMessage] {
        if oldFlags == newFlags {
            return []
        }
        let bitsAndKeyCodes: [(bit: UInt, keyCode: UInt)] = [
            (1, 0x37), // Command key
            (2, 0x3A), // Option key
            (4, 0x38), // Shift key
            (8, 0x3B), // Control key
            // (16, 0x3C), // Reset key
            (32, 0x39), // Caps lock key
            (64, 0x33) // Delete key
        ]
        var messages: [KeyboardMessage] = []
        var changedFlags = oldFlags
        for (bit, keycode) in bitsAndKeyCodes {
            changedFlags |= bit
            if oldFlags & bit != newFlags & bit { // change of a key
//                print("Change! \(String(format: "%02X", keycode)) - \((newFlags & bit) == 0  ? 1 : 0)")
                messages.append(KeyboardMessage(scanCode: keycode, isUp: (newFlags & bit) == 0  ? 1 : 0, modifierKeys: changedFlags))
            }
        }
        return messages
    }
}
