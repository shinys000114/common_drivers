# nodes.sh: find V4L2 nodes by name and the GStreamer element of a node (sourced after common.sh).

# node NAME: /dev/videoN whose video device is called NAME
node() {
	local d
	for d in /sys/class/video4linux/video*; do
		[ "$(cat "$d/name" 2>/dev/null)" = "$1" ] && { echo "/dev/${d##*/}"; return; }
	done
}

# gst_element DEV KIND: the GStreamer element (v4l2convert, v4l2video3convert, ...) registered for DEV
gst_element() {
	local e
	for e in $(as_user gst-inspect-1.0 2>/dev/null | grep -o "v4l2[a-z0-9]*$2" | sort -u); do
		as_user gst-inspect-1.0 "$e" 2>/dev/null | grep -A3 '^  device ' | grep -q "Default: \"$1\"" &&
			{ echo "$e"; return; }
	done
}
