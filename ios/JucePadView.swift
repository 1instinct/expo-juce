import ExpoModulesCore
import UIKit

// Native touch pad: triggers the engine directly from UIKit touch handling on the main thread
// (no JS round trip). Multi-touch safe: the note starts with the first touch on the view and
// stops when the last touch on it lifts or is cancelled. onNoteOn / onNoteOff are for visuals
// and haptics only; the sound has already been triggered when they fire.
class JucePadView: ExpoView {
  let onNoteOn = EventDispatcher()
  let onNoteOff = EventDispatcher()

  var note: Int = 60
  var velocity: Int = 100
  var enabled: Bool = true {
    didSet {
      if !enabled {
        releaseAllTouches()
      }
    }
  }

  private var activeTouches = Set<UITouch>()
  private var soundingNote: Int?

  required init(appContext: AppContext? = nil) {
    super.init(appContext: appContext)
    isMultipleTouchEnabled = true
  }

  // MARK: - Touches

  override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesBegan(touches, with: event)
      return
    }
    let wasIdle = activeTouches.isEmpty
    activeTouches.formUnion(touches)
    if wasIdle && !activeTouches.isEmpty {
      var timestamp: TimeInterval = 0
      for touch in touches {
        if timestamp == 0 || touch.timestamp < timestamp {
          timestamp = touch.timestamp
        }
      }
      startNote(touchTimestamp: timestamp)
    }
  }

  override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesMoved(touches, with: event)
      return
    }
  }

  override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesEnded(touches, with: event)
      return
    }
    endTouches(touches)
  }

  override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesCancelled(touches, with: event)
      return
    }
    endTouches(touches)
  }

  override func willMove(toWindow newWindow: UIWindow?) {
    super.willMove(toWindow: newWindow)
    if newWindow == nil {
      releaseAllTouches()
    }
  }

  // MARK: - Notes

  private func endTouches(_ touches: Set<UITouch>) {
    activeTouches.subtract(touches)
    if activeTouches.isEmpty {
      stopNote()
    }
  }

  private func releaseAllTouches() {
    activeTouches.removeAll()
    stopNote()
  }

  private func startNote(touchTimestamp: TimeInterval) {
    stopNote()
    let noteNumber = note
    let noteVelocity = velocity
    soundingNote = noteNumber
    ExpoJuceBridge.padNoteOn(Int32(clamping: noteNumber),
                             velocity: Int32(clamping: noteVelocity),
                             touchTimestamp: touchTimestamp)
    let payload: [String: Any] = [
      "note": noteNumber,
      "velocity": noteVelocity,
    ]
    onNoteOn(payload)
  }

  private func stopNote() {
    guard let noteNumber = soundingNote else {
      return
    }
    soundingNote = nil
    ExpoJuceBridge.noteOff(Int32(clamping: noteNumber))
    let payload: [String: Any] = [
      "note": noteNumber,
    ]
    onNoteOff(payload)
  }
}
