#!/bin/bash

set +x

releaseBranch="release"
commitToRelease=$(git log --before "2 weeks" --oneline | head -n1 | awk '{print $1}')
echo $commitToRelease

git checkout ${commitToRelease} -b ${releaseBranch}

changelogWithCommits="$(git log latest..${commitToRelease} --oneline --pretty='format:%h %s')"

# Figure out version to bump
isMajor=0
isMinor=0
isPatch=0

while read -r commit type title ; do
	# commit=$(awk '{print $1}' <<< $line)
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

	echo $commit
	# Check for extended description whether it has "BREAKING CHANGE: "
	echo $type
	if $(git log ${commit} -1 --pretty='%b' | tr '\n' ' ' | grep -q 'BREAKING CHANGE: '); then
		isMajor=1
		break
	fi
done <<< "${changelogWithCommits}"

# POTENTIAL BUG HERE!!!: What if a named tag start with v?
lastVersion=$(git tag | sed 's/^v//'| grep -v latest | sort -V --reverse | head -1)
echo $lastVersion

lastMajor="$(cut -d . -f 1 <<< $lastVersion)"
lastMinor="$(cut -d . -f 2 <<< $lastVersion)"
lastPatch="$(cut -d . -f 3 <<< $lastVersion)"
echo "lastMajor: $lastMajor"
echo "lastMinor: $lastMinor"
echo "lastPatch: $lastPatch"

newMajor=$lastMajor
newMinor=$lastMinor
newPatch=$lastPatch

if [[ ${isMajor} == 1 ]]; then
	echo "Relesing major version"
	((newMajor++))
	newMinor=0
	newPatch=0
elif [[ ${isMinor} == 1 ]]; then
	((newMinor++))
	newPatch=0
	echo "Relesing minor version"
elif [[ ${isPatch} == 1 ]]; then
	echo "Relesing patch version"
	((newPatch++))
else
	echo "Nothing to release"
	exit 1
fi
newVersion="${newMajor}.${newMinor}.${newPatch}"
newVersionTag="v${newMajor}.${newMinor}.${newPatch}"
echo "Version to release: $newVersionTag"

urgency="medium"

newsEntry="\n * SaunaFS (${newVersion}) ($(date '+%Y-%m-%d' -u))\n"
changelogEntry="saunafs (${newVersion}) stable; urgency=${urgency}\n"

while read -r commit title ; do
	newsEntry+=" - $title\n"
	changelogEntry+="  * $title\n"
done <<< "${changelogWithCommits}"
changelogEntry+="\n -- SaunaFS Team <contact@saunafs.com>  $(date -R -u)\n"

echo -e -n "${newsEntry}"
echo
echo -e -n "${changelogEntry}"

# Update CMakeLists.txt and other files
cmakeFile="CMakeLists.txt"
testFile="tests/tools/saunafsXX.sh"
sed -i -E 's/(set[(]DEFAULT_MIN_VERSION (["'\'']?))[^"'\'']*(\2)/\1'${newVersion}'\3/' "${cmakeFile}"
sed -i -E 's/(SAUNAFSXX_TAG=(["'\'']?))[^"'\'']*(\2)/\1'${newVersion}'\3/' "${testFile}"

newsFile="NEWS"
changelogFile="debian/changelog"

# Insert new NEWS entry after the first line
{
    read -r firstLine
    echo "${firstLine}"
    echo -e -n "${newsEntry}"
    cat
} < ${newsFile} > "/tmp/newsTemp"
mv "/tmp/newsTemp" ${newsFile}

# Insert changelog entry at the top of the changelog file
{
    echo -e "${changelogEntry}"
    cat
} < ${changelogFile} > "/tmp/changelogTemp"
mv "/tmp/changelogTemp" ${changelogFile}

git add ${newsFile} ${changelogFile} ${cmakeFile} ${testFile}
git commit -m "chore: Changelog for ${newVersionTag}"
git tag -a "${newVersionTag}-rc1" -m "${newVersionTag}"
newBranch="release/${newVersionTag}"
git branch -M "${newBranch}"
git push -u origin "${newBranch}"
git switch dev
git branch -D $releaseBranch
# git cherry-pick last commit from newVersionTag-rc1
