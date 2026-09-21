import SwiftUI
import UIKit

struct CaptureImageViewerItem: Identifiable {
    let id: String
    let image: UIImage
    let title: String
    let distanceMm: UInt16?
}

/// Full-resolution capture inspection with pinch, pan, and double-tap zoom.
/// Rotation is intentionally absent here because it is a global processing
/// choice in Settings and is materialized into every saved PNG.
struct CaptureImageViewer: View {
    @Environment(\.dismiss) private var dismiss
    let item: CaptureImageViewerItem

    @State private var scale: CGFloat = 1
    @State private var settledScale: CGFloat = 1
    @State private var offset: CGSize = .zero
    @State private var settledOffset: CGSize = .zero

    var body: some View {
        NavigationStack {
            GeometryReader { proxy in
                ZStack {
                    Color.black.ignoresSafeArea()
                    Image(uiImage: item.image)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFit()
                        .frame(width: proxy.size.width, height: proxy.size.height)
                        .scaleEffect(scale)
                        .offset(offset)
                        .contentShape(Rectangle())
                        .onTapGesture(count: 2) {
                            withAnimation(.easeInOut(duration: 0.2)) {
                                if scale > 1 {
                                    resetZoom()
                                } else {
                                    scale = 3
                                    settledScale = 3
                                }
                            }
                        }
                        .simultaneousGesture(magnificationGesture)
                        .simultaneousGesture(dragGesture)
                    if let distanceMm = item.distanceMm {
                        VStack {
                            Spacer()
                            Label("Distance \(distanceMm) mm", systemImage: "ruler")
                                .font(.headline.monospacedDigit())
                                .foregroundStyle(.white)
                                .padding(.horizontal, 14)
                                .padding(.vertical, 9)
                                .background(.ultraThinMaterial, in: Capsule())
                                .padding(.bottom, 18)
                        }
                        .allowsHitTesting(false)
                    }
                }
            }
            .navigationTitle(item.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(.black, for: .navigationBar)
            .toolbarColorScheme(.dark, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button("Reset") {
                        withAnimation(.easeInOut(duration: 0.2)) { resetZoom() }
                    }
                    .disabled(scale == 1 && offset == .zero)
                }
            }
        }
    }

    private var magnificationGesture: some Gesture {
        MagnificationGesture()
            .onChanged { value in
                scale = min(max(settledScale * value, 1), 8)
                if scale == 1 { offset = .zero }
            }
            .onEnded { _ in
                settledScale = scale
                if scale == 1 {
                    offset = .zero
                    settledOffset = .zero
                }
            }
    }

    private var dragGesture: some Gesture {
        DragGesture()
            .onChanged { value in
                guard scale > 1 else { return }
                offset = CGSize(
                    width: settledOffset.width + value.translation.width,
                    height: settledOffset.height + value.translation.height
                )
            }
            .onEnded { _ in
                settledOffset = offset
            }
    }

    private func resetZoom() {
        scale = 1
        settledScale = 1
        offset = .zero
        settledOffset = .zero
    }
}
