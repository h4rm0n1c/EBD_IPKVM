//
//  mouseView.swift
//  MacFriends
//
//  Created by Bart Jakobs on 11/05/2025.
//

import Cocoa
import CoreGraphics
import AppKit


let DistanceToLeftEdgeToLock: CGFloat = 15
let DistanceToLeftEdgeToAnimate: CGFloat = 50


let DefaultPathSize: CGFloat = 32
let MaxMouseDisplacement = 32.0
let MouseDisplacementBorderMovementThreshold: CGFloat = 24.0
let MouseCircleSize = 42.0


class LockedMouseView: NSVisualEffectView {
    var clampedPosition: CGPoint = .zero
    
    weak var delegate: LockedMouseViewDelegate?
    var positions = MousePositionHistory()
    private var isLocked = false
    private var lastModifierKeys: UInt = 0
    var maskShapeLayer = CAShapeLayer()
    var shadowShapeLayer = CAShapeLayer()
    
    private var cooldownTimer: Timer?
    private var checkIfKeyTimer: Timer?
    private var animationTimer: Timer?
    private var animationDisplayLink: CADisplayLink?
    private var isCoolingDown = false
    
    private var imageLayer = CALayer()
    private var mouseCloseToEdge = false
    
    
    
    public var enabled: Bool = true {
        didSet {
            if enabled == false {
                unlock()
            }
            generateAndSetPath()
        }
    }
    
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setupTrackingArea()
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupTrackingArea()
    }
    
    override func resize(withOldSuperviewSize oldSize: NSSize) {
        super.resize(withOldSuperviewSize: oldSize)
    }
    
    override func resizeSubviews(withOldSize oldSize: NSSize) {
        
        self.layer?.shadowColor = NSColor.black.cgColor
        self.layer?.shadowOffset = CGSize(width: 10, height: 10)
        self.layer?.shadowOpacity = 1.0
        self.layer?.shadowRadius = 10
        
    }
    
    
    private var mouseMovementX = 0.0
    
    private func generatePath() -> CGPath {
        
        let pathOrigin = CGPoint(x: -10, y: 0)
        var mouseX: CGFloat
     
        var mouseY: CGFloat = clampedPosition.y
        if mouseCloseToEdge {
            let max = easeOutQuart(x: (DistanceToLeftEdgeToAnimate - clampedPosition.x) / DistanceToLeftEdgeToAnimate) * DistanceToLeftEdgeToAnimate
            var mouse = max
            if mouse > clampedPosition.x {
                mouse = clampedPosition.x
            }
            mouseX = mouse
            
        } else {
            mouseX = mouseMovementX
        }
        
        
        if !enabled {
            mouseX = 30
            mouseY = self.frame.size.height / 2
        }
        
        let mousePosition = CGPoint(x: mouseX, y: mouseY)
        let pathWidth = max(mouseX - MouseDisplacementBorderMovementThreshold, 0) + DefaultPathSize
        let pathSize = CGSize(width: pathWidth, height: self.frame.size.height)
        
        let imageX = mouseX - 10
        
        //
        imageLayer.position = CGPoint(x: mouseX - (MouseCircleSize / 3), y: mousePosition.y - MouseCircleSize / 5)
        imageLayer.removeAllAnimations()
        
        return CGPath(roundedRect: CGRect(origin:pathOrigin, size: pathSize), cornerWidth: 5, cornerHeight: 5, transform: .none)
            .union(CGPath(ellipseIn: CGRect(center: mousePosition, size: .init(width: MouseCircleSize, height: MouseCircleSize) ), transform: .none))
    }
    
    private func generateAndSetPath() {
        let path = self.generatePath()
        maskShapeLayer.path = path
        self.layer?.mask = maskShapeLayer
        shadowShapeLayer.path = path
    }
    

    private func setupTrackingArea() {
        
        
        
        self.wantsLayer = true
        blendingMode = .behindWindow
        state = .active
        
        
        material = NSVisualEffectView.Material.selection
        
        maskShapeLayer = CAShapeLayer()
        maskShapeLayer.fillColor = NSColor.blue.cgColor
        shadowShapeLayer = CAShapeLayer()
        shadowShapeLayer.fillColor = NSColor.red.cgColor
        generateAndSetPath()
        
        self.layer?.shadowColor = NSColor.black.cgColor
        self.layer?.shadowOffset = CGSize(width: 10, height: 10)
        self.layer?.shadowOpacity = 1.0
        self.layer?.shadowRadius = 10
        

        NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDragged], handler: self.updateMouse)
        
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.activeAlways, .mouseEnteredAndExited], owner: self, userInfo: nil))
        NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { event in
            if self.enabled {
                self.keyboardInput(event: event)
                return nil
            }
            else {
                return event
            }
        }
        
        checkIfKeyTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true, block: { timer in
            if let key = self.window?.isKeyWindow, !key {
                // for some reason, the didResignKey (or something like that) function did not always get called.
                self.unlock()
            }
        })

                
        self.animationDisplayLink = displayLink(target: self, selector: #selector(self.updateAnimation))
        self.animationDisplayLink?.add(to: .current, forMode: .common)
        
        
        imageLayer = CALayer()
        let image = NSImage(named: NSImage.Name("mac"))
        imageLayer.contents = image
        imageLayer.contentsScale = 0.4
        
        imageLayer.bounds = CGRect(x: 0, y: 0, width: MouseCircleSize / 1.25, height: MouseCircleSize)
        self.layer?.addSublayer(imageLayer)
        
        // rotate imageLayer 90 deg
        imageLayer.anchorPoint = CGPoint(x: 0.5, y: 0.5)
        imageLayer.transform = CATransform3DMakeRotation(-CGFloat.pi / 2, 0, 0, 1)
        

    }
    
    private var lastDisplayTimestamp : CFTimeInterval = 0
    
    
    @objc func updateAnimation(isplaylink: CADisplayLink){
        let time = self.animationDisplayLink?.targetTimestamp ?? 0
        let dt = time - lastDisplayTimestamp
        lastDisplayTimestamp = time
        
        if !isLocked  {
            if self.mouseMovementX > 1 {
                self.mouseMovementX = max(0.0, self.mouseMovementX * (0.98)) // boeie dt
            }
        }
        else {
            guard positions.positions.count > 0 else {
                return
            }
            let mouseX = positions.averageVelocity(timeSinceLast: 0.5)
            let rightComponent = mouseX.componentInDirection(.right)
            mouseMovementX = (min(rightComponent, 4000.0) / 4000.0) * MaxMouseDisplacement
        }
        if mouseMovementX != 0 {
            self.generateAndSetPath()
        }
    }
    
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        generateAndSetPath()
    }

    
    func keyboardInput(event: NSEvent) {
        if event.type == .flagsChanged {
            let newFlags = KeyboardMessage.modifierKeys(forFlags: event.modifierFlags)
            if newFlags != lastModifierKeys {
                for change in KeyboardMessage.ForModifierFlagChange(from: lastModifierKeys, to: newFlags) {
                    if change.isUp == 0 {
                        delegate?.lockedMouseViewKeyDown(change.scanCode, modifierFlags: change.modifierKeys)
                    } else {
                        delegate?.lockedMouseViewKeyUp(change.scanCode, modifierFlags: change.modifierKeys)
                    }
                }
                lastModifierKeys = newFlags
            }
        }
        else if event.type == .keyDown {
            if event.isARepeat {
                return
            }
            delegate?.lockedMouseViewKeyDown(UInt(event.keyCode), modifierFlags: self.lastModifierKeys)
        }else {
            delegate?.lockedMouseViewKeyUp(UInt(event.keyCode), modifierFlags: self.lastModifierKeys)
        }
        
    }
    
    override func rightMouseUp(with event: NSEvent) {
        self.unlock()
        delegate?.lockedMouseViewRightClick()
    }
    
    override func mouseDown(with event: NSEvent) {
        delegate?.lockedMouseViewMouseDown()
    }
    
    override func mouseUp(with event: NSEvent) {
        delegate?.lockedMouseViewMouseUp()
    }

    
    private func handleMouseMovement(positionInWindow: CGPoint, mouseDelta: CGSize, timestamp: Double) {
        
        if !self.isLocked {
            if positionInWindow.x < DistanceToLeftEdgeToLock {
                tryToGetFocusAndLock()
                clampedPosition = CGPoint(x: positionInWindow.x, y:  positionInWindow.y)
            } else if positionInWindow.x < DistanceToLeftEdgeToAnimate {
                
                clampedPosition = CGPoint(x: positionInWindow.x, y:  positionInWindow.y)
                mouseCloseToEdge = true;
                self.generateAndSetPath()
            } else {
                mouseCloseToEdge = false;
            }
            
            return
           
        } else {
            self.generateAndSetPath()
        }
        
        clampedPosition.y -= mouseDelta.height
        clampedPosition.x = positionInWindow.x
        if(clampedPosition.y < MouseCircleSize ) {
            clampedPosition.y = MouseCircleSize
        }
        if clampedPosition.y > self.frame.height - MouseCircleSize  {
            clampedPosition.y = self.frame.height - MouseCircleSize
        }
        
    
        if self.positions.append(deltaX: mouseDelta.width, deltaY: mouseDelta.height, time: timestamp, positionInWindow: positionInWindow) { // wants to unlock
            if (isLocked) {
                self.unlock()
            }
        }

        if self.isLocked {
            delegate?.lockedMouseViewMouseDidMove(CGSize(width: mouseDelta.width, height: mouseDelta.height))
        }
    }

    func updateMouse(_ event: NSEvent)  -> NSEvent {
        guard enabled else {
            return event
        }

        let mouseLocation = NSEvent.mouseLocation
        guard let positionInWindow = window?.convertPoint(fromScreen: mouseLocation) else {
            return event
        }
        
        handleMouseMovement(positionInWindow: positionInWindow, mouseDelta: CGSize(width: event.deltaX, height: event.deltaY), timestamp: event.timestamp)
        
        
        return event
    }

    override func awakeFromNib() {
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.activeAlways, .mouseEnteredAndExited], owner: self, userInfo: nil))
    }
    
    private func tryToGetFocusAndLock(){
        if let key = self.window?.isKeyWindow, !key {
            self.window?.makeKeyAndOrderFront(nil)
            NSApplication.shared.activate(ignoringOtherApps: true)
            self.window?.makeFirstResponder(nil)
        }
        self.lock()
    }
    
    func unlock(){
        guard isLocked else  {
            return
        }
        print("Unlock!!")
        NSCursor.unhide()
        CGAssociateMouseAndMouseCursorPosition(1)
        self.isLocked = false
        positions.clear();
        
        self.isCoolingDown = true
        self.cooldownTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: false) { timer in
            self.isCoolingDown = false
        }
    }
    
    func lock() {
        if isCoolingDown {
            return
        }
        
        if isLocked {
            unlock()
        }
        
        mouseCloseToEdge = false;
        self.isLocked =  true
        window?.makeFirstResponder(self)
        NSCursor.hide()
        
        self.window?.becomeKey()
        NSApplication.shared.activate(ignoringOtherApps: true)
        CGAssociateMouseAndMouseCursorPosition(0)
    }
    
    @objc override func mouseEntered(with event: NSEvent) {
        super.mouseEntered(with: event)
        guard let positionInScreen = self.window?.convertPoint(toScreen: event.locationInWindow) else {
            return
        }
        
        let positionInWindow = event.locationInWindow
        handleMouseMovement(positionInWindow: positionInWindow, mouseDelta: CGSize(width: event.deltaX, height: event.deltaY), timestamp: event.timestamp)
    }
    
    @objc override func mouseExited(with event: NSEvent) {
        super.mouseExited(with: event)
        self.unlock()
        
    }
    
    
    
    override var acceptsFirstResponder: Bool { true }
    
    override func viewDidMoveToWindow() {
        window?.makeFirstResponder(self)
    }
    // when losing focus:
    override func resignFirstResponder() -> Bool {
        super.resignFirstResponder()
        self.unlock()
        return true
    }
    
    
    // Cleanup when exiting
    deinit {
        NSCursor.unhide()
    }
}





func easeOutQuart(x: CGFloat) -> CGFloat {
    return 1.0 - pow(1.0 - x, 4.0);
}
