#!/usr/bin/env bash
set -eu -o pipefail

die() {
	echo "Error: ${*}" >&2
	exit 1
}

declare nexus_repo_name="${NEXUS_REPO_NAME:-saunafs}"
declare nexus_url="${NEXUS_URL:-http://localhost:8081}"
nexus_url="${nexus_url%/}"
declare nexus_username="${NEXUS_USERNAME:-}"
declare nexus_password="${NEXUS_PASSWORD:-}"
if [ -z "${nexus_username}" ] || [ -z "${nexus_password}" ]; then
	die "Nexus username and password must be set"
fi
declare nexus_auth="$(echo ${nexus_username}:${nexus_password})"


declare source_dir="${1:-}"
[ -d "${source_dir}" ] || die "Source directory '${source_dir}' does not exist"

declare files=$(find "${source_dir}" -type f -name '*.deb')

for file in ${files}; do
	echo "Uploading ${file}..."
	curl -u "${nexus_auth}" --fail -X POST \
	"${nexus_url}/service/rest/v1/components?repository=${nexus_repo_name,,}" \
	-H "accept: application/json" \
	-H "Content-Type: multipart/form-data" \
	-F "apt.asset=@${file}"
done
