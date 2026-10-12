# M27: filesystem compatibility (in progress)

Working directories now belong to guest processes. `chdir`, `fchdir`, `getcwd`, relative
open/stat/rename/exec and clone inheritance use directory descriptors without
changing the host process working directory. Children can change directories
without affecting their parent. Forked regular descriptors retain shared file
offsets. Forked directory descriptors share a cursor with reference-counted stream ownership; closing the child descriptor preserves the parent cursor. Exec preserves the working directory.

The root-relative, no-link policy remains in force, including rejection of `..`.
After a cwd is renamed, relative operations continue through its descriptor;
`getcwd` returns ENOENT until a verified pathname is established by another
chdir. It does not return a stale or host pathname. This differs from Linux's
ability to discover the renamed path and is an explicit remaining gap.

`mac-m27-validation.json` and `evidence/m27-sanitizer.json` cover 19 local checks each, plus a Linux comparison for fchdir/shared-directory lifetime:
three execution modes across cache variants, pointer/buffer errors, child cwd
isolation, shared offsets, symlink/traversal rejection, renamed directories,
and unmodified upstream ash using cd/subshells/relative files.

M27 remains open for directory dup/dup2 across descriptor classes, permitted link
semantics, physical parent traversal, fuller metadata and adversarial filesystem race qualification.
