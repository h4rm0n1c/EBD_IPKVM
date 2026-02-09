//
//  SettingsViewModel.swift
//  MacFriends
//
//  Created by Bart Jakobs on 28/07/2025.
//

import SwiftUI

@Observable 
class SettingsViewModel {
    public weak var serialTask: SerialTask?
    
    var timer: Timer?
    
    
    var serialPort: String {
        get {
            access(keyPath: \.serialPort)
            return UserDefaults.standard.string(forKey: "serialPort") ?? ""
        }
        set {
            withMutation(keyPath: \.serialPort) {
                UserDefaults.standard.setValue(newValue, forKey: "serialPort")
            }
            
            serialTask?.portPath = newValue
        }
    }
    
    var isConnected: Bool = false
    public var currentPortFound: Bool = false
    public var allSerialPorts: [String] = []
    
    init(){
        self.updateSerials()
    }
    
    func setSerialPortCheker(){
        
        self.timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true, block: { timer in
            self.updateSerials()
        })
    
    }
   
    
    private func updateSerials() {
        let usbSerials = try? FileManager.default.contentsOfDirectory(atPath: "/dev").filter { $0.hasPrefix("cu.usb") }
        let allSerials = usbSerials?.map { "/dev/\($0)" } ?? []
        if allSerials != self.allSerialPorts {
            self.allSerialPorts = allSerials
            currentPortFound = allSerialPorts.contains(serialPort)
        }
        
    }
    
    public func connect(){
        Task{
            try await serialTask?.connect()
        }
    }
    
    public func disconnect() {
        Task {
            await serialTask?.disconnect()
        }
    }
    

}
