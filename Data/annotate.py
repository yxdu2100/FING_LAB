import os
import sys
import csv
import argparse
from pathlib import Path

# Qt's FFmpeg backend can select macOS VideoToolbox automatically.  Some
# VideoToolbox H.264 decoders fail for this source (-12909), so use the
# software decoder by default.  Users may still override this before launch.
os.environ.setdefault("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",")
os.environ.setdefault("QT_DISABLE_HW_TEXTURES_CONVERSION", "1")

from PyQt6.QtCore import Qt, QUrl
from PyQt6.QtGui import QFont, QKeyEvent
from PyQt6.QtWidgets import (
    QApplication,
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
)
from PyQt6.QtMultimedia import QMediaPlayer, QAudioOutput
from PyQt6.QtMultimediaWidgets import QVideoWidget


LABELS = {
    1: "ring",
    2: "glasses",
    3: "phone",
}

# Amount to seek when the left/right arrow keys are pressed.
SEEK_STEP_SECONDS = 5
UI_TEXT_POINT_SIZE = 11


class VideoAnnotationTool(QWidget):
    def __init__(self, video_path: str):
        super().__init__()

        self.video_path = Path(video_path).resolve()

        if not self.video_path.exists():
            raise FileNotFoundError(
                f"Video file does not exist: {self.video_path}"
            )

        if self.video_path.suffix.lower() != ".mp4":
            raise ValueError(
                f"Expected an .mp4 file, got: {self.video_path}"
            )

        # CSV stored beside the video
        self.csv_path = self.video_path.with_name(
            f"{self.video_path.stem}_annotations.csv"
        )

        self.session_start = None
        self.waiting_for_label = False

        self.init_csv()
        self.init_ui()
        self.init_player()

    def init_csv(self):
        """
        Create CSV if it doesn't already exist.
        """
        if not self.csv_path.exists():
            with open(self.csv_path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow([
                    "start_time",
                    "end_time",
                    "label",
                ])

    def init_ui(self):
        self.setWindowTitle(
            f"Video Annotation - {self.video_path.name}"
        )

        self.resize(1200, 800)

        layout = QVBoxLayout()

        # -------------------------
        # Video
        # -------------------------

        self.video_widget = QVideoWidget()
        layout.addWidget(self.video_widget)

        # -------------------------
        # Status
        # -------------------------

        self.status_label = QLabel(
            "SPACE: start annotation    ←/→: seek 5 seconds"
        )

        self.status_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.status_label.setFont(self.ui_font())

        layout.addWidget(self.status_label)

        # -------------------------
        # Annotation buttons
        # -------------------------

        button_layout = QHBoxLayout()

        self.ring_button = QPushButton("1 - Ring")
        self.glasses_button = QPushButton("2 - Glasses")
        self.phone_button = QPushButton("3 - Phone")

        for button in (
            self.ring_button,
            self.glasses_button,
            self.phone_button,
        ):
            button.setFont(self.ui_font())

        self.ring_button.clicked.connect(
            lambda: self.choose_label("ring")
        )

        self.glasses_button.clicked.connect(
            lambda: self.choose_label("glasses")
        )

        self.phone_button.clicked.connect(
            lambda: self.choose_label("phone")
        )

        button_layout.addWidget(self.ring_button)
        button_layout.addWidget(self.glasses_button)
        button_layout.addWidget(self.phone_button)

        layout.addLayout(button_layout)

        self.setLayout(layout)

        # Keep keyboard controls on the main window instead of leaving focus on
        # one of the label buttons after it is clicked.
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        for button in (
            self.ring_button,
            self.glasses_button,
            self.phone_button,
        ):
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        self.enable_label_buttons(False)

    @staticmethod
    def ui_font():
        """Use a compact font for controls below the video."""
        font = QFont()
        font.setPointSize(UI_TEXT_POINT_SIZE)
        return font

    def init_player(self):
        self.player = QMediaPlayer()

        self.audio_output = QAudioOutput()

        self.player.setAudioOutput(self.audio_output)
        self.player.setVideoOutput(self.video_widget)

        self.audio_output.setVolume(1.0)

        self.player.setSource(
            QUrl.fromLocalFile(str(self.video_path))
        )

        # Start automatically
        self.player.play()

    def enable_label_buttons(self, enabled: bool):
        self.ring_button.setEnabled(enabled)
        self.glasses_button.setEnabled(enabled)
        self.phone_button.setEnabled(enabled)

    def current_time(self):
        """
        Return video position in seconds.
        QMediaPlayer.position() returns milliseconds.
        """
        return self.player.position() / 1000.0

    def seek_relative(self, seconds: float):
        """Move playback by ``seconds``, without seeking past either end."""
        target_ms = self.player.position() + int(seconds * 1000)
        duration_ms = self.player.duration()

        if duration_ms > 0:
            target_ms = min(target_ms, duration_ms)
        target_ms = max(target_ms, 0)

        self.player.setPosition(target_ms)
        target_seconds = target_ms / 1000.0

        if self.session_start is None:
            self.status_label.setText(
                f"Position: {target_seconds:.3f}s\n"
                "SPACE: start annotation    ←/→: seek 5 seconds"
            )
        else:
            self.status_label.setText(
                f"Recording session...\n"
                f"Start: {self.session_start:.3f}s\n"
                f"Current position: {target_seconds:.3f}s\n"
                "Press SPACE to end session"
            )

    def start_session(self):
        self.session_start = self.current_time()

        self.status_label.setText(
            f"Recording session...\n"
            f"Start: {self.session_start:.3f}s\n"
            f"Press SPACE to end session"
        )

        print(
            f"[SESSION START] {self.session_start:.3f}s"
        )

    def stop_session(self):
        if self.session_start is None:
            return

        self.session_end = self.current_time()

        self.player.pause()

        self.waiting_for_label = True

        self.status_label.setText(
            f"Session: "
            f"{self.session_start:.3f}s → "
            f"{self.session_end:.3f}s\n\n"
            f"Choose label:\n"
            f"1 = Ring   "
            f"2 = Glasses   "
            f"3 = Phone"
        )

        self.enable_label_buttons(True)

        print(
            f"[SESSION END] {self.session_end:.3f}s"
        )

    def choose_label(self, label: str):
        if not self.waiting_for_label:
            return

        self.save_annotation(
            self.session_start,
            self.session_end,
            label,
        )

        print(
            f"[ANNOTATION] "
            f"{self.session_start:.3f}s -> "
            f"{self.session_end:.3f}s : "
            f"{label}"
        )

        self.session_start = None
        self.session_end = None
        self.waiting_for_label = False

        self.enable_label_buttons(False)

        self.status_label.setText(
            f"Saved label: {label}\n"
            f"Press SPACE to start next session"
        )

        # Resume playback automatically
        self.player.play()

    def save_annotation(
        self,
        start_time: float,
        end_time: float,
        label: str,
    ):
        with open(
            self.csv_path,
            "a",
            newline="",
        ) as f:
            writer = csv.writer(f)

            writer.writerow([
                f"{start_time:.3f}",
                f"{end_time:.3f}",
                label,
            ])

    def keyPressEvent(self, event: QKeyEvent):

        # Ignore key repeat caused by holding space
        if event.isAutoRepeat():
            return

        key = event.key()

        # -------------------------
        # Waiting for label
        # -------------------------

        if self.waiting_for_label:

            if key == Qt.Key.Key_1:
                self.choose_label("ring")

            elif key == Qt.Key.Key_2:
                self.choose_label("glasses")

            elif key == Qt.Key.Key_3:
                self.choose_label("phone")

            return

        # -------------------------
        # SPACE session control
        # -------------------------

        if key == Qt.Key.Key_Left:
            self.seek_relative(-SEEK_STEP_SECONDS)
            return

        if key == Qt.Key.Key_Right:
            self.seek_relative(SEEK_STEP_SECONDS)
            return

        if key == Qt.Key.Key_Space:

            # No active annotation session
            if self.session_start is None:
                self.start_session()

            # Active session exists
            else:
                self.stop_session()

            return

        super().keyPressEvent(event)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Video annotation tool"
    )

    parser.add_argument(
        "--data_path",
        required=True,
        type=str,
        help="Path to the .mp4 video file",
    )

    return parser.parse_args()


def main():
    args = parse_args()

    app = QApplication(sys.argv)

    window = VideoAnnotationTool(
        args.data_path
    )

    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
