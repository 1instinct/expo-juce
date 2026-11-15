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
//
// Sampler mode (mode "sampler"): `note` is a sampler slot (0–15). Every touch-down hits the slot at
// its root note (a second finger retriggers, like drums) and the last touch-up releases the slot
// (a one-shot slot ignores the release and plays to its end).
class JucePadView: ExpoView {
  enum Mode: String {
    case notes
    case drums
    case sampler
  }

  let onNoteOn = EventDispatcher()
  let onNoteOff = EventDispatcher()

  var note: Int = 60
  var velocity: Int = 100
  // What the pad plays. Changing the mode releases whatever this pad is sounding.
  var padMode: Mode = .notes {
    didSet {
      if padMode != oldValue {
        releaseAllTouches()
      }
    }
  }
  // Drums and sampler pads hit on every touch-down; notes pads hold one note.
  private var hitsEveryTouch: Bool {
    return padMode != .notes
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
  // The mode the sounding note was started in (what to release in the engine: nothing for a drum
  // hit, the slot for a sampler hit, the note otherwise).
  private var soundingMode: Mode = .notes

  required init(appContext: AppContext? = nil) {
    super.init(appContext: appContext)
    isMultipleTouchEnabled = true
    // Slow-touch probe: start watching app activity changes (once per process).
    _ = AppActivityClock.shared
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
    let heldBefore = activeTouches.count
    activeTouches.formUnion(touches)
    let startsNote = (wasIdle || hitsEveryTouch) && !activeTouches.isEmpty
    if startsNote {
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
    probeSlowTouches(touches, event: event, entryTimestamp: entryTimestamp, heldBefore: heldBefore,
                     startedNote: startsNote)
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
    endTouches(touches, cancelled: false)
  }

  override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
    guard enabled else {
      super.touchesCancelled(touches, with: event)
      return
    }
    endTouches(touches, cancelled: true)
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
    let recognizers: [[String: Any]] = (touch.gestureRecognizers ?? []).prefix(30).map { JucePadView.describe($0) }
    // Typed locals keep the dictionary literal below cheap to type-check.
    let deliveryMs: Double = (entryTimestamp - touch.timestamp) * 1000.0
    let horizontalEdge: CGFloat = min(point.x - bounds.minX, bounds.maxX - point.x)
    let verticalEdge: CGFloat = min(point.y - bounds.minY, bounds.maxY - point.y)
    let edgeDistance: Double = Double(min(horizontalEdge, verticalEdge))
    let report: [String: Any] = [
      "deliveryMs": deliveryMs,
      "x": Double(point.x),
      "y": Double(point.y),
      "windowWidth": Double(bounds.width),
      "windowHeight": Double(bounds.height),
      "edgeDistance": edgeDistance,
      "recognizers": recognizers,
    ]
    ExpoJuceBridge.reportLastTouch(report)
  }

  // MARK: - Slow-touch probe (diagnostics, off the note path)

  // A touch whose delivery (touchesBegan entry − UITouch.timestamp) is over slowTouchSec. `fields`
  // is what diagnostics report (touchLatency.slowTouchLog); the end and the main-thread gaps are
  // filled in after the fact. The touch is held weakly, only to match its end. Main thread only.
  private final class SlowTouchRecord {
    weak var touch: UITouch?
    var fields: [String: Any]
    let touchTimestamp: TimeInterval
    let entryTimestamp: CFTimeInterval
    // CACurrentMediaTime() when the pad handled the touch's end (0: not yet).
    var endTimestamp: CFTimeInterval = 0
    var deliveryGapMeasured = false
    var lifetimeGapMeasured = false

    init(touch: UITouch, fields: [String: Any], touchTimestamp: TimeInterval, entryTimestamp: CFTimeInterval) {
      self.touch = touch
      self.fields = fields
      self.touchTimestamp = touchTimestamp
      self.entryTimestamp = entryTimestamp
    }
  }

  private static let slowTouchSec: Double = 0.1
  private static let slowTouchLogSize = 8
  // The last slowTouchLogSize slow touches of every pad, oldest first.
  private static var slowTouchLog: [SlowTouchRecord] = []
  // A report to the engine is scheduled (coalesces bursts; the gaps are measured then).
  private static var slowTouchReportScheduled = false

  // Runs after the note has started: counts this call's touches (and the slow ones) in the engine
  // with no allocation, and records the slow ones in detail. heldBefore: the view's touches before
  // this call; startedNote: whether this call started a note or hit.
  private func probeSlowTouches(_ touches: Set<UITouch>, event: UIEvent?, entryTimestamp: CFTimeInterval,
                                heldBefore: Int, startedNote: Bool) {
    var over100ms: Int32 = 0
    var over1s: Int32 = 0
    for touch in touches {
      let delivery: Double = entryTimestamp - touch.timestamp
      if delivery > JucePadView.slowTouchSec {
        over100ms += 1
      }
      if delivery > 1.0 {
        over1s += 1
      }
    }
    ExpoJuceBridge.countPadTouches(Int32(clamping: touches.count), over100ms: over100ms, over1s: over1s)
    guard over100ms > 0 else {
      return
    }

    // Shared by every slow touch of this call.
    let appState: String
    switch UIApplication.shared.applicationState {
    case .active:
      appState = "active"
    case .inactive:
      appState = "inactive"
    case .background:
      appState = "background"
    @unknown default:
      appState = "unknown"
    }
    let clock = AppActivityClock.shared
    let becameActiveAt: CFTimeInterval = clock.didBecomeActiveAt
    let resignedActiveAt: CFTimeInterval = clock.willResignActiveAt
    let eventTimestamp: Double = event?.timestamp ?? 0
    let eventTouches: Int = event?.allTouches?.count ?? 0
    // Touches of this call that the view already held (a touch delivered to touchesBegan twice).
    let alreadyHeld: Int = max(0, heldBefore + touches.count - activeTouches.count)

    for touch in touches {
      let touchTimestamp: TimeInterval = touch.timestamp
      let delivery: Double = entryTimestamp - touchTimestamp
      guard delivery > JucePadView.slowTouchSec else {
        continue
      }
      var edgeDistance: Double = -1
      if let window = touch.window {
        let point = touch.location(in: window)
        let bounds = window.bounds
        let horizontalEdge: CGFloat = min(point.x - bounds.minX, bounds.maxX - point.x)
        let verticalEdge: CGFloat = min(point.y - bounds.minY, bounds.maxY - point.y)
        edgeDistance = Double(min(horizontalEdge, verticalEdge))
      }
      let attached: [UIGestureRecognizer] = touch.gestureRecognizers ?? []
      let recognizers: [[String: Any]] = attached.prefix(12).map { (recognizer: UIGestureRecognizer) -> [String: Any] in
        let entry: [String: Any] = [
          "class": NSStringFromClass(type(of: recognizer)),
          "state": JucePadView.stateName(recognizer.state),
          "delaysTouchesBegan": recognizer.delaysTouchesBegan,
        ]
        return entry
      }
      let activityChanged: Bool = (becameActiveAt >= touchTimestamp && becameActiveAt <= entryTimestamp)
        || (resignedActiveAt >= touchTimestamp && resignedActiveAt <= entryTimestamp)

      var fields: [String: Any] = [:]
      fields["deliveryMs"] = delivery * 1000.0
      fields["touchTimestamp"] = touchTimestamp
      fields["entryTimestamp"] = entryTimestamp
      fields["eventTimestamp"] = eventTimestamp
      fields["eventDeliveryMs"] = eventTimestamp > 0 ? (entryTimestamp - eventTimestamp) * 1000.0 : -1.0
      fields["phase"] = JucePadView.phaseName(touch.phase)
      fields["tapCount"] = touch.tapCount
      fields["majorRadius"] = Double(touch.majorRadius)
      fields["edgeDistance"] = edgeDistance
      fields["touchesInCall"] = touches.count
      fields["heldBefore"] = heldBefore
      fields["alreadyHeld"] = alreadyHeld
      fields["eventTouches"] = eventTouches
      fields["mode"] = padMode.rawValue
      fields["startedNote"] = startedNote
      fields["recognizers"] = recognizers
      fields["appState"] = appState
      fields["sinceDidBecomeActiveSec"] = becameActiveAt > 0 ? entryTimestamp - becameActiveAt : -1.0
      fields["sinceWillResignActiveSec"] = resignedActiveAt > 0 ? entryTimestamp - resignedActiveAt : -1.0
      fields["activityChangedDuringDelivery"] = activityChanged
      // Filled in after the fact (-1 / "" / false until then).
      fields["endPhase"] = ""
      fields["endedAfterMs"] = -1.0
      fields["endedWithin50ms"] = false
      fields["heldMs"] = -1.0
      fields["endDeliveryMs"] = -1.0
      fields["deliveryGapMs"] = -1.0
      fields["lifetimeGapMs"] = -1.0

      let record = SlowTouchRecord(touch: touch, fields: fields, touchTimestamp: touchTimestamp,
                                   entryTimestamp: entryTimestamp)
      JucePadView.slowTouchLog.append(record)
    }
    let overflow = JucePadView.slowTouchLog.count - JucePadView.slowTouchLogSize
    if overflow > 0 {
      JucePadView.slowTouchLog.removeFirst(overflow)
    }
    JucePadView.scheduleSlowTouchReport()
  }

  // After the note-off for these touches: fills in the end of any slow touch among them.
  private static func noteSlowTouchEnds(_ touches: Set<UITouch>, cancelled: Bool) {
    guard !slowTouchLog.isEmpty else {
      return
    }
    let now = CACurrentMediaTime()
    var changed = false
    for record in slowTouchLog {
      // (The timestamp check skips a touch object UIKit might reuse for a later touch.)
      guard let touch = record.touch, touches.contains(touch), touch.timestamp >= record.touchTimestamp else {
        continue
      }
      record.touch = nil
      record.endTimestamp = now
      let endedAfterMs: Double = (now - record.entryTimestamp) * 1000.0
      record.fields["endPhase"] = cancelled ? "cancelled" : "ended"
      record.fields["endedAfterMs"] = endedAfterMs
      record.fields["endedWithin50ms"] = endedAfterMs <= 50.0
      record.fields["heldMs"] = (touch.timestamp - record.touchTimestamp) * 1000.0
      record.fields["endDeliveryMs"] = (now - touch.timestamp) * 1000.0
      changed = true
    }
    if changed {
      scheduleSlowTouchReport()
    }
  }

  // Reports the log 0.25 s later (at most one pending report), measuring the gaps whose window
  // ended gapSettleSec ago or more: by then the display-link tick that ends a main-thread stall
  // overlapping the touch has been logged. Gaps not yet settled schedule another report.
  private static func scheduleSlowTouchReport() {
    guard !slowTouchReportScheduled else {
      return
    }
    slowTouchReportScheduled = true
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
      JucePadView.slowTouchReportScheduled = false
      JucePadView.publishSlowTouchLog()
    }
  }

