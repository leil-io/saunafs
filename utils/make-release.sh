#!/bin/bash
set -eux -o pipefail

: "${CHANGELOG_FILE:="debian/changelog"}"
: "${NEWS_FILE:="NEWS"}"
: "${RELEASE_BRANCH:="release"}"
: "${DEVELOP_BRANCH:="dev"}"
: "${RELEASE_WAIT:="2 weeks"}"
: "${RELEASE_URGENCY:="medium"}"

die() {
	echo "Error: ${*}" >&2
	exit 1
}

is_truthy() {
	local -r value="${1:-}"
	[ -n "${value}" ] || return 1
	case "${value,,}" in
		1 | true | yes | y | on | enabled)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

get_release_commit() {
	{
		git stash push -m "Stash local changes before release"
		git switch "${DEVELOP_BRANCH}"
		git pull
	} > /dev/null 2>&1 || die "Failed to switch to the develop branch"
	git log --before "${RELEASE_WAIT}" --oneline | head -n1 | awk '{print $1}'
}

create_release_branch() {
	git switch -c "${RELEASE_BRANCH}" "${1}"
}

get_changelog() {
	git log "${1}..${2}" --oneline --pretty='format:%h %s'
}

get_latest_version_from_tags() {
	git tag --list | \
		grep -P '^v?[0-9]+(?:\.[0-9]+)*' | \
		sed 's/^v//' | \
		sort --version-sort --reverse | \
		head -n1
}

## Get the type of changes
## example: echo "${changelog}" | get_changes_type
get_changes_type() {
	local isMajor=0
	local isMinor=0
	local isPatch=0

	while read -r commit type title ; do
		type=$(sed -E 's/\([^)]*\)//' <<< "${type}")

		case ${type,,} in
			fix:) isPatch=1
				;;
			feat:) isMinor=1
				;;
			feat!:) isMajor=1
				break
				;;
			fix!:) isMajor=1
				break
				;;
			"breaking change:") isMajor=1
				;;
			*)
				;;
		esac

		if git log "${commit} "-1 --pretty='%b' | tr '\n' ' ' | grep -q 'BREAKING CHANGE: '; then
			isMajor=1
			break
		fi
	done < /dev/stdin

	if is_truthy "${isMajor}"; then
		echo "major"
	elif is_truthy "${isMinor}"; then
		echo "minor"
	elif is_truthy "${isPatch}"; then
		echo "patch"
	else
		echo "invalid"
	fi
}

get_first_integer() {
	local value="${1}"
	local valueArr result
	IFS=' ' read -r -a valueArr <<< "${value//[!0-9]/ }"
	result="${valueArr[0]:-}"
	[ -n "${result}" ] || die "Invalid version part: '${value}'"
	echo "${result}"
}

get_next_version() {
	local currentVersion="${1:-0.0.0}"
	local changesType="${2}"

	local versionParts
	IFS='.' read -r -a versionParts <<< "${currentVersion}"

	local newMajor="${versionParts[0]}"
	newMajor=$(get_first_integer "${newMajor}")
	local newMinor="${versionParts[1]:-0}"
	newMinor=$(get_first_integer "${newMinor}")
	local newPatch="${versionParts[2]:-0}"
	newPatch=$(get_first_integer "${newPatch}")

	case "${changesType,,}" in
		major)
			((newMajor++))
			newMinor=0
			newPatch=0
			;;
		minor)
			((newMinor++))
			newPatch=0
			;;
		patch)
			((newPatch++))
			;;
		*)
			die "Invalid changes type: ${changesType}"
			;;
	esac

	echo "${newMajor}.${newMinor}.${newPatch}"
}

update_changelog_file() {
	local newVersion="${1}"
	local urgency="${2}"
	local changelogFile="${3}"

	local changelogEntry="saunafs (${newVersion}) stable; urgency=${urgency}\n"

	while read -r commit title ; do
		changelogEntry+="  * ${title}\n"
	done < /dev/stdin

	changelogEntry+="\n -- SaunaFS Team <contact@saunafs.com>  $(date -u -R)\n"

	{
		echo -e "${changelogEntry}"
		cat
	} < "${changelogFile}" > "/tmp/changelogTemp"
	mv "/tmp/changelogTemp" "${changelogFile}"
}

update_news_file() {
	local newVersion="${1}"
	local newsFile="${2}"

	local newsEntry="\n * SaunaFS (${newVersion}) ($(date -u '+%Y-%m-%d'))\n"

	while read -r commit title ; do
		newsEntry+=" - ${title}\n"
	done < /dev/stdin

	{
		read -r firstLine
		echo "${firstLine}"
		echo -e -n "${newsEntry}"
		cat
	} < "${newsFile}" > "/tmp/newsTemp"
	mv "/tmp/newsTemp" "${newsFile}"
}

update_cmake_file() {
	local newVersion="${1}"
	local cmakeFile="${2}"

	## Look for a line like 'set(DEFAULT_MIN_VERSION "X.Y.Z"' with optional single or double quote
	## surrounding the version, and replace the version.
	sed -i -E 's/(set[(]DEFAULT_MIN_VERSION (["'\'']?))[^"'\'']*(\2)/\1'"${newVersion}"'\3/' "${cmakeFile}"
}

process_release() {
	local commitToRelease="${1}"
	local changelogWithCommits="$(get_changelog latest "${commitToRelease}")"
	local changesType="$(get_changes_type <<< "${changelogWithCommits}")"
	local currentVersion="$(get_latest_version_from_tags)"
	local newVersion="$(get_next_version "${currentVersion}" "${changesType}")"
	local newVersionTag="v${newVersion}"

	update_changelog_file "${newVersion}" "${RELEASE_URGENCY}" "${CHANGELOG_FILE}" <<< "${changelogWithCommits}"
	update_news_file "${newVersion}" "${NEWS_FILE}" <<< "${changelogWithCommits}"
	update_cmake_file "${newVersion}" "CMakeLists.txt"
	git add "${CHANGELOG_FILE}" "${NEWS_FILE}" "CMakeLists.txt"
	git commit -m "chore: Changelog for ${newVersionTag}"

	local newBranch="release/${newVersionTag}"
	git branch -M "${newBranch}" # rename the current branch
	git push -u origin "${newBranch}"

	git tag -a "${newVersionTag}-rc1" -m "Release candidate 1 for ${newVersionTag}" # create a release candidate tag
	git push origin "${newVersionTag}-rc1"

	git switch "${DEVELOP_BRANCH}"
	git branch -D "${newBranch}"
}

## Main
## Usage: make-release.sh [commitToRelease]
## If commitToRelease is not provided, it will look for the latest commit in the last 2 weeks
main() {
	local commitToRelease="${1:-"$(get_release_commit)"}"
	create_release_branch "${commitToRelease}"
	process_release "${commitToRelease}"
}

main "${@}"

# TODO: Create a PR to merge the release branch into the main branch
# TODO: Add a script to react on the merge, and:
# 1. git cherry-pick "${newVersionTag}" from dev
# 2. git push origin dev (beware of the branch protection)
# 3. create the release tag
# 4. create the release
# 4.1. Add the changelog to the release
# 5. create the latest tag
# 6. Use a GPT API to imrove the changelog
