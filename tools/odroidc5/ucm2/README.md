# UCM2 profile for the ODROID-C5 native sound card

Without a UCM profile PipeWire sees the native card (`AML-AUGESOUND`, driver
`axg-sound-card`) only through its `stereo-fallback` mapping on `hw:0,0`, so
the desktop offers HDMI stereo and nothing else. This profile, laid out as in
alsa-ucm-conf, gives PipeWire and PulseAudio one `HiFi` use case with:

| UCM device | PCM | Output |
|---|---|---|
| `HDMI1` | `hw:AMLAUGESOUND,0` | HDMI through TDM B (I2S); jack `HDMI Jack` |
| `Line1` | `hw:AMLAUGESOUND,2` | line out of the internal DAC, and the 40-pin I2S with the `i2s` overlay |
| `SPDIF` | `hw:AMLAUGESOUND,3` | S/PDIF on 40-pin pin 29, only with the `spdif_a` overlay |

`hw:0,1` (HDMI through SPDIF B) reaches the same sink as `hw:0,0`, and the HDMI
transmitter plays one stream at a time, so it is not offered. `hw:0,4` records
what an output plays (TDMIN_LB) for the loopback tests and is not an input.

The `HiFi` verb has three variants, which PipeWire shows as three profiles:
`HiFi` (HDMI stereo), `HiFi 5+1` and `HiFi 7+1` (HDMI 5.1 and 7.1). The card
limits HDMI to what the sink's ELD lists, so a variant the connected sink
cannot take fails to open while PipeWire probes the card, and PipeWire drops
it. PipeWire probes when it starts or the card appears: after changing to a
sink with more speakers, restart PipeWire to see the surround profiles. The
card gives each HDMI channel count its speaker order (`Playback Channel Map`),
which PipeWire uses for UCM devices.

Each device sets its own mixer route again when it is enabled, in case a saved
mixer state changed the route the device tree sets.

## Install

On the board, from this directory:

```sh
sudo install -d /usr/share/alsa/ucm2/Amlogic/ODROID-C5 \
	/usr/share/alsa/ucm2/conf.d/axg-sound-card
sudo install -m 644 Amlogic/ODROID-C5/ODROID-C5.conf Amlogic/ODROID-C5/HiFi.conf \
	/usr/share/alsa/ucm2/Amlogic/ODROID-C5/
sudo ln -sf ../../Amlogic/ODROID-C5/ODROID-C5.conf \
	/usr/share/alsa/ucm2/conf.d/axg-sound-card/AML-AUGESOUND.conf
systemctl --user restart wireplumber pipewire pipewire-pulse
```

alsa-lib finds the profile as `conf.d/axg-sound-card/AML-AUGESOUND.conf`, from
the card's driver and long name. The vendor kernel's `Hardkernel/ODROID-C5`
profile is keyed to the vendor card and does not match this card. The
profile uses UCM syntax 6 (verb variants and a path condition); alsa-lib
1.2.11 parses it.

To remove it, delete the link and the `Amlogic/ODROID-C5` directory and
restart PipeWire.

## Check

```sh
alsaucm -c hw:AMLAUGESOUND list _verbs            # HiFi, HiFi 5+1, HiFi 7+1
alsaucm -c hw:AMLAUGESOUND list _devices/HiFi     # HDMI1, Line1 (and SPDIF)
wpctl status                                      # HDMI and Line Out sinks
pactl list cards | grep -A12 Profiles             # the HiFi profiles
```

With the profile, PipeWire sink names change from
`alsa_output.platform-auge_sound.stereo-fallback` to names such as
`alsa_output.platform-auge_sound.HiFi__HDMI1__sink`. A default-sink setting
that names the old sink, such as the one odroid-alsa writes, must follow.

HDMI passthrough (AC-3, DTS, TrueHD) needs `hdmi:` and `iec958:` PCMs from an
alsa-lib card configuration, which this profile does not add; its devices play
L-PCM.
