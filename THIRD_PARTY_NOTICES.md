# Third-party notices

expo-juce's own code is MIT (see `LICENSE`). The package also ships third-party code that keeps
its own licence. Anything you build with expo-juce compiles that code into your app, so your app
has to meet those terms too.

## JUCE 8.0.1 (`ios/juce_modules/`)

JUCE is dual licensed. You choose one:

- **The JUCE licence** (the JUCE 8 End User Licence Agreement, <https://juce.com/legal/juce-8-licence/>).
  This is the route for a closed-source app. JUCE sells Starter, Indie and Pro tiers; Starter is
  free below a revenue limit. The limits, and what counts as revenue for an individual or a
  company, are set by the current EULA, so read it there rather than relying on a summary.
- **AGPLv3** (<https://www.gnu.org/licenses/agpl-3.0.en.html>). Your whole app must then be
  released under AGPLv3, with its complete source, to everyone who receives it.

expo-juce doesn't hold or grant a JUCE licence on your behalf. Using expo-juce under MIT doesn't
change the terms that apply to JUCE.

The vendored tree is JUCE 8.0.1 with the small patches in `src/scripts/juce-patches.cjs`. Each
JUCE file carries its own licence header.

### Code bundled inside JUCE

- **Oboe** (`juce_audio_devices/native/oboe/`): Apache License 2.0, Copyright The Android Open
  Source Project. Compiled into Android builds only.
- **libFLAC** (`juce_audio_formats/codecs/flac/`): BSD 3-clause, Copyright Josh Coalson and the
  Xiph.Org Foundation. Enabled (`JUCE_USE_FLAC 1`).
- **The MP3 decoder** (`juce_audio_formats/codecs/juce_MP3AudioFormat.cpp`): JUCE's own code under
  the JUCE licence above. Enabled (`JUCE_USE_MP3AUDIOFORMAT 1`). JUCE's header disclaims any
  warranty against third-party patent or IP claims for this decoder; take your own advice before
  shipping it.
- **VST3 SDK, LV2 SDK and other plug-in format code** (`juce_audio_processors/format_types/`):
  shipped as part of the JUCE tree but **not compiled**. Plug-in hosting is switched off in
  `ios/JuceConfig.h` (`JUCE_PLUGINHOST_* 0`).
