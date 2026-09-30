# Oculi

A realistic animated ASCII eye for your terminal, written in a single C++ file with no dependencies.

![Oculi preview](assets/eye.gif)

The iris moves and shifts its gaze, the pupil breathes, light reflects in the eye, and it blinks now and then. Runs in a black-and-white or a green "hacker" mode.

## Features

- Moving iris with fine fibres, breathing pupil, glint and soft reflection
- Natural gaze that jumps to a new spots and eases into them
- Random blinks (sometimes a quick double blink)
- Grayscale mode (default) and green mode
- Centered in your terminal, cleans up properly when you quit

## Requirements

- A C++ compiler, for example `g++` (MinGW on Windows, GCC or Clang on Linux/macOS)
- A terminal with 256-color support:
  - Windows 10 or newer (Command Prompt, PowerShell, Windows Terminal or the VS Code terminal)
  - Any modern Linux or macOS terminal
- A terminal window at least **61 columns x 23 rows**

No other libraries or files are needed.

## Download

Clone the repository:

```
git clone https://github.com/hasheramin5-cyber/Oculi.git
cd Oculi
```

Or click **Code > Download ZIP** on GitHub, extract it, and open a terminal in that folder.

## Build

```
g++ eye.cpp -o eye
```

## Run

### Windows (Command Prompt)

```
eye
eye green
```

### Windows (PowerShell)

```
.\eye
.\eye green
```

### Linux / macOS

```
./eye
./eye green
```

Press **Ctrl+C** to quit. The terminal is restored automatically.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| `'g++' is not recognized` | Install MinGW (or another compiler) and add its `bin` folder to your `PATH`, then reopen the terminal. |
| `eye` is not recognized in PowerShell | Use `.\eye` instead of `eye`. |
| Colors look wrong or the picture is garbled | Use Windows Terminal or the VS Code terminal, or make sure your Windows is up to date. |
| Picture is cut off | Make the terminal window bigger (at least 61x23) or reduce the font size. |

## How it works

Every frame is drawn from math, not from stored images. Each cell of a 61x23 grid gets a brightness value, which is turned into a character (from `.` to `@`) and a color using ANSI escape codes. The whole frame is written in one go to keep the animation smooth.

## License

This project is licensed under the [MIT License](LICENSE).
