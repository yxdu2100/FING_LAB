import SwiftUI

struct ContentView: View {
    @StateObject private var model = CaptureViewModel()
    @State private var confirmDeleteAll = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 18) {
                    statusCard
                    previewCard
                    controls
                    gallery
                }
                .padding()
            }
            .navigationTitle("Fing Lab Capture")
            .alert("Capture error", isPresented: Binding(
                get: { model.errorMessage != nil },
                set: { if !$0 { model.errorMessage = nil } }
            )) {
                Button("OK") { model.errorMessage = nil }
            } message: {
                Text(model.errorMessage ?? "Unknown error")
            }
            .confirmationDialog("Delete every saved image and raw Bayer file?",
                                isPresented: $confirmDeleteAll,
                                titleVisibility: .visible) {
                Button("Delete All", role: .destructive) { model.deleteAll() }
                Button("Cancel", role: .cancel) {}
            }
        }
    }

    private var statusCard: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(model.connectionState.ready ? Color.green : Color.orange)
                .frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 3) {
                Text(model.connectionState.description).font(.headline)
                Text("Double flick → ToF check → photo")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Reconnect") { model.reconnect() }
                .buttonStyle(.bordered)
        }
        .padding()
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var previewCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Latest 320 × 240 Preview").font(.headline)
                Spacer()
                if let frameID = model.latestFrameID {
                    Text("Frame \(frameID)").font(.caption.monospacedDigit())
                }
            }
            ZStack {
                RoundedRectangle(cornerRadius: 12).fill(Color.black)
                if let image = model.latestImage {
                    Image(uiImage: image)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFit()
                } else {
                    ContentUnavailableView("Waiting for a photo",
                                           systemImage: "camera",
                                           description: Text("Keep this screen open during Phase 1/2."))
                        .foregroundStyle(.white)
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
        }
    }

    private var controls: some View {
        HStack {
            Text("\(model.captures.count) saved")
                .font(.subheadline).foregroundStyle(.secondary)
            Spacer()
            Button("Prepare Export", systemImage: "archivebox") { model.prepareExport() }
                .disabled(model.captures.isEmpty)
            if let archive = model.exportArchive {
                ShareLink(item: archive) {
                    Label("Share ZIP", systemImage: "square.and.arrow.up")
                }
                .buttonStyle(.borderedProminent)
            }
            Button(role: .destructive) { confirmDeleteAll = true } label: {
                Image(systemName: "trash")
            }
            .disabled(model.captures.isEmpty)
        }
    }

    private var gallery: some View {
        LazyVStack(alignment: .leading, spacing: 12) {
            Text("Saved Captures").font(.title2.bold())
            if model.captures.isEmpty {
                Text("No saved images yet.").foregroundStyle(.secondary)
            }
            ForEach(model.captures) { capture in
                HStack(spacing: 12) {
                    Image(uiImage: capture.image)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFill()
                        .frame(width: 120, height: 90)
                        .clipped()
                        .background(Color.black)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    VStack(alignment: .leading, spacing: 5) {
                        Text("Frame \(capture.frameID)").font(.headline)
                        Text(capture.receivedAt, style: .time)
                            .font(.caption).foregroundStyle(.secondary)
                        Text("PNG + 324×244 Bayer + JSON")
                            .font(.caption2).foregroundStyle(.secondary)
                        HStack {
                            ShareLink(items: capture.exportURLs) {
                                Label("Export", systemImage: "square.and.arrow.up")
                            }
                            Button(role: .destructive) { model.delete(capture) } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                        .font(.caption)
                    }
                    Spacer()
                }
                .padding(10)
                .background(Color(uiColor: .secondarySystemBackground),
                            in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }
}
