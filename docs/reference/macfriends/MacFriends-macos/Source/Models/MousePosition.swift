//
//  MousePosition.swift
//  MacFriends
//
//  Created by Bart Jakobs on 11/05/2025.

//

import Foundation
import CoreGraphics

struct MousePosition {
    var delta: CGSize
    var timestamp: TimeInterval
    var deltaCompensatedForTime: CGSize?

    var position: CGPoint
}

//  This is a bit of a mess, sorry.
struct MousePositionHistory {
    
    var position: CGPoint = .zero
    var positions: [MousePosition] = []
    
    func averageSpeed(timeSinceLast: Double) -> CGFloat {
        guard let last = positions.last else {
            return .zero
        }
        return positions.filter { position in
            position.timestamp >= last.timestamp - timeSinceLast
        }.reduce(0.0) { partialResult, position in
            if let delta = position.deltaCompensatedForTime {
                return partialResult + delta.size
            }
            return partialResult
        }
    }
    
    func averageSpeed(inLast: Int = 10) -> CGFloat {
        let lastN = positions.suffix(inLast)
        let distance = lastN.reduce(0.0) { partialResult, position in
            if let delta = position.deltaCompensatedForTime {
                return partialResult + delta.size
            }
            return partialResult
        }
        return distance
    }
    
    private func interpolatedPosition(atTime time: Double) -> CGPoint {
        guard let first = positions.first, let last = positions.last else {
            return .zero
        }
        
        guard time >= first.timestamp else { 
            return CGPoint(x: first.position.x, y: first.position.y)
        }
        guard time <= last.timestamp else { 
            return CGPoint(x: last.position.x, y: last.position.y)
        }
        
        let under = positions.first { $0.timestamp > time }
        let above = positions.last { $0.timestamp < time }
        guard let underPosition = under, let abovePosition = above else {
            return .zero
        }
        let ratio = (time - underPosition.timestamp) / (abovePosition.timestamp - underPosition.timestamp)
        let x = underPosition.position.x + ratio * (abovePosition.position.x - underPosition.position.x)
        let y = underPosition.position.y + ratio * (abovePosition.position.y - underPosition.position.y)
        return CGPoint(x: x, y: y) 
    }

    func averageVelocity(timeSinceLast t: Double) -> CGSize {
        let steps = 0.1
        let now = ProcessInfo.processInfo.systemUptime
        var velocity = CGSize.zero
        var previousPoint: CGPoint? = nil
        for i in stride(from: now - t, to: now, by: steps) {
            let position = interpolatedPosition(atTime: i)
            if let previous = previousPoint {
                let dx = position.x - previous.x
                let dy = position.y - previous.y
                velocity.width += dx / CGFloat(steps)
                velocity.height += dy / CGFloat(steps)
            }
            previousPoint = position
        }
        return velocity
    }


    
    func accumulatedDisplacement(timeSinceLast: Double) -> CGSize {
        guard let last = positions.last else {
            return .zero
        }
        return positions.filter { position in
            position.timestamp >= last.timestamp - timeSinceLast
        }.reduce(CGSize.zero) { partialResult, position in
            return partialResult + position.delta
        }
    }
    
    func accumulatedDisplacement(inLast: Int = 10) -> CGSize {
        let lastN = positions.suffix(inLast)
        return lastN.reduce(CGSize.zero) { partialResult, position in
            return partialResult + position.delta
        }
    }
    
   
    
    
    
    @discardableResult
    mutating func append(deltaX: CGFloat, deltaY: CGFloat, time: TimeInterval, positionInWindow: NSPoint) -> Bool {
        if self.position == .zero {
            self.position = CGPoint(x: positionInWindow.x, y: positionInWindow.y)
        } else {
            self.position.x += deltaX
            self.position.y += deltaY
        }

        return self.append(MousePosition(delta: CGSize(width: deltaX, height: deltaY), timestamp: time, position: self.position))
    }
    
    @discardableResult
    mutating func append(_ position: MousePosition) -> Bool{
        var position = position
        if let last = positions.last {
            let previousTimestamp = last.timestamp
            let deltaTime = position.timestamp - previousTimestamp
            position.deltaCompensatedForTime = position.delta * deltaTime
        }
        
        positions.append(position)
        let movement = accumulatedDisplacement(timeSinceLast: 1.0)
        let mouseX = averageVelocity(timeSinceLast: 0.5)
        let rightComponent = mouseX.componentInDirection(.right)
        
        
        if(movement.width > 1000 && rightComponent > 5000) {
            return true
        }
        return false
        
    }
    
    
    mutating func clear(){
        self.positions.removeAll()
        self.position = .zero
    }
    
    

}
