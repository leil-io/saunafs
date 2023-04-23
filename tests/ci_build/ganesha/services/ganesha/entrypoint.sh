: "${GANESHA_LOGFILE:="/var/log/ganesha.log"}"
: "${GANESHA_LOGLEVEL:="WARN"}" # DEBUG, EVENT, WARN, CRIT
: "${GANESHA_CONFIGFILE:="/etc/ganesha/ganesha.conf"}"
: "${GANESHA_NFS_PORT:="2049"}"
: "${GANESHA_NFS_PROTOCOLS:="3,4"}"
: "${GANESHA_EXPORT_ID:="77"}"
: "${GANESHA_PATH:="/shares"}"
: "${GANESHA_PSEUDO:="/shares"}"
: "${GANESHA_ACCESS_TYPE:="RW"}"
: "${GANESHA_SQUASH:="No_Root_Squash"}" # No_Root_Squash, Root_Id_Squash, Root_Squash, All_Squash
: "${GANESHA_SECTYPE:="sys"}"
: "${GANESHA_CLIENTS:="*"}"
: "${GANESHA_TRANSPORTS:="UDP,TCP"}"
: "${GANESHA_SVCNAME:="ganesha"}"
: "${GANESHA_NFS_START:="true"}"
: "${GANESHA_RQUOTA_START:="false"}"
: "${GANESHA_NLM_START:="false"}"
: "${GANESHA_MNT_START:="false"}"

cat <<EOF | sudo tee "${GANESHA_CONFIGFILE}" >/dev/null
LOG {
	Default_Log_Level = ${GANESHA_LOGLEVEL};
	Components {
		FSAL = INFO;
		NFS4 = EVENT;
	}
	Facility {
		name = FILE;
		destination = "${GANESHA_LOGFILE}";
		enable = active;
	}
}

EXPORT {
	Export_Id = ${GANESHA_EXPORT_ID};
	Path = ${GANESHA_PATH};
	Pseudo = ${GANESHA_PSEUDO};
	Protocols = ${GANESHA_NFS_PROTOCOLS};
	Transports = ${GANESHA_TRANSPORTS};
	Access_Type = ${GANESHA_ACCESS_TYPE};
	SecType = ${GANESHA_SECTYPE};
	Squash = ${GANESHA_SQUASH};
	FSAL {
		Name = VFS;
	}
	CLIENT {
		Clients = ${GANESHA_CLIENTS};
		Access_Type = ${GANESHA_ACCESS_TYPE};
		Squash = ${GANESHA_SQUASH};
	}
}
EOF

# Create the deepest directory with 777 permissions
sudo mkdir -m 0777 -p "${GANESHA_PATH}"

# Set 755 permissions for all parent directories
current_path="${GANESHA_PATH}"
while [[ "${current_path}" != "/" ]]; do
    current_path=$(dirname "${current_path}")
    sudo chmod 0755 "${current_path}"
done

sudo mkdir -m 0755 -p /var/run/ganesha
