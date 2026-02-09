//
//  SerialTaskDelegate.swift
//  MacFriends
//
//  Created by Bart Jakobs on 30/07/2025.
//

import Foundation


protocol SerialTaskDelegate: AnyObject {
    func serialDidConnect()
    func serialDidDisconnect()
    func serialDidEncounterError(_ error: SerialError)
}
