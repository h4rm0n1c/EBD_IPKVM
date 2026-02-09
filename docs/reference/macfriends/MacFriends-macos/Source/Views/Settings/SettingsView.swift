//
//  SettingsView.swift
//  MacFriends
//
//  Created by Bart Jakobs on 28/07/2025.
//

import SwiftUI


public struct SettingsView: View {
    @Bindable var viewModel: SettingsViewModel
    
    init(viewModel: SettingsViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        
        Form {
            Section("Setting") {
                Picker("Serial port", selection: $viewModel.serialPort) {
                    ForEach(viewModel.allSerialPorts, id: \.self) { port in
                        Text(port)
                    }
                }.disabled(viewModel.isConnected)
            }
            HStack{
                
                    Button {
                        exit(0)
                    } label: {
                        Text("Quit")
                    }
                
                
                if viewModel.isConnected {
                    Button {
                        viewModel.disconnect()
                    } label: {
                        Text("Disconnect")
                    }
                } else {
                    Button {
                        viewModel.connect()
                    } label: {
                        Text("Connect")
                    }
                }

            }
//            .picker
        }
        .padding()
        .frame(width: 500, height: 150)
        
    }
}

