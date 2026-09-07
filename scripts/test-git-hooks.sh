#!/usr/bin/env bash

# Smoke-test hook installation and staged-content checking.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
source "$root/.ci/common.sh"

# The hook downgrades a missing tool to a note and passes, so probe for all of
# them up front; otherwise an absent tool surfaces as a bogus assertion failure.
formatter=$(find_clang_format) || {
    echo "clang-format version 20 not found" >&2
    exit 1
}

# The names the checkers resolve, not the defaults: an override pointing at a
# tool under another name is a valid configuration, and file(1) is a dependency
# of check-newline.sh that nothing else here would notice was missing.
for tool in "${COMMENTFLOW:-commentflow}" "${SHFMT:-shfmt}" \
    "${SHELLCHECK:-shellcheck}" file; do
    command -v "$tool" > /dev/null 2>&1 || {
        echo "$tool not found" >&2
        exit 1
    }
done

# The repositories below must not inherit the developer's own configuration: a
# global core.hooksPath sends install-git-hooks.sh outside this sandbox, where
# the trap does not reach and a stray hook is left behind for every later
# commit.
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null

test_dir=$(mktemp -d)
linked_dir="$test_dir-linked"
trap 'rm -rf "$test_dir" "$linked_dir"' EXIT

git -C "$test_dir" init -q
git -C "$test_dir" config user.email test@example.com
git -C "$test_dir" config user.name Test
cp -R "$root/.ci" "$root/scripts" "$test_dir/"
cp "$root/.clang-format" "$root/.editorconfig" "$test_dir/"
git -C "$test_dir" add .
(cd "$test_dir" && scripts/install-git-hooks.sh) > /dev/null
test -L "$test_dir/.git/hooks/pre-commit"

echo 'int add(int first,int second,int third){return first+second+third;} int main(){return 0;}' > "$test_dir/bad.c"
git -C "$test_dir" add bad.c
if (cd "$test_dir" && .git/hooks/pre-commit) > /dev/null 2>&1; then
    echo "pre-commit accepted unformatted C" >&2
    exit 1
fi

"$formatter" -i "$test_dir/bad.c"
git -C "$test_dir" add bad.c
(cd "$test_dir" && .git/hooks/pre-commit)
git -C "$test_dir" commit -q -m "Add baseline"

printf 'no newline' > "$test_dir/bad.txt"
git -C "$test_dir" add bad.txt
if (cd "$test_dir" && .git/hooks/pre-commit) > /dev/null 2>&1; then
    echo "pre-commit accepted a text file without a final newline" >&2
    exit 1
fi
printf 'newline restored\n' > "$test_dir/bad.txt"
git -C "$test_dir" add bad.txt
(cd "$test_dir" && .git/hooks/pre-commit)
git -C "$test_dir" commit -q -m "Add text"

mkdir -p "$test_dir/.ci"
printf '#!/usr/bin/env bash\nreadonly value="$(false)"\necho "$value"\n' \
    > "$test_dir/.ci/warning.sh"
git -C "$test_dir" add .ci/warning.sh
if (cd "$test_dir" && .git/hooks/pre-commit) > /dev/null 2>&1; then
    echo "pre-commit accepted a ShellCheck warning" >&2
    exit 1
fi
printf '#!/usr/bin/env bash\nreadonly value\nvalue="$(false)"\necho "$value"\n' \
    > "$test_dir/.ci/warning.sh"
git -C "$test_dir" add .ci/warning.sh
(cd "$test_dir" && .git/hooks/pre-commit)
git -C "$test_dir" commit -q -m "Add clean shell"

sed -i.bak 's/indent_size = 4/indent_size = 2/' "$test_dir/.editorconfig"
rm "$test_dir/.editorconfig.bak"
git -C "$test_dir" add .editorconfig
if (cd "$test_dir" && .git/hooks/pre-commit) > /dev/null 2>&1; then
    echo "pre-commit ignored a staged EditorConfig change" >&2
    exit 1
fi

git -C "$test_dir" checkout -q HEAD -- .editorconfig
printf '\nColumnLimit: 20\n' >> "$test_dir/.clang-format"
git -C "$test_dir" add .clang-format
if (cd "$test_dir" && .git/hooks/pre-commit) > /dev/null 2>&1; then
    echo "pre-commit ignored a staged clang-format change" >&2
    exit 1
fi
git -C "$test_dir" checkout -q HEAD -- .clang-format

