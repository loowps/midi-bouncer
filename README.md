# MIDI Bouncer

A GUI-less [CLAP] plugin that assigns each of the 16 MIDI input channels to a choke group (mute group). Within a group,
a lower-numbered channel that has any active note blocks higher-numbered channels in the same group from starting new
notes. Lower channel number = higher priority.

The plugin is a note effect: MIDI/note events in → filtered MIDI/note events out. No audio I/O, no GUI.

#### 🔊 [Bandcamp] / [Soundcloud] / [Apple Music] / [Spotify]

[CLAP]: https://github.com/free-audio/clap
[Bandcamp]: https://loowps.bandcamp.com
[Soundcloud]: https://soundcloud.com/loowps
[Apple Music]: https://music.apple.com/us/artist/loowps/1326334750
[Spotify]: https://open.spotify.com/artist/2jOQrKX3rRoZORPfFcXaYU
