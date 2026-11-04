# ODROID-C5 HDMI CEC setup

The native S7D CEC driver registers `/dev/cec0` (`s7d-cecb`). Like every
Linux CEC adapter it claims no logical address until user space configures
it, so without a configuration the TV does not see the board, and neither
sleep nor power off can hand the TV power-on to the AOCPU firmware. These
files configure the adapter at boot with `cec-ctl` from `v4l-utils`.

| File | Install to | Purpose |
|---|---|---|
| `70-odroidc5-cec.rules` | `/etc/udev/rules.d/` | Starts `odroidc5-cec@cec0.service` when the S7D adapter appears. |
| `odroidc5-cec@.service` | `/etc/systemd/system/` | Claims a playback device named ODROID-C5, once per boot. |
| `odroidc5-cec-standby@.service` | `/etc/systemd/system/` | Optional: suspends the board when the TV goes to standby. |
| `odroidc5-cec-standby` | `/usr/local/sbin/` | The standby service's script; it reads `cec-ctl --monitor`. |

## Install

Run from this directory:

```sh
sudo apt install v4l-utils
sudo install -m 644 70-odroidc5-cec.rules /etc/udev/rules.d/
sudo install -m 644 odroidc5-cec@.service odroidc5-cec-standby@.service \
	/etc/systemd/system/
sudo install -m 755 odroidc5-cec-standby /usr/local/sbin/
sudo systemctl daemon-reload
sudo udevadm control --reload
sudo udevadm trigger --action=add --subsystem-match=cec
```

To let TV standby suspend the board as well:

```sh
sudo systemctl enable --now odroidc5-cec-standby@cec0.service
```

## Check

```sh
systemctl status odroidc5-cec@cec0.service
cec-ctl -d0
```

`cec-ctl -d0` shows the OSD name ODROID-C5, a playback logical address
(normally 4) and the physical address of the TV input, for example 1.0.0.0.
Without a TV at boot the claim is kept and made when the physical address
appears. The kernel keeps it across HPD changes and sleep, and claims again
when the TV returns, so the service does not run again on hotplug.

## What the TV can do

- Power the board on. Deep sleep, the default `mem_sleep`, and power off hand
  CEC to the AOCPU firmware; s2idle keeps the controller receiving. The board
  wakes when the TV selects it (`<Set Stream Path>` or `<Routing Change>` to
  its physical address), on `<Image View On>` or `<Text View On>`, and on the
  Power and Power On Function keys.
- Suspend the board when the TV goes to standby, with
  `odroidc5-cec-standby@cec0.service` enabled. Only `<Standby>` from the TV
  (logical address 0) counts. Repeats within `CEC_STANDBY_HOLDOFF` seconds
  (default 10; set it with `Environment=` in the unit) are ignored, so a
  repeated `<Standby>` does not suspend the board again right after resume.
- Send remote keys. `cec-ctl` claims with RC passthrough, so the keys appear
  on an input device under `/sys/class/rc/`. The Power key arrives as
  `KEY_POWER`, which systemd-logind (`HandlePowerKey=`) or GNOME treat as the
  power button. A key that wakes the board is not passed on, so the Power key
  that wakes it does not put it back to sleep. Add `--no-rc-passthrough` to
  `ExecStart=` if the keys are not wanted.

To stop the TV from waking or powering on the board while keeping CEC:

```sh
echo disabled | sudo tee /sys/bus/platform/devices/fe044040.cec/power/wakeup
```

This does not persist across boots. To make it persistent, add a udev rule
such as
`SUBSYSTEM=="platform", DRIVER=="s7d-cecb", ATTR{power/wakeup}="disabled"`.

The name can be changed in `odroidc5-cec@.service`, up to 14 characters. No
vendor ID is set.

## Remove

```sh
sudo systemctl disable --now odroidc5-cec-standby@cec0.service
sudo rm /etc/udev/rules.d/70-odroidc5-cec.rules \
	/etc/systemd/system/odroidc5-cec@.service \
	/etc/systemd/system/odroidc5-cec-standby@.service \
	/usr/local/sbin/odroidc5-cec-standby
sudo systemctl daemon-reload
sudo cec-ctl -d0 --clear
```