# A checkout without .editorconfig must still be committable: git checkout-index
# fails outright on a name the index does not hold, which used to block every
# commit on any branch predating the file.
git -C "$test_dir" rm -q .editorconfig
git -C "$test_dir" commit -q -m "Drop EditorConfig"
printf 'int answer(void)\n{\n    return 42;\n}\n' > "$test_dir/answer.c"
git -C "$test_dir" add answer.c
(cd "$test_dir" && .git/hooks/pre-commit)
git -C "$test_dir" commit -q -m "Add answer"

# What is staged is what is being committed, so the snapshot has to be judged
# rather than the working tree, which may hold anything at all.
printf 'int partial(void)\n{\n    return 1;\n}\n' > "$test_dir/partial.c"
git -C "$test_dir" add partial.c
printf 'int  partial (void){return 1;}\n' > "$test_dir/partial.c"
(cd "$test_dir" && .git/hooks/pre-commit)
git -C "$test_dir" commit -q -m "Add partial"
git -C "$test_dir" checkout -q -- partial.c

# A checker that cannot run at all must read as a note rather than as a verdict
# on the tree, or a machine missing one tool can no longer commit anything.
printf 'int advisory(void)\n{\n    return 0;\n}\n' > "$test_dir/advisory.c"
git -C "$test_dir" add advisory.c
note=$(cd "$test_dir" && COMMENTFLOW=$test_dir/absent .git/hooks/pre-commit 2>&1) || {
    echo "pre-commit rejected a commit over an unavailable tool" >&2
    exit 1
}
case "$note" in
    *"commentflow unavailable"*) ;;
    *)
        echo "pre-commit did not report the unavailable tool" >&2
        exit 1
        ;;
esac
git -C "$test_dir" commit -q -m "Add advisory"

# .git/hooks is shared, so the hook must run the checkers of the worktree being
# committed to, not those of whichever worktree installed it.
git -C "$test_dir" worktree add -q -b linked-test "$linked_dir"
printf '#!/usr/bin/env bash\nexit 1\n' > "$linked_dir/.ci/check-newline.sh"
git -C "$linked_dir" add .ci/check-newline.sh
hook=$(git -C "$linked_dir" rev-parse --path-format=absolute \
    --git-path hooks/pre-commit)
if (cd "$linked_dir" && "$hook") > /dev/null 2>&1; then
    echo "pre-commit used another worktree's checks" >&2
    exit 1
fi
git -C "$test_dir" worktree remove --force "$linked_dir"

# A moved or renamed checkout leaves our own symlink dangling; installing again
# must repoint it rather than report it as someone else's hook.
ln -sfn /nonexistent/scripts/git-pre-commit.sh "$test_dir/.git/hooks/pre-commit"
(cd "$test_dir" && scripts/install-git-hooks.sh) > /dev/null
test -x "$test_dir/.git/hooks/pre-commit"

(cd "$test_dir" && scripts/install-git-hooks.sh --uninstall) > /dev/null
test ! -e "$test_dir/.git/hooks/pre-commit"
printf '#!/bin/sh\nexit 0\n' > "$test_dir/.git/hooks/pre-commit"
(cd "$test_dir" && scripts/install-git-hooks.sh) > /dev/null
test ! -L "$test_dir/.git/hooks/pre-commit"

# git ignores a hook it cannot run, and a hook whose source is missing or not
# executable is exactly that. Installing one has to refuse rather than report
# success, so neither refusal can go quiet again.
rm -f "$test_dir/.git/hooks/pre-commit"
chmod -x "$test_dir/scripts/git-pre-commit.sh"
if (cd "$test_dir" && scripts/install-git-hooks.sh) > /dev/null 2>&1; then
    echo "install accepted a source that cannot be executed" >&2
    exit 1
fi
test ! -e "$test_dir/.git/hooks/pre-commit"
chmod +x "$test_dir/scripts/git-pre-commit.sh"

mv "$test_dir/scripts/git-pre-commit.sh" "$test_dir/git-pre-commit.sh"
if (cd "$test_dir" && scripts/install-git-hooks.sh) > /dev/null 2>&1; then
    echo "install accepted a source that is not there" >&2
    exit 1
fi
test ! -e "$test_dir/.git/hooks/pre-commit"
mv "$test_dir/git-pre-commit.sh" "$test_dir/scripts/git-pre-commit.sh"

# An extra argument is a mistake, not a second opinion about uninstalling.
if (cd "$test_dir" && scripts/install-git-hooks.sh --uninstall extra) > /dev/null 2>&1; then
    echo "install accepted --uninstall with a stray argument" >&2
    exit 1
fi
