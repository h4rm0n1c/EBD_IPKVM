//
//  ViewController.swift
//  MacFriends
//
//  Created by Bart Jakobs on 10/05/2025.
//

import Cocoa
import CoreGraphics
import AppKit

extension ViewController: LockedMouseViewDelegate {
    func lockedMouseViewRightClick() {
        openSettings()
    }
    
    func lockedMouseViewKeyDown(_ scanCode: UInt, modifierFlags: UInt) {
        self.serialTask.addKeyboardEvent(KeyboardMessage(scanCode: scanCode, isUp: 0, modifierKeys: modifierFlags))
    }
    
    func lockedMouseViewKeyUp(_ scanCode: UInt, modifierFlags: UInt) {
       
        self.serialTask.addKeyboardEvent(KeyboardMessage(scanCode: scanCode, isUp: 1, modifierKeys: modifierFlags))
    }
    
    func lockedMouseViewMouseDown() {
        if !self.serialTask.isConnected {
            openSettings()
            return
        }
        
        self.serialTask.mouseIsDown = true
        
        
    }
    
    func lockedMouseViewMouseUp() {
        self.serialTask.mouseIsDown = false
    }
    
    func lockedMouseViewMouseDidMove(_ size: CGSize) {
        self.serialTask.sendMouseDelta(size)
    }

}
