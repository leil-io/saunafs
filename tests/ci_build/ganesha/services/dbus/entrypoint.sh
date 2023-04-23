echo "Starting dbus"
cat <<EOC | sudo tee /etc/dbus-1/system.d/ganesha.nfsd.conf >/dev/null
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>

  <!-- Allow the user running Ganesha to own the service name -->
  <policy user="root">
    <allow own="org.ganesha.nfsd"/>
  </policy>

  <!-- Allow everyone to invoke methods on Ganesha -->
  <policy context="default">
    <allow send_destination="org.ganesha.nfsd"/>
  </policy>

</busconfig>
EOC

sudo mkdir -p /var/run/dbus
sudo /usr/bin/dbus-uuidgen --ensure
sudo /usr/bin/dbus-daemon --system --fork
