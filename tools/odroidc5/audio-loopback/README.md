# TDMIN_LB audio loopback check

S7D can capture the data of TDMOUT A (40-pin I2S), B (HDMI) or C (analog codec)
through TDMIN_LB and a TODDR. This compares that capture with the played
samples, bit for bit: channel order, dropped or repeated frames, and the number
of significant bits that reached the TDM output.

Each sample carries a 4-bit channel tag and a 12-bit frame counter in its top 16
bits, so the check aligns even when a path keeps only 16 or 24 bits; the lower
bits hold a hash of channel and frame for full-width checks.

It checks the samples entering the HDMI transmitter, not the HDMI audio packets,
channel allocation or IEC 60958 channel status; those need TX register
readback and a real multichannel sink.

```sh
make check                        # host selftest of the analyzer
./route.sh hdmi                   # on the board: loop back TDMOUT_B (HDMI)
./run.sh hw:0,0 hw:0,4 2 S32_LE 48000 3 24
```

The card sets every output route from the device tree when it registers.
`route.sh` restores the route of `hdmi` (hw:0,0), `spdif2hdmi` (hw:0,1),
`line` (hw:0,2), `spdif` (hw:0,3) or `i2s` (hw:0,2, with the i2s overlay)
and selects that TDM output as the loopback source; the two SPDIF outputs
are not looped back.

`run.sh PLAY CAPTURE [channels] [format] [rate] [seconds] [valid_bits]` needs
`aplay`, `arecord` and Python 3. Results and raw captures go to a temporary
directory under `/var/tmp` (override with `WORK=`). The exit status is 0 only
for an exact match with the identity channel map.
