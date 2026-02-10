//
//  LockedMouseViewDelegate.swift
//  MacFriends
//
//  Created by Bart Jakobs on 30/07/2025.
//

import Foundation

protocol LockedMouseViewDelegate: AnyObject {
    func lockedMouseViewMouseDidMove(_ size: CGSize)
    func lockedMouseViewMouseDown()
    func lockedMouseViewMouseUp()
    
    func lockedMouseViewRightClick()
    
    
    func lockedMouseViewKeyDown(_ scanCode: UInt, modifierFlags: UInt)
    func lockedMouseViewKeyUp(_ scanCode: UInt, modifierFlags: UInt)
    
}
