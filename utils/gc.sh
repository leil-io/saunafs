#!/usr/bin/env bash
set -eu -o pipefail

die() { echo "Error: ${*}" >&2; exit 1; }
while read -r timestamp file; do ts=$(date -u +%s --date="${timestamp}"); now=$(date -u +%s); if ((now - ts > 5000 )); then echo "${file}"; fi ; done < <(find -name '*.met' -o -name '*.dat' | awk '{print $0".'"$(date -u +'%Y%m%d%H%M%S')"'"}' | awk -F'.' '{print $NF" "$0}' | sed -E 's/^(....)(..)(..)(..)(..)(..)/\1-\2-\3T\4:\5:\6/')

main() {
	while [ -n "${1:-}" ]; do
		case "${1}" in
			--help|-h)
				usage
				exit 0
				;;
			--
			*)
				die "Unknown option: $1"
				;;
		esac
		shift
	done
}

main "$@"
