//
//  ViewController.swift
//  MacFriends
//
//  Created by Bart Jakobs on 10/05/2025.
//

import Cocoa
import CoreGraphics
import AppKit

let height = 500.0
class ViewController: NSViewController {
    
    @IBOutlet weak var lockedMouseView: LockedMouseView!
    
    var childViewController = SettingsViewController()
    
    var serialTask = SerialTask()
    override func viewDidLoad() {
        super.viewDidLoad()
        
    }
    
    var timer: Timer!
    var visualEffect: NSVisualEffectView!
    
    override func viewWillAppear() {
        serialTask.delegate = self
        let frame = NSScreen.main?.frame ?? NSRect(x: 0, y: 0, width: 2100, height: 1200)
        
        
        let rect = NSRect(x: -10.0, y:  height / 2, width: 100, height: frame.height - height)

        view.window?.setFrame(rect, display: true)
        view.shadow?.shadowColor = .black
        view.shadow?.shadowBlurRadius = 10
        
        super.viewWillAppear()
    }
    
    
    func openSettings(){
        lockedMouseView.enabled = false
        childViewController.mainViewController = self
        childViewController.serialTask = serialTask
        
        childViewController.onClosed = {
            if self.serialTask.isConnected {
                self.lockedMouseView.enabled = true
            } else {
                self.lockedMouseView.enabled = false
            }
        }
        
                
        let frame = NSScreen.main?.frame ?? NSRect(x: 0, y: 0, width: 2100, height: 1200)
        present(childViewController, asPopoverRelativeTo: NSRect(x: 0, y: 0, width: 40, height: frame.height - height - 15), of: view, preferredEdge: .maxX, behavior: .transient)
    }
    
    

    override func viewDidAppear() {
        view.window?.level = .floating
        view.window?.backgroundColor = .clear
  
        
        lockedMouseView.delegate = self
        Task {
            
            print("Task!")
            do {
                print("Connecting...?")
                try await serialTask.connect()
                childViewController.viewModel.isConnected = true
                print("Connected!")
            } catch {
                print("error \(error)")
                childViewController.viewModel.isConnected = false
                openSettings()
                
            }
            
        }
        
        
        
        
        guard let window = self.view.window else {
            print("Geen window...")
            return;
        }
        window.delegate = self

    }
    
    
}
