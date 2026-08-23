import AudioToolbox
import UIKit

enum FeedbackManager {
    private static let light = UIImpactFeedbackGenerator(style: .light)
    private static let medium = UIImpactFeedbackGenerator(style: .medium)
    private static let heavy = UIImpactFeedbackGenerator(style: .heavy)
    private static let notification = UINotificationFeedbackGenerator()

    static func prepare() {
        light.prepare()
        medium.prepare()
        heavy.prepare()
        notification.prepare()
    }

    static func countdownTick() {
        light.impactOccurred()
    }

    static func goCue() {
        notification.notificationOccurred(.success)
        AudioServicesPlaySystemSound(1057)
    }

    static func blockStart() {
        medium.impactOccurred()
    }

    static func pauseToggle() {
        medium.impactOccurred()
    }

    static func warning() {
        notification.notificationOccurred(.warning)
    }
}

enum ByteCountFormatterUtil {
    static func string(from bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

enum DurationFormatterUtil {
    static func mmss(_ interval: TimeInterval) -> String {
        let total = max(0, Int(interval))
        return String(format: "%02d:%02d", total / 60, total % 60)
    }

    static func hms(_ interval: TimeInterval) -> String {
        let total = max(0, Int(interval))
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let seconds = total % 60
        if hours > 0 {
            return String(format: "%d:%02d:%02d", hours, minutes, seconds)
        }
        return String(format: "%02d:%02d", minutes, seconds)
    }
}
