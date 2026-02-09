//
//  ViewController+WindowDelegate.swift
//  MacFriends
//
//  Created by Bart Jakobs on 30/07/2025.
//
import Cocoa

extension ViewController: SerialTaskDelegate {
    func serialDidConnect() {
        childViewController.viewModel.isConnected = true
    }
    
    func serialDidDisconnect() {
        childViewController.viewModel.isConnected = false
    }
    
    func serialDidEncounterError(_ error: SerialError) {
        // probably should handle this somehow
        
    }
    
   
}
