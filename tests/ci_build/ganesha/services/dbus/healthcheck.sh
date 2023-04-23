#pgrep dbus-daemon
/usr/bin/timeout \
	--kill-after=1s \
	8s \
	/usr/bin/dbus-send \
		--system \
		--type=method_call \
		--print-reply \
		--reply-timeout=7500 \
		--dest=org.freedesktop.DBus \
		/org/freedesktop/DBus \
		org.freedesktop.DBus.GetId