  // A gap is measured only once its window ended at least gapSettleSec ago (a touch that arrived
  // while a report was already scheduled waits for the next one).
  private static let gapSettleSec: Double = 0.2

  private static func publishSlowTouchLog() {
    let now = CACurrentMediaTime()
    var waiting = false
    for record in slowTouchLog {
      if !record.deliveryGapMeasured {
        if now - record.entryTimestamp >= gapSettleSec {
          record.fields["deliveryGapMs"] = ExpoJuceBridge.longestMainThreadGapMs(from: record.touchTimestamp,
                                                                                to: record.entryTimestamp)
          record.deliveryGapMeasured = true
        } else {
          waiting = true
        }
      }
      if !record.lifetimeGapMeasured && record.endTimestamp > 0 {
        if now - record.endTimestamp >= gapSettleSec {
          record.fields["lifetimeGapMs"] = ExpoJuceBridge.longestMainThreadGapMs(from: record.touchTimestamp,
                                                                                to: record.endTimestamp)
          record.lifetimeGapMeasured = true
        } else {
          waiting = true
        }
      }
    }
    let entries: [[String: Any]] = slowTouchLog.map { $0.fields }
    ExpoJuceBridge.reportSlowTouchLog(entries)
    if waiting {
      scheduleSlowTouchReport()
    }
  }

