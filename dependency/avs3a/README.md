# AVS3 audio decoding and speaker output

The reference decoder returns interleaved sound-bed and object signals. These
signals are not all speaker channels. `speaker_renderer` maps a selected
mixed-content presentation into its existing sound-bed layout, using libear's
ITU-R BS.2127 gain calculator. The FFmpeg wrapper then publishes the actual PCM
layout, and downstream resampling negotiates the device's output channels.

The FFmpeg `content_index` decoder option selects a zero-based reference in
`audioProgramme.refContentIdx`. The default is the first presentation (`0`). This
does not infer a language or mix all alternative presentations. DASH Preselection
UI and language selection are not provided by this decoder option.

The current speaker path supports the verified stereo, 5.1 and 7.1.4 beds,
object position and extent, gain, mute, and interpolation unless `jumpPosition`
is set. Cartesian positions and extents use libear's polar conversion. Static
metadata must be available after initialization; subsequent frames and seeks
within the same stream reuse it. Seek clears gain interpolation history. A new
speaker layout requires new programme metadata. Rendering happens for each
decoded frame, before object signals can
be removed from the PCM buffer.

This is not a complete Audio Vivid renderer. Diffuse objects, channel lock,
divergence, screen-relative or head-locked objects, VR environments,
complementary object groups, custom sound-bed positions and unsupported layouts
return an explicit error.
Pure-object and HOA decoding retain the existing decoder path; they are
not covered by the mixed-content speaker renderer. The reference decoder's
existing object-count and bitstream limits still apply.

`libear` and Boost sources are pinned in CMake. Eigen and xsimd use libear's
pinned submodule revisions. The resulting dependency set links `arcdav3a` and
`ear` statically; Android uses the same `libc++_shared` as the other native
libraries. FFmpeg must enable version 3 licensing for libear's Apache 2.0 code.
Installed license notices are under `share/licenses/arcdav3a`.

For offline builds, provide `FETCHCONTENT_SOURCE_DIR_ARCDAV3A_BOOST` and
`FETCHCONTENT_SOURCE_DIR_ARCDAV3A_EAR`. The latter must include its Eigen and
xsimd submodules. `ARCDAV3A_TESTS=ON` builds `speaker_renderer_test`; when cross
compiling, run that executable on the matching target device.
