# Specification for the target curve designer

The target curve designer presents the smoothed curves of both channels and lets the
user define a target curve for later inversion and final filter calculation.

This feature is split into two responsibilities:

- UI editing and interaction
- target curve and EQ calculation in a separate non-UI module

The UI must not implement the EQ math directly.

## Curve model

The final target curve is defined as:

`target curve = basic curve + sum of all enabled EQ bands`

All curve summation is done in dB.

## Basic curve

The basic curve has three control points:

- a low-frequency starting and anchor point
- a mid point
- a high-frequency ending point

Rules:

- the low and high points are fixed to the edges of the visible band
- the mid point can be positioned anywhere in the band
- all three points can move on the amplitude axis
- the low point acts as the vertical anchor for the whole basic curve
- the mid point changes only its own position
- the high point changes only its own position
- the basic curve uses linear interpolation between the three points

## Equalizer

For the initial version, the editable EQ bands are bell bands.

Rules:

- users can create an arbitrary number of bell bands
- each band has frequency, gain, Q, enabled state, and color
- bell bands support both positive and negative gain
- Q for bell bands is edited only in the sidebar
- the EQ calculation is implemented in a separate calculation module

Reasonable parameter limits should be applied:

- gain: `-12 dB` to `+12 dB`
- Q: `0.3` to `6.0`
- frequency: limited to the visible correction band

## Workflow and UI

The target curve designer is a dedicated page with a graph-first layout:

- a large response graph on the left
- an EQ sidebar on the right

The graph shows:

- smoothed left channel as a thinner subdued line
- smoothed right channel as a thinner subdued line
- the target curve as the strongest line, colored blue

When an EQ band is selected, the graph also shows that band's individual contribution
as a highlighted overlay in the band's own color.

### Basic curve interaction

- the UI starts with a flat basic curve at `0 dB`
- the three basic-curve points are shown as rectangular handles on the curve
- the low point moves vertically only
- the mid point moves horizontally and vertically
- the high point moves vertically only
- dragging any handle updates the target curve immediately

### EQ band interaction

- users add a new EQ band with a `New` button
- users remove the selected EQ band with a `Delete` button
- each EQ band is represented by a handle on the graph
- graph handles edit frequency and gain only
- any change to band parameters updates the target curve immediately
- a `Reset target` action restores the flat basic curve and default band state
- a `Bypass all EQ bands` action temporarily disables all EQ bands for comparison

## Default state

The page starts with:

- a flat basic curve at `0 dB`
- four preconfigured bell bands at `200 Hz`, `500 Hz`, `1000 Hz`, and `5000 Hz`. All inactive.
- each preconfigured band defaults to `0 dB` gain and `Q = 1.0`

Each band must use a distinct strong color with good contrast against the graph
background and against the blue target curve.

## EQ sidebar

The EQ sidebar sits on the right side of the page.

### Band list

At the top of the sidebar is a scrollable list of all EQ bands.

Rules:

- bands are ordered by frequency
- each row shows:
  - enabled checkbox
  - colored round dot corresponding with the band's color in the graph
  - band type
  - center frequency
- selecting a row also selects the corresponding graph handle

### Detail pane

Below the band list is a detail pane for the selected band.

Each parameter is presented with:

- a slider
- a value field

Users can choose their preferred editing method.

Parameter editing rules:

- frequency uses a logarithmic slider
- Q uses a logarithmic slider
- gain uses a linear slider in 0.1 dB increments

## Implementation boundaries

- page layout, controls, and graph interaction belong in the UI module
- target curve composition and EQ math belong in a separate calculation module
- the graph widget receives view data and does not read workspace or application
  state directly

