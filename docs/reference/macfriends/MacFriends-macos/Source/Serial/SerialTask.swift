//
//  SerialTask.swift
//  MacFriends
//
//  Created by Bart Jakobs on 11/05/2025.
//
import SwiftSerial
import Foundation
import BitByteData


class SerialTask{
    
    var portPath: String = ""
    var port: SerialPort? = nil
    var receiveTask: Task<Void, Never>? = nil
    var mouseBuffer = false
    var delegate: SerialTaskDelegate?
    var keyboardInputBuffer: [KeyboardMessage] = []
    
    public var isConnected: Bool = false
    var mouseIsDown = false {
        didSet {
            if mouseIsDown == false && mouseDownSent == false { // click, release.
                mouseBuffer = true
            }
        }
    }
    var mouseDownSent = false

    private var accumulatedDelta: CGSize = .zero // if the mouse movement is too big for a single adb packet.
    
    init(){
        // Create the port
        
        portPath = UserDefaults.standard.string(forKey: "serialPort") ?? ""
    }
    
    public func addKeyboardEvent(_ event: KeyboardMessage) {
        keyboardInputBuffer.append(event)
    }
    
    public func sendMouseDelta(_ delta: CGSize) {
        accumulatedDelta = accumulatedDelta + delta

    }
    
    func connect() async throws {
        self.port = SerialPort(path: self.portPath)
        // Configure and open
        do {
            try await self.port!.open(
                receiveRate: .baud115200,
                transmitRate: .baud115200)
        }
        catch {
            print("Error connecting... \(error)")
            throw SerialError.failedToOpen
        }
    
        self.receiveTask = Task(operation: {
            await self.writeReadLoop()
        })
        self.isConnected = true
        delegate?.serialDidConnect()
        
    }
    
    
    func writeReadLoop() async {
        while true {
            guard port != nil else {
                await self.disconnect()
                return
            }
            do {
                if keyboardInputBuffer.count > 0 || accumulatedDelta != .zero || mouseIsDown != mouseDownSent || mouseBuffer {
                    mouseDownSent = mouseIsDown
                    // force a mouse down if a click is done between sending frames.
                    
                    try await writeNextPacket(forceMouseDown: mouseBuffer)
                    if mouseBuffer {
                        mouseBuffer = false
                    }
                }
                
                _ = try await port?.readByte() // try to read, to flush buffer and to check if still connected.

            } catch {
                print("Writing / receiving error: \(error)")
                await self.disconnect()
                delegate?.serialDidEncounterError(.errorInReading)
            }
            try? await Task.sleep(nanoseconds: 1000000000 / 50)
        }
        
    }
    
    func disconnect() async{
        self.receiveTask?.cancel()
        guard self.isConnected else {
            return
        }
        
        await self.port?.close()
        self.port = nil
        self.isConnected = false
        delegate?.serialDidDisconnect()
    }
    
    
    
    private func writeNextPacket(forceMouseDown: Bool = false) async throws {
        if port == nil {
            return
        }
        
        
        
        let delta = accumulatedDelta.clamped(between: -63, max: 63)
        accumulatedDelta = accumulatedDelta - delta
        let key = keyboardInputBuffer.first
            
        let packet = ADBInstruction(mouseIsDown: (mouseIsDown || forceMouseDown),
                                    mouseDelta: delta,
                                    updateType: keyboardInputBuffer.isEmpty ? .mouse : .mouseAndKeyboard,
                                    keyboardMessage: key
        ).toData()
    
        do {
            let written = try await self.port?.writeBytes(packet)
            guard written == packet.count else {
                await self.disconnect()
                throw SerialError.writingError
            }
        }
        catch {
            await self.disconnect()
            throw SerialError.writingError
        }

    }
    
 
    
    deinit{
        
        
    }
}
