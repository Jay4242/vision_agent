# Vision Agent

A desktop AI assistant for Linux/X11 that captures your screen and voice to answer your questions.

This C program listens for a global keybinding (`Ctrl+Shift+F12`). When activated, it:
1.  Takes a screenshot of the entire desktop.
2.  Records a 5-second audio clip from the default microphone.
3.  Sends the screenshot to a vision language model (LLM) to get a description.
4.  Sends the audio clip to a Whisper server to get a transcription.
5.  Combines the visual description and the audio transcription into a final prompt for a text-based LLM.
6.  Prints the final answer to the console.

## Dependencies

To build and run this program, you need the following libraries and command-line tools installed.

### Build-time Libraries (Development Headers)
- **libX11**: For creating the global keybinding.
- **libcurl**: For making HTTP requests to the AI/ML APIs.
- **cJSON**: For constructing and parsing JSON payloads.
- **OpenSSL (libssl, libcrypto)**: For Base64 encoding the screenshot.

**Installation on Debian/Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install -y libx11-dev libcurl4-openssl-dev libcjson-dev libssl-dev
```

**Installation on Fedora/RHEL:**
```bash
sudo dnf install -y libX11-devel libcurl-devel cjson-devel openssl-devel
```

### Runtime Tools
- **scrot**: A command-line utility for capturing screenshots.
- **ffmpeg**: A command-line utility for recording audio.

**Installation on Debian/Ubuntu:**
```bash
sudo apt-get install -y scrot ffmpeg
```

**Installation on Fedora/RHEL:**
```bash
sudo dnf install -y scrot ffmpeg
```

## Compilation

With all dependencies installed, compile the program using `gcc`:

```bash
gcc main.c -o vision_agent -lX11 -lcurl -lcjson -lssl -lcrypto
```

This will create an executable file named `vision_agent`.

## Configuration

The program is hardcoded to use the following API endpoints and models:
- **LLM Server URL**: `http://localhost:9090/v1`
- **Whisper Server URL**: `http://localhost:9191/inference`
- **Vision Model**: `gemma3:4b-it-q8_0`
- **Text Model**: `gemma-2-2b-it-q8_0`

Ensure you have compatible OpenAI-style and Whisper servers running and accessible at these addresses before running the application.

## Usage

1.  Compile the program as described above.
2.  Run the executable from your terminal: `./vision_agent`
3.  The program will run in the foreground and print a confirmation that it has started.
4.  Press `Ctrl+Shift+F12` anywhere in your desktop session to trigger the agent.
5.  The agent will perform the capture, transcription, and analysis, then print the final result to the terminal where it was launched.

Temporary screenshot (`.png`) and audio (`.wav`) files are created in `/dev/shm/` and are deleted immediately after use.
