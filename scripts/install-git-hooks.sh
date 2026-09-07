#!/usr/bin/env bash

# Link this repository's pre-commit hook without replacing a hook already there.

set -uo pipefail

uninstall=0
if [ "${1:-}" = --uninstall ] && [ "$#" -eq 1 ]; then
    uninstall=1
elif [ "$#" -gt 0 ]; then
    echo "usage: $0 [--uninstall]" >&2
    exit 2
fi

# .git/hooks is shared by every linked worktree, so the link has to name the
# main one; pointing it at a linked worktree leaves it dangling once that is
# removed.
root=$(git worktree list --porcelain | sed -n '1s/^worktree //p')
hooks=$(git rev-parse --path-format=absolute --git-path hooks) || exit 1
mkdir -p "$hooks" || exit 1

source=$root/scripts/git-pre-commit.sh
target=$hooks/pre-commit

# .git/hooks being shared has a second consequence: the main worktree may be on
# a branch that does not carry this script, and git runs a dangling hook by
# quietly running nothing. Refuse rather than install a link to a file that is
# not there, so that the hook is never silently absent.
if [ "$uninstall" -eq 0 ] && [ ! -x "$source" ]; then
    if [ -e "$source" ]; then
        echo "Error: $source is not executable" >&2
        echo "git ignores a hook it cannot run, and says nothing about it." >&2
    else
        echo "Error: $source does not exist" >&2
        echo "The main worktree is on a branch without it; check that branch" \
            "out there, or commit this one into it, first." >&2
    fi
    exit 2
fi

# Whether the link is one this script created. Matching the tail rather than a
# current worktree path keeps the answer right after the checkout has been moved
# or renamed, which is precisely when the link dangles and needs attention.
ours()
{
    local link
    link=$(readlink "$target") || return 1
    [ "$link" != "${link%/scripts/git-pre-commit.sh}" ]
}

if [ "$uninstall" -eq 1 ]; then
    if [ ! -L "$target" ]; then
        echo "No pre-commit symlink to remove"
    elif ours; then
        rm -f "$target" && echo "Removed pre-commit"
    else
        echo "Left $target (not ours)"
    fi
elif [ -L "$target" ] && [ ! -e "$target" ] && ours; then

    # A moved or renamed checkout leaves our own link dangling. Keeping it would
    # disable the hook for good, so repoint it.
    ln -sfn "$source" "$target" && echo "Repointed pre-commit"
elif [ -e "$target" ] || [ -L "$target" ]; then
    echo "Kept existing $target"
else
    ln -s "$source" "$target" && echo "Installed pre-commit"
fi
