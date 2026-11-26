# Merging MuseScore updates into Ploq

The GitHub repo's history starts with **one commit containing MuseScore Studio v4.7.7** ("MuseScore
Studio v4.7.7 (upstream base)"). MuseScore's full history isn't on GitHub here because its first
commit (2012) contains `test/ntest/core`, a 105 MB file, and GitHub rejects files over 100 MB.
(A public GitHub *fork* of musescore/MuseScore wouldn't have this problem, but forks of public
repos can't be private.)

To merge a MuseScore release, tell your local git that the base commit really is upstream v4.7.7.
Then merge "squashed", so no upstream history (and no 105 MB file) gets pushed.

## One-time setup in a clone

```sh
git remote add upstream https://github.com/musescore/MuseScore.git   # if not already there
git fetch upstream --tags
# The base commit is the first commit on main:
BASE=$(git rev-list --max-parents=0 main)
git replace --graft "$BASE" v4.7.7
```

`git replace` is local only: it isn't pushed, and it only changes how *your* git sees history.

## Merging a release (e.g. v4.8.0)

```sh
git fetch upstream --tags
git checkout main
git merge --squash v4.8.0
# Resolve conflicts. Ours should only be the small "COLLAB:" hooks in upstream files
# (git grep -n "COLLAB:" lists them). Rebuild, run both instances, test.
git commit -m "Merge MuseScore Studio v4.8.0 (squashed)"
git --no-replace-objects push
```

Then record that this commit contains v4.8.0, so the *next* merge starts from there:

```sh
MERGED=$(git rev-parse HEAD)
git replace --graft "$MERGED" "$MERGED^" v4.8.0
```

## Pushing

Once the graft is set up, push with **`git --no-replace-objects push`**, so git ignores the local
graft and can't try to upload upstream history. Never use `--follow-tags` (upstream tags point at
upstream history). If a push ever tries to upload hundreds of MB, stop. Something pulled upstream
history in (for example a merge that wasn't `--squash`).
