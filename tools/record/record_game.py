"""Records OpenTIE with its audio, from launch until the game closes.

The screen is captured with ffmpeg (ddagrab, NVENC when there is one) while
the game writes its own audio mix (AERON_RECORD_AUDIO, aeron/src/audio.c).
When the game closes, the recording stops, the audio is lined up with the
video by their wall-clock start times, and both are cut by --trim seconds at
the start and the end into one MP4 in --out-dir.

    python tools/record/record_game.py [--config Release] [--trim 2] -- [game arguments]
"""

import argparse
import ctypes
import ctypes.wintypes
import datetime
import json
import os
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME_PATH = ["C:/msys64/ucrt64/bin", "D:/development/OpenTIE-deps/prefix/bin"]
WINGET_FFMPEG = os.path.expandvars(
    r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe"
)


def log(message):
    print("[record] " + message, flush=True)


def ffmpeg_has(ffmpeg, listing, name):
    try:
        out = subprocess.run([ffmpeg, "-hide_banner", listing], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return any(line.split()[1:2] == [name] for line in out.stdout.splitlines() if line.strip())


def find_ffmpeg():
    """An ffmpeg with the ddagrab screen grabber, and whether it has NVENC."""
    candidates = []
    if os.environ.get("FFMPEG"):
        candidates.append(os.environ["FFMPEG"])
    if os.path.isdir(WINGET_FFMPEG):
        for entry in sorted(os.listdir(WINGET_FFMPEG), reverse=True):
            candidates.append(os.path.join(WINGET_FFMPEG, entry, "bin", "ffmpeg.exe"))
    found = shutil.which("ffmpeg")
    if found:
        candidates.append(found)
    for ffmpeg in candidates:
        if os.path.isfile(ffmpeg) and ffmpeg_has(ffmpeg, "-filters", "ddagrab"):
            return ffmpeg, ffmpeg_has(ffmpeg, "-encoders", "h264_nvenc")
    raise SystemExit("no ffmpeg with the ddagrab screen grabber found (set FFMPEG)")


def ffprobe_for(ffmpeg):
    probe = os.path.join(os.path.dirname(ffmpeg), "ffprobe" + (".exe" if ffmpeg.endswith(".exe") else ""))
    return probe if os.path.isfile(probe) else shutil.which("ffprobe")


def duration_of(ffprobe, path):
    out = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True, check=True)
    return float(out.stdout.strip())


class ScreenRecorder:
    """ffmpeg grabbing a monitor into an MKV. Its start on the wall clock is
    taken from its progress reports: the earliest (report time - recorded
    time) is the closest to when the first frame was grabbed."""

    def __init__(self, ffmpeg, nvenc, monitor, fps, path):
        if nvenc:
            encode = ["-c:v", "h264_nvenc", "-preset", "p4", "-tune", "ll", "-rc", "vbr", "-cq", "18",
                      "-b:v", "0", "-bf", "0", "-g", str(fps * 2)]
        else:
            encode = ["-vf", "hwdownload,format=bgra", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18",
                      "-pix_fmt", "yuv420p"]
        command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
                   "-stats_period", "0.1", "-f", "lavfi", "-i",
                   "ddagrab=output_idx=%d:framerate=%d:draw_mouse=0" % (monitor, fps)] + encode + ["-y", path]
        self.start_ns = None
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.reader = threading.Thread(target=self._read_progress, daemon=True)
        self.reader.start()

    def _read_progress(self):
        for line in self.process.stdout:
            now = time.time_ns()
            key, _, value = line.strip().partition("=")
            if key == "out_time_us" and value.isdigit() and int(value) > 0:
                start = now - int(value) * 1000
                if self.start_ns is None or start < self.start_ns:
                    self.start_ns = start

    def wait_started(self, timeout):
        deadline = time.time() + timeout
        while self.start_ns is None and time.time() < deadline:
            if self.process.poll() is not None:
                raise SystemExit("ffmpeg stopped before recording anything")
            time.sleep(0.05)
        if self.start_ns is None:
            raise SystemExit("ffmpeg did not start recording")

    def stop(self):
        try:
            self.process.stdin.write("q")
            self.process.stdin.flush()
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=5)


class WindowTracker:
    """Follows the game window's client area on the primary monitor, in
    physical pixels, so the recording can be cropped to it."""

    def __init__(self, pid):
        self.pid = pid
        self.rect = None
        self.done = threading.Event()
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except (AttributeError, OSError):
            pass
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _find(self):
        user32 = ctypes.windll.user32
        best = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
        def visit(hwnd, _):
            pid = ctypes.wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.pid and user32.IsWindowVisible(hwnd) and not user32.IsIconic(hwnd):
                client = ctypes.wintypes.RECT()
                user32.GetClientRect(hwnd, ctypes.byref(client))
                origin = ctypes.wintypes.POINT(0, 0)
                user32.ClientToScreen(hwnd, ctypes.byref(origin))
                w, h = client.right, client.bottom
                if w > 64 and h > 64 and (not best or w * h > best[0][2] * best[0][3]):
                    best[:] = [(origin.x, origin.y, w, h)]
            return True

        user32.EnumWindows(visit, 0)
        return best[0] if best else None

    def _run(self):
        while not self.done.wait(0.5):
            found = self._find()
            if found:
                self.rect = found

    def stop(self):
        self.done.set()
        self.thread.join(timeout=2)
        return self.rect


