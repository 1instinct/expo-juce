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
  // The touch path (ancestor recognizers / scroll views) is reported again on the first touch end
  // after attaching: recognizers some libraries attach after mount are then in place.
  private var touchPathNeedsRefresh = false
  // Main thread: when any pad last reported its touch (reportLastTouch is throttled).
  private static var lastTouchReportTime: CFTimeInterval = 0
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
    // First thing: the handler's entry time (CACurrentMediaTime and UITouch.timestamp are both
    // seconds on the mach_absolute_time clock), for the delivery and handler latency legs.
    let entryTimestamp = CACurrentMediaTime()
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
      startNote(touchTimestamp: timestamp, entryTimestamp: entryTimestamp)
    }
    // After the note: diagnostics only.
    reportLastTouch(touches, entryTimestamp: entryTimestamp)
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

  override func didMoveToWindow() {
    super.didMoveToWindow()
    guard window != nil else {
      return
    }
    reportTouchPath()
    // Again a second later and on the first touch end: recognizers that libraries attach after
    // mount are in place by then.
    touchPathNeedsRefresh = true
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
      guard let self = self, self.window != nil else {
        return
      }
      self.reportTouchPath()
    }
  }

  // MARK: - Touch path diagnostics (off the note path)

  // Every gesture recognizer and scroll view from this view up to its window (the window's own
  // recognizers included): anything there with a delay flag can hold touchesBegan back. depth 0 is
  // this view. Over the engine's cap (30), entries that can delay touchesBegan come first.
  private func reportTouchPath() {
    var entries: [[String: Any]] = []
    var current: UIView? = self
    var depth = 0
    while let view = current {
      if let scrollView = view as? UIScrollView {
        entries.append([
          "kind": "scrollView",
          "class": NSStringFromClass(type(of: scrollView)),
          "depth": depth,
          "scrollEnabled": scrollView.isScrollEnabled,
          "delaysContentTouches": scrollView.delaysContentTouches,
          "canCancelContentTouches": scrollView.canCancelContentTouches,
        ])
      }
      for recognizer in view.gestureRecognizers ?? [] {
        var entry = JucePadView.describe(recognizer)
        entry["depth"] = depth
        entries.append(entry)
      }
      if view is UIWindow {
        break
      }
      current = view.superview
      depth += 1
    }
    let cap = 30
    if entries.count > cap {
      // Keep the original order among the kept entries.
      let ranked = entries.indices.sorted { a, b in
        let ra = JucePadView.delayRank(entries[a])
        let rb = JucePadView.delayRank(entries[b])
        return ra != rb ? ra < rb : a < b
      }
      entries = Array(ranked.prefix(cap)).sorted().map { entries[$0] }
    }
    ExpoJuceBridge.reportTouchPath(entries)
  }

  // 0: can delay touchesBegan; 1: another flag on; 2: the rest.
  private static func delayRank(_ entry: [String: Any]) -> Int {
    if entry["delaysTouchesBegan"] as? Bool == true {
      return 0
    }
    if entry["kind"] as? String == "scrollView" {
      let delays = entry["delaysContentTouches"] as? Bool == true && entry["scrollEnabled"] as? Bool == true
      return delays ? 0 : 1
    }
    if entry["delaysTouchesEnded"] as? Bool == true || entry["cancelsTouchesInView"] as? Bool == true {
      return 1
    }
    return 2
  }

  private static func describe(_ recognizer: UIGestureRecognizer) -> [String: Any] {
    return [
      "kind": "recognizer",
      "class": NSStringFromClass(type(of: recognizer)),
      "view": recognizer.view.map { NSStringFromClass(type(of: $0)) } ?? "",
      "enabled": recognizer.isEnabled,
      "delaysTouchesBegan": recognizer.delaysTouchesBegan,
      "delaysTouchesEnded": recognizer.delaysTouchesEnded,
      "cancelsTouchesInView": recognizer.cancelsTouchesInView,
    ]
  }

  // The recognizers this touch is delivered to (including ones not on any ancestor, such as the
  // system's screen-edge gate), how late it arrived and where it landed. At most every 0.5 s.
  private func reportLastTouch(_ touches: Set<UITouch>, entryTimestamp: CFTimeInterval) {
    guard entryTimestamp - JucePadView.lastTouchReportTime >= 0.5,
          let touch = touches.first,
          let window = window else {
      return
    }
    JucePadView.lastTouchReportTime = entryTimestamp
    let point = touch.location(in: window)
    let bounds = window.bounds
    let recognizers = (touch.gestureRecognizers ?? []).prefix(30).map { JucePadView.describe($0) }
    ExpoJuceBridge.reportLastTouch([
      "deliveryMs": (entryTimestamp - touch.timestamp) * 1000.0,
      "x": Double(point.x),
      "y": Double(point.y),
      "windowWidth": Double(bounds.width),
      "windowHeight": Double(bounds.height),
      "edgeDistance": Double(min(point.x - bounds.minX, bounds.maxX - point.x,
                                 point.y - bounds.minY, bounds.maxY - point.y)),
      "recognizers": Array(recognizers),
    ])
  }

  // MARK: - Notes

  private func endTouches(_ touches: Set<UITouch>) {
    activeTouches.subtract(touches)
    if activeTouches.isEmpty {
      stopNote()
    }
    if touchPathNeedsRefresh {
      touchPathNeedsRefresh = false
      reportTouchPath()
    }
  }

  private func releaseAllTouches() {
    activeTouches.removeAll()
    stopNote()
  }

  private func startNote(touchTimestamp: TimeInterval, entryTimestamp: CFTimeInterval) {
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
                                touchTimestamp: touchTimestamp,
                                entryTimestamp: entryTimestamp)
    } else {
      ExpoJuceBridge.padNoteOn(Int32(clamping: noteNumber),
                               velocity: Int32(clamping: noteVelocity),
                               touchTimestamp: touchTimestamp,
                               entryTimestamp: entryTimestamp)
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
