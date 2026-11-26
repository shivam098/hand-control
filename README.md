# Hand Control

Control the primary display of a Mac or Windows PC with one hand and a webcam.
The camera preview and hand tracking run locally. No account or video upload is required.

## Get started

Download a build from [Releases](https://github.com/shivam098/hand-control/releases).

- **Mac:** The release ZIP is for Apple silicon, macOS 13 or later. Extract `Hand Control.app`, choose its permanent location, then open it. Click **Start Camera**, grant Camera access, and click **Allow Accessibility** for desktop control. The public build is ad-hoc signed, not Developer ID signed or notarized; macOS or device policy may block it. You can also [build from source](Build.txt).
- **Windows:** On Windows 10 or 11 x64, extract the **whole ZIP** and open `Hand Control.exe`. Keep the model and DLL beside it. The executable is not publisher-signed. The Windows build was cross-compiled; live webcam and desktop input have not been verified on Windows hardware.

Select **Calibrate Reach**. Hold your index fingertip at the comfortable upper-left and then lower-right limits of your reach. Minimize the window to control other apps.

| Action | Gesture |
| --- | --- |
| Move | Point with your index finger. |
| Click / double-click | Pinch thumb and index, then release; repeat quickly for a double-click. |
| Drag and drop | Hold the index pinch until **Dragging**, move your whole hand, then open the pinch. |
| Right-click | Pinch thumb and middle finger, then release. |
| Scroll | Extend only index and middle fingers and move vertically. |
| Pause / resume | Make a fist; open your hand and separate your fingertips to resume. |

If macOS shows **Preview only** despite an enabled Accessibility entry, first quit and reopen the same app. The expected status is **Desktop control enabled**. Ad-hoc rebuilds can invalidate an earlier grant even if the switch is still on. See [getting started](Getting%20Started.txt) for the app-specific recovery steps and [build instructions](Build.txt) for persistent local signing.

## Development

`Sources/GestureCore` is shared by the Swift/AppKit Mac frontend and the C++ Windows frontend. The Mac uses AVFoundation and Vision; Windows uses Media Foundation and MediaPipe. Run `swift test` on a Mac. Set `HAND_CONTROL_TEST_VIDEO` to a local hand video to include the optional replay test. Reference footage is not distributed.

Windows runtime and model details are in [third-party notices](Windows/NOTICE.txt).
