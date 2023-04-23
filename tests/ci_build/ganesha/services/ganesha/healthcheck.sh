#pgrep ganesha.nfsd
TRANSPORT=tcp
HOST=127.0.0.1
PROGRAM=100003
VERSION=4

ulimit -n 1024

/usr/bin/timeout \
        --kill-after=1s \
        8s \
        /usr/bin/rpcinfo \
            -T ${TRANSPORT} \
            ${HOST} \
            ${PROGRAM} \
            ${VERSION}
