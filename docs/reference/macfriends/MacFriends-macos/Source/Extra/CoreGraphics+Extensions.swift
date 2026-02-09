//
//  CoreGraphics+Extensions.swift
//  MacFriends
//
//  Created by Bart Jakobs on 11/05/2025.
//

import CoreGraphics

extension CGRect {
    var center: CGPoint {
        return CGPoint(x: midX, y: midY)
    }
    
    init(center: CGPoint, size: CGSize) {
        let distanceToCenterX = size.size / 2
        let distanceToCenterY = size.size / 2
        self.init(origin: CGPoint(x: center.x - distanceToCenterX, y: center.y - distanceToCenterY), size: size)
    }
    
}
enum Direction {
    case left
    case right
    case top
    case bottom
    
    init(fromSize size: CGSize) {
        self = Self(fromAngle: size.angle)
    }
    
    init(fromAngle angle: CGFloat) {
        var angle = Int(((angle  + CGFloat.pi * 2.5).truncatingRemainder(dividingBy: CGFloat.pi * 2) / (CGFloat.pi / 2)).rounded(.toNearestOrEven))
        if angle == 1{
            self = .right
        }
        else if angle == 2 {
            self = .bottom
        }
        else if angle == 3 {
            self = .left
        }
        else {
            self = .top
        }
        
    }
}

extension CGSize {
    var size: Double {
        return sqrt(width * width + height * height)
    }
    
    static func * (lhs: CGSize, rhs: CGFloat) -> CGSize {
        return CGSize(width: lhs.width * rhs, height: lhs.height * rhs)
    }
    
    static func + (lhs: CGSize, rhs: CGSize) -> CGSize {
        return CGSize(width: lhs.width + rhs.width, height: lhs.height + rhs.height)
    }
    
    static func - (lhs: CGSize, rhs: CGSize) -> CGSize {
        return CGSize(width: lhs.width - rhs.width, height: lhs.height - rhs.height)
    }
    
    var angle: CGFloat {
        return atan2(height, width) //
    }
    
    func clamped(between min: CGFloat, max: CGFloat) -> CGSize {
        let width = width.clamped(between: min, max: max)
        let height = height.clamped(between: min, max: max)
        return CGSize(width: width, height: height)
    }

    func componentInDirection(_ direction: Direction) -> CGFloat {
        switch direction {
        case .left: 
            return -width
        case .right:
            return width
        case .top:
            return height
        case .bottom:
            return -height
        }
    }
}
extension CGFloat {
    func clamped(between min: CGFloat, max: CGFloat) -> CGFloat {
        return Swift.min(Swift.max(self, min), max)
    }
}

extension CGPoint {
    static func - (lhs: CGPoint, rhs: CGPoint) -> CGSize {
        return CGSize(width: lhs.x - rhs.x, height: lhs.y - rhs.y)
    }
}
