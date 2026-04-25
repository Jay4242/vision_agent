#!/usr/bin/env python3

import subprocess
import tempfile
import os
import time
from pynput import keyboard
from PIL import ImageGrab
import threading
import sys

# --- Configuration ---
HOTKEY = {keyboard.Key.ctrl, keyboard.Key.shift, keyboard.Key.f12}
AUDIO_DURATION = 10  # seconds
WHISPER_SCRIPT = './whisper-remote.py'
VISION_SCRIPT = './llm-python-vision.py'
TEMP_DIR = '/dev/shm/vision_agent'

# --- State ---
current_keys = set()
processing = False
processing_lock = threading.Lock()

def get_transcription(audio_path):
    """Calls whisper-remote.py and returns the transcribed text."""
    print("Transcribing audio...")
    try:
        # whisper-remote.py expects a video file, but works with audio too
        subprocess.run([sys.executable, WHISPER_SCRIPT, audio_path], check=True, capture_output=True, text=True)
        
        srt_path = os.path.splitext(audio_path)[0] + ".srt"
        if not os.path.exists(srt_path):
            print("Error: SRT file not created.")
            return ""

        with open(srt_path, 'r') as f:
            lines = f.readlines()
        
        # Extract text from SRT, ignoring timestamps and sequence numbers
        text_lines = [line.strip() for line in lines if not line.strip().isdigit() and '-->' not in line and line.strip()]
        transcription = " ".join(text_lines)
        
        os.remove(srt_path) # Clean up SRT file
        print(f"Transcription: {transcription}")
        return transcription
    except subprocess.CalledProcessError as e:
        print(f"Error during transcription: {e.stderr}")
        return ""
    except FileNotFoundError:
        print(f"Error: Could not find '{WHISPER_SCRIPT}'.")
        return ""

def get_vision_response(image_path, prompt):
    """Calls llm-python-vision.py and returns the response."""
    print("Getting vision response...")
    try:
        result = subprocess.run(
            [sys.executable, VISION_SCRIPT, image_path, prompt],
            check=True,
            capture_output=True,
            text=True
        )
        print("Vision model response:")
        print(result.stdout.strip())
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Error during vision processing: {e.stderr}")
        return ""
    except FileNotFoundError:
        print(f"Error: Could not find '{VISION_SCRIPT}'.")
        return ""

def record_audio(path, duration):
    """Records audio from the default microphone using ffmpeg."""
    print("Get ready to record. Starting in...")
    for i in range(3, 0, -1):
        print(f"{i}...", flush=True)
        try:
            # Play a short beep using ffplay (part of ffmpeg)
            subprocess.run(
                ['ffplay', '-f', 'lavfi', '-i', 'sine=frequency=1000:duration=0.2', '-autoexit', '-nodisp'],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True
            )
        except (FileNotFoundError, subprocess.CalledProcessError):
            # Fallback to terminal bell if ffplay fails or isn't found
            print('\a', end='', flush=True)
        time.sleep(0.8) # Sleep for less than a second to account for beep duration
    print("RECORDING!")

    # Using 'pulse' for PipeWire/PulseAudio, 'alsa' for ALSA. 'default' might work.
    # Use `pactl list sources` or `arecord -l` to find your input device.
    command = [
        'ffmpeg',
        '-f', 'pulse',  # Use 'avfoundation' on macOS, 'dshow' on Windows
        '-i', 'default', # Use device name if 'default' fails
        '-t', str(duration),
        '-ac', '1',
        '-ar', '16000',
        '-c:a', 'pcm_s16le',
        '-y',
        path
    ]
    try:
        # Using Popen to allow for a visual indicator during recording
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        # Simple visual indicator
        print(f"[{duration}s] ", end="", flush=True)
        for _ in range(duration):
            print("=", end="", flush=True)
            time.sleep(1)
        print(">")

        stdout, stderr = process.communicate() # Wait for ffmpeg to finish

        if process.returncode != 0:
            print(f"\nError recording audio: {stderr}")
            return False

        print(f"Audio saved to {path}")
        return True
    except FileNotFoundError:
        print("Error: 'ffmpeg' not found. Please ensure it is installed and in your PATH.")
        return False

def take_screenshot(path):
    """Takes a screenshot and saves it to the given path."""
    print("Taking screenshot...")
    try:
        screenshot = ImageGrab.grab()
        screenshot.save(path, 'PNG')
        print(f"Screenshot saved to {path}")
        return True
    except Exception as e:
        print(f"Error taking screenshot: {e}")
        return False

def vision_agent_task():
    """The main task to be run when the hotkey is pressed."""
    global processing
    with processing_lock:
        if processing:
            print("Already processing a request.")
            return
        processing = True
    
    print("\n--- Hotkey Activated! Starting Vision Agent ---")
    
    audio_path = os.path.join(TEMP_DIR, 'capture.wav')
    image_path = os.path.join(TEMP_DIR, 'capture.png')

    try:
        # 1. Take Screenshot
        if not take_screenshot(image_path):
            return # Exit if screenshot fails

        # 2. Record Audio
        if not record_audio(audio_path, AUDIO_DURATION):
            return # Exit if recording fails

        # 3. Transcribe Audio
        command = get_transcription(audio_path)
        if not command:
            print("Could not get transcription, using default prompt.")
            command = "What is in this image?"

        # 4. Get Vision Response
        get_vision_response(image_path, command)

    finally:
        # Files in TEMP_DIR are overwritten, no cleanup needed.
        with processing_lock:
            processing = False
        print("--- Vision Agent Finished ---")


def on_press(key):
    if key in HOTKEY:
        current_keys.add(key)
        if all(k in current_keys for k in HOTKEY):
            # Use a thread to not block the listener
            threading.Thread(target=vision_agent_task).start()

def on_release(key):
    try:
        current_keys.remove(key)
    except KeyError:
        pass

def main():
    print("Vision Agent is running. Press Ctrl+Shift+F12 to activate.")
    
    # Create the temporary directory in /dev/shm
    os.makedirs(TEMP_DIR, exist_ok=True)
    print(f"Using temporary directory: {TEMP_DIR}")

    # Ensure scripts are executable
    if not os.access(WHISPER_SCRIPT, os.X_OK):
        os.chmod(WHISPER_SCRIPT, 0o755)
    if not os.access(VISION_SCRIPT, os.X_OK):
        os.chmod(VISION_SCRIPT, 0o755)
        
    # Check for dependencies
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("WARNING: `ffmpeg` command not found. Audio recording will fail.")
        print("Please install ffmpeg and ensure it's in your system's PATH.")

    with keyboard.Listener(on_press=on_press, on_release=on_release) as listener:
        listener.join()

if __name__ == "__main__":
    # Check for display server environment for screenshot capability
    if os.environ.get('DISPLAY') is None:
        print("ERROR: No display server found (e.g., X11, Wayland). Cannot take screenshots.")
        print("This script must be run in a graphical environment.")
        sys.exit(1)
        
    try:
        from pynput import keyboard
        from PIL import ImageGrab
    except ImportError as e:
        print(f"ERROR: Missing dependency -> {e}")
        print("Please install required libraries: pip install pynput pillow")
        sys.exit(1)

    main()
