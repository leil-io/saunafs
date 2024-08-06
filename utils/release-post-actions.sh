#!/usr/bin/env bash
set -eux -o pipefail

die() {
	echo "Error: ${*}" >&2
	exit 1
}

released_version="${1:-}"
[ -n "${released_version}" ] || die "Missing the released version"
previous_version="${2:-}"
[ -n "${previous_version}" ] || die "Missing the previous version"

: "${DEVELOP_BRANCH:="dev"}"
: "${MASTER_BRANCH:="main"}"

## TODO: fill from the changelog
tag_message() {
	cat <<-EOF
	## What's Changed
	- (...)

	**Full Changelog**: https://github.com/leil-io/saunafs/compare/${previous_version}...${released_version}
	EOF
}

url_encode() {
	local message="${1:-}"
	echo "${message}" | \
		tr '\n' '\0' | \
		python3 -c "import urllib.parse; print(urllib.parse.quote(input()))" | \
		sed 's/%00/%0A/g'
}

## tag the release
git switch "${MASTER_BRANCH}"
git pull --ff-only
git tag -a "${released_version}" -m "Release ${released_version}"
git push origin "${released_version}"

## Create the github release
## Manual way:
xdg-open "https://github.com/leil-io/saunafs/releases/new?tag=${released_version}&title=${released_version}&body=$(url_encode "$(tag_message)")"
## TODO: automate via github client

## move the latest tag
git tag -d "latest"
git tag -a "latest" -m "${released_version}"
git push origin ":latest"
git push origin "latest"

## Bring back the release commits to the develop branch
git switch "${DEVELOP_BRANCH}"
git pull --ff-only
git merge --no-ff --no-edit "${released_version}"
git push origin "${DEVELOP_BRANCH}"