def crop_filter(rect, width, height):
    """The crop to the game window, or "" for a window covering the screen
    or one partly off it."""
    if not rect:
        return ""
    x, y, w, h = rect
    x, y = max(x, 0), max(y, 0)
    w, h = min(w, width - x) & ~1, min(h, height - y) & ~1
    if w <= 0 or h <= 0 or (w >= width - 1 and h >= height - 1):
        return ""
    return ",crop=%d:%d:%d:%d" % (w, h, x, y)


def finish(ffmpeg, nvenc, video, audio, out, trim, rect):
    """Crops to the game window, lines the audio up with the video and cuts
    `trim` seconds off both ends."""
    ffprobe = ffprobe_for(ffmpeg)
    length = duration_of(ffprobe, video)
    size = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                           "-of", "csv=p=0", video], capture_output=True, text=True, check=True).stdout
    width, height = (int(v) for v in size.strip().split(",")[:2])
    crop = crop_filter(rect, width, height)
    if crop:
        log("cropping to the game window: %dx%d at %d,%d" % (rect[2], rect[3], rect[0], rect[1]))
    end = length - trim
    if end <= trim:
        log("the recording is %.1f s long: too short to cut %g s off each end; kept whole" % (length, trim))
        start, end = 0.0, length
    else:
        start = trim
    inputs = ["-i", video]
    graph = "[0:v]trim=start=%.6f:end=%.6f,setpts=PTS-STARTPTS%s[v]" % (start, end, crop)
    maps = ["-map", "[v]"]
    if audio:
        pcm, info = audio
        offset = (info["start_epoch_ns"] - info["video_start_ns"]) / 1e9
        log("audio starts %.3f s after the video" % offset)
        inputs += ["-f", "s16le", "-ar", str(info["rate"]), "-ac", str(info["channels"]), "-i", pcm]
        if offset >= 0:
            place = "adelay=delays=%d:all=1" % round(offset * 1000)
        else:
            place = "atrim=start=%.6f,asetpts=PTS-STARTPTS" % -offset
        graph += ";[1:a]%s,apad,atrim=start=%.6f:end=%.6f,asetpts=PTS-STARTPTS[a]" % (place, start, end)
        maps += ["-map", "[a]", "-c:a", "aac", "-b:a", "192k"]
    if nvenc:
        encode = ["-c:v", "h264_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", "20", "-b:v", "0"]
    else:
        encode = ["-c:v", "libx264", "-preset", "medium", "-crf", "20"]
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + inputs + ["-filter_complex", graph] + maps + \
        encode + ["-pix_fmt", "yuv420p", "-movflags", "+faststart", out]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="Release", help="build/<config>/OpenTIE.exe (default Release)")
    parser.add_argument("--trim", type=float, default=2.0, help="seconds cut off the start and the end")
    parser.add_argument("--monitor", type=int, default=0, help="the monitor to record (0 = primary)")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--out-dir", default=os.path.join(ROOT, "recordings"))
    parser.add_argument("--whole-screen", action="store_true", help="don't crop to the game window")
    parser.add_argument("--open-folder", action="store_true", help="open the folder, the video selected, when done")
    parser.add_argument("game_args", nargs=argparse.REMAINDER, help="arguments for the game, after --")
    args = parser.parse_args()
    game_args = args.game_args[1:] if args.game_args[:1] == ["--"] else args.game_args

    game_dir = os.path.join(ROOT, "build", args.config)
    game = os.path.join(game_dir, "OpenTIE.exe")
    if not os.path.isfile(game):
        raise SystemExit("no game at " + game)
    ffmpeg, nvenc = find_ffmpeg()
    os.makedirs(args.out_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = os.path.join(args.out_dir, "OpenTIE-" + stamp)
    video, pcm, out = base + ".raw.mkv", base + ".audio.pcm", base + ".mp4"
    sidecar = pcm + ".json"

    log("recording monitor %d with %s (%s)" % (args.monitor, ffmpeg, "NVENC" if nvenc else "x264"))
    screen = ScreenRecorder(ffmpeg, nvenc, args.monitor, args.fps, video)
    try:
        screen.wait_started(15)
        env = dict(os.environ)
        env["PATH"] = os.pathsep.join(GAME_PATH + [env.get("PATH", "")])
        env.setdefault("SDL_ASSERT", "always_ignore")
        env["AERON_RECORD_AUDIO"] = pcm
        log("starting " + " ".join([game] + game_args))
        process = subprocess.Popen([game] + game_args, cwd=game_dir, env=env)
        tracker = WindowTracker(process.pid) if args.monitor == 0 and not args.whole_screen else None
        code = process.wait()
        rect = tracker.stop() if tracker else None
        log("the game closed (exit code %d); stopping the recording" % code)
    finally:
        screen.stop()

    audio = None
    if os.path.isfile(sidecar) and os.path.isfile(pcm):
        with open(sidecar) as f:
            info = json.load(f)
        info["video_start_ns"] = screen.start_ns
        audio = (pcm, info)
    else:
        log("the game recorded no audio; the video will be silent")
    finish(ffmpeg, nvenc, video, audio, out, args.trim, rect)
    for path in (video, pcm, sidecar):
        if os.path.isfile(path):
            os.remove(path)
    log("saved " + out)
    if args.open_folder:
        subprocess.Popen('explorer /select,"%s"' % os.path.normpath(out))


if __name__ == "__main__":
    sys.exit(main())
