TIMEOUT=10

for i in $(seq 1 $TIMEOUT); do
	echo "Waiting for rpcbind to be up ($i/$TIMEOUT)..."
	if /usr/bin/rpcinfo -T tcp 127.0.0.1 100000 4 2>/dev/null; then
		break
	fi
	sleep 1
done
echo "rpcbind listening, starting rpc.statd"
sudo /usr/sbin/rpc.statd
