//
//  AudioDevices.swift
//
//  The output devices media can play through. AVPlayer routes audio by CoreAudio
//  device UID, but a UID is unreadable, so the setting stores the device's name
//  (as the Windows and Linux apps do) and the UID is looked up when playback
//  starts — which also means an unplugged device simply falls back to the
//  system's own output instead of failing.
//

import CoreAudio
import Foundation

enum AudioDevices {
    /// Every device with output channels, by display name, in CoreAudio's order.
    /// The system default isn't included — the settings page offers that itself.
    static func outputNames() -> [String] {
        var names: [String] = []
        for id in deviceIDs() where hasOutput(id) {
            guard let name = string(id, kAudioObjectPropertyName), !name.isEmpty,
                  !names.contains(name) else { continue }
            names.append(name)
        }
        return names
    }

    /// The UID for a device the user picked by name; nil when the name is empty
    /// or that device isn't connected any more.
    static func uid(forName name: String) -> String? {
        guard !name.isEmpty else { return nil }
        for id in deviceIDs() where hasOutput(id) {
            if string(id, kAudioObjectPropertyName) == name {
                return string(id, kAudioDevicePropertyDeviceUID)
            }
        }
        return nil
    }

    // MARK: - CoreAudio plumbing

    private static func address(
        _ selector: AudioObjectPropertySelector,
        _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
    ) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope,
                                   mElement: kAudioObjectPropertyElementMain)
    }

    private static func deviceIDs() -> [AudioObjectID] {
        let system = AudioObjectID(kAudioObjectSystemObject)
        var addr = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(system, &addr, 0, nil, &size) == noErr,
              size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0,
                                  count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(system, &addr, 0, nil, &size, &ids) == noErr else {
            return []
        }
        return ids
    }

    private static func string(_ id: AudioObjectID,
                               _ selector: AudioObjectPropertySelector) -> String? {
        var addr = address(selector)
        var size = UInt32(MemoryLayout<CFString?>.size)
        var value: CFString?
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr else {
            return nil
        }
        return value as String?
    }

    /// Input-only devices (microphones) have no output channels; they'd be
    /// nonsense in a list of places to play audio.
    private static func hasOutput(_ id: AudioObjectID) -> Bool {
        var addr = address(kAudioDevicePropertyStreamConfiguration,
                           kAudioObjectPropertyScopeOutput)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr,
              size > 0 else { return false }
        let buffer = UnsafeMutableRawPointer.allocate(
            byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { buffer.deallocate() }
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, buffer) == noErr else {
            return false
        }
        let list = UnsafeMutableAudioBufferListPointer(
            buffer.assumingMemoryBound(to: AudioBufferList.self))
        return list.contains { $0.mNumberChannels > 0 }
    }
}
