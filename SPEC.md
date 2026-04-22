# SPECIFICATION
## Objective
Build a portable desktop application for creating FIR audio convolving filters that can be imported into Roon.
The user can experiment with the target room response via a graphical editor.
The user can create workspaces for individual room configurations. For example when speakers are moved.
The workspaces keep all settings and data in files. Workspaces manage multiple variants of filters. 
The user can export ROON configurations for a variant individually.

## Step-by-step process overview
- Import microphone calibration text file.
- Align microphone with speakers.
- Measure room response (log sweep) with left and right channels.
- Smoothen room response using a psychoacoustic model.
- Graphically design a target frequency response common for both channels.
- Calculate the inversion curves for both channels.
- Create the filters for both channels.
- Simulate the filters in the UI and show the f-response and group delay.
- Create a ROON config as ZIP.

## Process details
### Microphone calibration
The calibration must be applied to all measurements.
The .txt has a common frequency / dB format. Here is a sample:
```
0.000 -3.560
11.719 -3.560
23.438 -1.586
35.156 -0.838
46.875 -0.577
58.594 -0.467
70.313 -0.436
```

### General settings for all measurements
The app uses ASIO. The UI configuration contains:
- soundcard driver
- mic input channel
- left output channel
- right output channel
- sample rate (44.1kHz default)
- button to open asio control panel
- All configuration parameters are stored as a JSON file in the workspace.

### Align microphone with speakers
The goal is to position the mike exactly symmetrically with the speakers.
The app sends high frequency pulses to both channels and measures the runtime.
The UI shows an arrow to the left or right, depending on which channel is farther away from the mike.
The UI shows the graphical representation of the channel impulses on the time and amplitude axis for a visual feedback
of the alignment. The left channel is green, the right channel is red.
Parameters:
- Output volume slider in dB. muted to the far left, start with -60dB. 0dB at the far right. Default is the safe center at -30dB.

### Room response measurement
The app runs a sweep for both channels and measures the room response.
It uses the ASIO settings and the microphone calibration.
Parameters are:
- general ASIO settings (see above)
- fade-in in seconds (default 0.5)
- fade-out in seconds (default 0.1)
- sweep duration in seconds (default 60)
- sweep start frequency in Hz (default 20Hz)
- sweep end frequency in Hz (default that makes sense with the sample rate)
- target length and lead in in samples (default 65536 and 6000)

Pulses are normalized to 1.0.
Peak optimize the sweep.
Show the current sweep frequency, current amplitude and max amplitude. 
Autostore the logsweep in files. Choose a popular format to have some compatibility.
In the UI show the result in a graph in the frequency/amplitude domain.
Show both channels in one graph. Green for left, red for right.

### Target curve design
To be done.

### Working with the workspace
A workspace is a folder under a common parent folder.
A workspace contains exactly one measurement.
A workspace can contain multiple independent, named target curves.
A variant is a configuration of a target curve and the calculated inversion curve and filters.
A variant can be exported as a ROON config.
Each variant is stored in subdirectory under the workspace.

## UI design
The UI should be simple, modern, and intuitive. 
Use a tab for each process step.
Align the basic work areas along the process steps.
Stretch the content horizontally to use all available space.
Steps that have a fixed number of form elements are not resizable.
Store the UI configuration in a file in the workspace directory.

Put the common asio settings and microphone calibration in a settings popup. This is used rarely.

The config is located as "settings" in the file menu.

The workspace operations are located in the file menu. Allow for:
- Recent with a submenu for recent workspaces
- Open...
- Save... with Shortcut Ctrl-S
- Save As...
- New...
No further shortcuts.
Open with the last workspace.

## Autosave feature
Everything is autosaved except the target curve.