  private static func phaseName(_ phase: UITouch.Phase) -> String {
    switch phase {
    case .began:
      return "began"
    case .moved:
      return "moved"
    case .stationary:
      return "stationary"
    case .ended:
      return "ended"
    case .cancelled:
      return "cancelled"
    default:
      return "other(\(phase.rawValue))"
    }
  }

  private static func stateName(_ state: UIGestureRecognizer.State) -> String {
    switch state {
    case .possible:
      return "possible"
    case .began:
      return "began"
    case .changed:
      return "changed"
    case .ended:
      return "ended"
    case .cancelled:
      return "cancelled"
    case .failed:
      return "failed"
    @unknown default:
      return "unknown(\(state.rawValue))"
    }
  }

  // MARK: - Notes

  private func endTouches(_ touches: Set<UITouch>, cancelled: Bool) {
    activeTouches.subtract(touches)
    if activeTouches.isEmpty {
      stopNote()
    }
    if touchPathNeedsRefresh {
      touchPathNeedsRefresh = false
      reportTouchPath()
    }
    // After the note-off: diagnostics only.
    JucePadView.noteSlowTouchEnds(touches, cancelled: cancelled)
  }

  private func releaseAllTouches() {
    activeTouches.removeAll()
    stopNote()
  }

  private func startNote(touchTimestamp: TimeInterval, entryTimestamp: CFTimeInterval) {
    // A drums / sampler retrigger (another finger while a hit is held) is just another hit: no
    // onNoteOff (or sampler release) in between, that comes only when the last touch lifts.
    let noteNumber = note
    let noteVelocity = velocity
    // (A sampler pad whose slot changed while held releases the old slot first.)
    let retrigger = hitsEveryTouch && soundingMode == padMode && (padMode == .drums || soundingNote == noteNumber)
    if !retrigger {
      stopNote()
    }
    soundingNote = noteNumber
    soundingMode = padMode
    switch padMode {
    case .drums:
      ExpoJuceBridge.padDrumHit(Int32(clamping: noteNumber),
                                velocity: Int32(clamping: noteVelocity),
                                touchTimestamp: touchTimestamp,
                                entryTimestamp: entryTimestamp)
    case .sampler:
      ExpoJuceBridge.padSamplerHit(Int32(clamping: noteNumber),
                                   velocity: Int32(clamping: noteVelocity),
                                   touchTimestamp: touchTimestamp,
                                   entryTimestamp: entryTimestamp)
    case .notes:
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
    switch soundingMode {
    case .drums:
      break
    case .sampler:
      ExpoJuceBridge.samplerRelease(Int32(clamping: noteNumber))
    case .notes:
      ExpoJuceBridge.noteOff(Int32(clamping: noteNumber))
    }
    soundingMode = .notes
    let payload: [String: Any] = [
      "note": noteNumber,
    ]
    onNoteOff(payload)
  }
}

// Slow-touch probe: when the app last became active and last resigned active (CACurrentMediaTime;
// 0 = not seen since the first pad was created). Notifications arrive on the main thread.
private final class AppActivityClock: NSObject {
  static let shared = AppActivityClock()

  private(set) var didBecomeActiveAt: CFTimeInterval = 0
  private(set) var willResignActiveAt: CFTimeInterval = 0

  private override init() {
    super.init()
    let center = NotificationCenter.default
    center.addObserver(self, selector: #selector(appDidBecomeActive),
                       name: UIApplication.didBecomeActiveNotification, object: nil)
    center.addObserver(self, selector: #selector(appWillResignActive),
                       name: UIApplication.willResignActiveNotification, object: nil)
  }

  @objc private func appDidBecomeActive() {
    didBecomeActiveAt = CACurrentMediaTime()
  }

  @objc private func appWillResignActive() {
    willResignActiveAt = CACurrentMediaTime()
  }
}
