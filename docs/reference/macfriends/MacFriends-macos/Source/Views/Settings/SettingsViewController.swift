//
//  SettingsViewController.swift
//  MacFriends
//
//  Created by Bart Jakobs on 28/07/2025.
//

import Cocoa
import SwiftUI
class SettingsViewController: NSViewController {
   
    public var onClosed: (() -> Void)?
    
    var settingsView: NSHostingView<SettingsView>!
     
    weak var mainViewController: ViewController?
    weak var serialTask: SerialTask?
    
    var viewModel = SettingsViewModel()

    
    
    
    override func viewDidLoad() {
        super.viewDidLoad()
    
        viewModel.serialTask = serialTask
        
        self.settingsView = NSHostingView(rootView: SettingsView(viewModel: viewModel))
        self.view.addSubview(settingsView)
    
         
        settingsView.translatesAutoresizingMaskIntoConstraints = false
        settingsView.leadingAnchor.constraint(equalTo: view.leadingAnchor).isActive = true
        settingsView.trailingAnchor.constraint(equalTo: view.trailingAnchor).isActive = true
        settingsView.topAnchor.constraint(equalTo: view.topAnchor).isActive = true
        settingsView.bottomAnchor.constraint(equalTo: view.bottomAnchor).isActive = true
        
        
        
    }
    
    override func viewWillAppear() {
        viewModel.setSerialPortCheker()
    }
    
    private func wantsToConnect(){
        print("Wants to connect!")
    }
    
    
    override func viewWillDisappear() {
        print("View doei")
        onClosed?()
    }
    
    
}
