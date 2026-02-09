//
//  SerialError.swift
//  MacFriends
//
//  Created by Bart Jakobs on 30/07/2025.
//
import Foundation



enum SerialError: Error{
    case failedToOpen
    case writingError
    case errorInReading
}

