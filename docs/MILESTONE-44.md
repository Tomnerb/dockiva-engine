# M44: filesystem prerequisites in progress

The filesystem adapter implements fsync and fdatasync on owned guest file and
directory descriptors. Both call host fsync; fdatasync may perform the stronger
metadata flush. This does not promise host power-loss durability or implement
Darwin F_FULLFSYNC. Crash ordering and database recovery are not yet qualified.

Bounded faccessat checks use the synthetic root identity and retain the read-only
and no-link policies. readlinkat returns only the final link's raw text; neither
ancestor links nor the returned target are followed. fstatat supports owned
file descriptors with AT_EMPTY_PATH. This does not add general symlink traversal.

`python3 scripts/verify_m44_native.py --sanitize` tests sync, close/reopen contents,
invalid descriptors, access modes, empty-path metadata, raw link reads and parent
traversal rejection. Atomic replacement, broader permissions, locking, shared
mappings, crash injection and persistence ordering remain open for M44–M48.
