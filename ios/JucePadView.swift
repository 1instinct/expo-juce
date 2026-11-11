import ExpoModulesCore
import UIKit

// Native touch pad: triggers the engine directly from UIKit touch handling on the main thread
// (no JS round trip). Multi-touch safe: the note starts with the first touch on the view and
// stops when the last touch on it lifts or is cancelled. onNoteOn / onNoteOff are for visuals
// and haptics only; the sound has already been triggered when they fire.
//
// Drums mode (mode "drums"): `note` is a drum voice (0–8) and every touch-down hits it (one-shot,
// with the same velocity as a note; a second finger retriggers). onNoteOn fires per hit and
// onNoteOff when the last touch lifts; no note-off is sent to the engine.
class JucePadView: ExpoView {
  let onNoteOn = EventDispatcher()
  let onNoteOff = EventDispatcher()

  var note: Int = 60
  var velocity: Int = 100
  // Drums mode. Changing the mode releases whatever this pad is sounding.
  var drumMode: Bool = false {
    didSet {
      if drumMode != oldValue {
        releaseAllTouches()
      }
    }
  }
  // Disabled pads leave hit-testing entirely, so touches reach the views underneath (forwarding
  // to super would only walk the responder chain, which does not include siblings).
  var enabled: Bool = true {
    didSet {
      if !enabled {
        releaseAllTouches()
      }
      isUserInteractionEnabled = enabled
    }
  }

  // Hit shape: a polygon in normalised view coordinates, flat [x0, y0, x1, y1, …] with 0–1
  // spanning the view's bounds. Touches outside it fall through to the views underneath.
  // Empty, odd-length or fewer than 3 points: the whole rect (the default).
  var hitPolygon: [Double] = [] {
    didSet {
      rebuildHitPath()
    }
  }

  private var hitPath: UIBezierPath?
  private var hitPathBounds: CGRect = .null

  private var activeTouches = Set<UITouch>()
  private var soundingNote: Int?
  // The sounding note is a drum hit (nothing to release in the engine).
  private var soundingIsDrum = false

  required init(appContext: AppContext? = nil) {
    super.init(appContext: appContext)
    isMultipleTouchEnabled = true
  }

  // MARK: - Hit shape

  override func layoutSubviews() {
    super.layoutSubviews()
    if bounds != hitPathBounds {
      rebuildHitPath()
    }
  }

  override var bounds: CGRect {
    didSet {
      if bounds != hitPathBounds {
        rebuildHitPath()
      }
    }
  }

  override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
    guard let path = hitPath else {
      return super.point(inside: point, with: event)
    }
    return path.contains(point)
  }

  private func rebuildHitPath() {
    hitPathBounds = bounds
    let values = hitPolygon
    guard values.count >= 6, values.count % 2 == 0, values.allSatisfy({ $0.isFinite }) else {
      hitPath = nil
      return
    }
    let path = UIBezierPath()
    let width = Double(bounds.width)
    let height = Double(bounds.height)
    for index in stride(from: 0, to: values.count, by: 2) {
      let point = CGPoint(x: bounds.minX + CGFloat(values[index] * width),
                          y: bounds.minY + CGFloat(values[index + 1] * height))
      if index == 0 {
        path.move(to: point)
      } else {
        path.addLine(to: point)
      }
    }
    path.close()
    hitPath = path
  }

  // MARK: - Touches

  override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesBegan(touches, with: event)
      return
    }
    let wasIdle = activeTouches.isEmpty
    activeTouches.formUnion(touches)
    if (wasIdle || drumMode) && !activeTouches.isEmpty {
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
    // A drums-mode retrigger (another finger while a hit is held) is just another hit: no
    // onNoteOff in between, that fires only when the last touch lifts.
    if !(drumMode && soundingIsDrum) {
      stopNote()
    }
    let noteNumber = note
    let noteVelocity = velocity
    soundingNote = noteNumber
    soundingIsDrum = drumMode
    if drumMode {
      ExpoJuceBridge.padDrumHit(Int32(clamping: noteNumber),
                                velocity: Int32(clamping: noteVelocity),
                                touchTimestamp: touchTimestamp)
    } else {
      ExpoJuceBridge.padNoteOn(Int32(clamping: noteNumber),
                               velocity: Int32(clamping: noteVelocity),
                               touchTimestamp: touchTimestamp)
    }
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
    if !soundingIsDrum {
      ExpoJuceBridge.noteOff(Int32(clamping: noteNumber))
    }
    soundingIsDrum = false
    let payload: [String: Any] = [
      "note": noteNumber,
    ]
    onNoteOff(payload)
  }
}
