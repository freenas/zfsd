#! /bin/sh
#
# See the README - this is method 4c, as a script,
# with some fanciness.

FREEBSD_URL="git://github.com/freebsd/freebsd.git"
THEIR_RMT_BRANCH=origin/projects/zfsd
OUR_RMT_BRANCH=refs/remotes/upstream/master
OUR_RMT_BRANCH_SHORT=${OUR_RMT_BRANCH#refs/remotes/}

# return true (0) if given directory ($1) is a git repository
is_git_repo()
{
    git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1
}

# return true (0) if given repo ($1) has given ref
repo_has_branch()
{
    git -C "$1" rev-parse -q --verify "$2" >/dev/null 2>&1
}

# Locate or set freebsd repo.
# Returns with $upstream set to the upstream freebsd repo path.
locate_freebsd_repo()
{
    local answer path

    upstream=$(git config --get remote.upstream.url) && return 0
    while :; do
	cat << EOF
You have not yet configured a freebsd git repo as your
upstream.  Please enter the path to the freebsd repository.
NOTE: if this is a relative path, you must be in the top level
of the zfsd repo now!  Interrupt this script and start over if
necessary.
EOF
	echo -n "Enter path: "
	read path
	case "$path" in
	/*)
	    ;;
	./*|../*)
	    local cwd=$(pwd) top=$(git rev-parse --show-toplevel)
	    if [ "$cwd" != "$top" ]; then
		echo "Relative path error: current dir $cwd != top level $top"
		continue
	    fi
	    ;;
	*)
	    echo "error: path must be absolute or relative, not $path" 1>&2
	    continue;;
	esac
	if [ -e "$path" ]; then
	    if is_git_repo "$path"; then
		if repo_has_branch "$path" $THEIR_RMT_BRANCH; then
		    # found existing freebsd at url = "$path"
		    upstream="$path"
		    git config remote.upstream.url "$upstream"
		    return 0
		fi
		echo "error: $path exists but has no $THEIR_RMT_BRANCH"
	    else
		echo "error: $path exists but is not a repository"
	    fi
	    continue
	fi
	echo "Hm ... $path does not exist."
	echo -n "Do you want me to clone freebsd there? (y/n) "
	while :; do
	    read answer
	    case "$answer" in
	    [yY]|[yYeE][yYeEsS]) answer=y; break;;
	    [nN]|[nN][oO]) answer=n; break;;
	    esac
	    echo "I didn't understand '$answer'.  Do you want me to clone"
	    echo -n "freebsd to '$path'? (y/n) "
	done
	case "$answer" in
	n) continue;;
	esac
	if git clone $FREEBSD_URL "$path"; then
	    echo "Yay, that worked!"
	    upstream="$path"
	    git config remote.upstream.url "$upstream"
	    return 0
	fi
	echo "Alas, that failed."
    done
}

# Given freebsd repo path ($1), create .git/objects/info/alternates
# and put that in there.  (If the file already exists, check to
# see if it's in, and add only if needed.)
add_alternate()
{
    local alt_file line

    alt_file=.git/objects/info/alternates
    if [ -f "$alt_file" ]; then
	while read line; do [ "$line" = "$1" ] && return; done < $alt_file
    fi
    # echo "DEBUG: alternate $1 being added"
    echo "$1" >> $alt_file
}

# Make sure we're in the zfsd repo
top=$(git rev-parse --show-toplevel) || exit 1

# Find freebsd repo
locate_freebsd_repo
# echo "DEBUG: upstream freebsd repo is at $upstream"

# Add alternate
(cd $top && add_alternate "$upstream/.git/objects")

# Get tip of freebsd $THEIR_RMT_BRANCH.  Make it
# the tip of our upstream/master.
their_tip=$(cd $top && git -C $upstream rev-parse $THEIR_RMT_BRANCH) || {
    echo "Error: upstream repo $upstream" 1>&2
    echo "has no remote-tracking branch $theirbranch!" 1>&2
    exit 1
}
# echo "DEBUG: their tip is $their_tip"
our_tip=$(git rev-parse -q --verify $OUR_RMT_BRANCH)
if [ "$their_tip" = "$our_tip" ]; then
    echo "$OUR_RMT_BRANCH_SHORT: already up to date"
else
    echo "$OUR_RMT_BRANCH_SHORT: $our_tip..$their_tip"
    git update-ref $OUR_RMT_BRANCH $their_tip
fi
