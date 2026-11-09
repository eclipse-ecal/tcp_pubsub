# Frozen tcp_pubsub v2 snapshot

| Property | Value |
|---|---|
| Version | 2.0.1 |
| Source | `master` at `971382ee3c62ef02795f16d5aa51aa9b508b58e7` |
| Commit date | 2026-11-03 |
| Scope | The complete `tcp_pubsub/` library, original `tests/`, and `LICENSE` from that commit |
| License | [MIT](LICENSE), copied unchanged from the snapshot commit |

The local `master` and `origin/master` refs both pointed to this commit when the snapshot was
created. The snapshot was extracted with `git -c core.autocrlf=false archive` from that immutable
commit, not copied from the working tree. Disabling line-ending conversion preserves the exact
Git blob bytes on Windows. It does not contain any of the rewrite's M0 wire codec.

The files under [tcp_pubsub/](tcp_pubsub/) and [tests/](tests/) are frozen, including their
original CMake files and C++14 requirement. Do not apply rewrite changes or fixes here.
Maintain build selection, benchmark adapters, and comparison tooling outside the snapshot.

## Building

Use the repository root and its [CMake presets](../../CMakePresets.json), not this directory
as a standalone CMake source tree:

```console
cmake --preset legacy
cmake --build --preset legacy
ctest --preset legacy
```

This builds the original library and transport tests, plus the shared end-to-end and micro
benchmark programs. The `current` preset builds the active implementation and its tests instead.
Build and install directories are separate for each preset. Both implementations retain the
same target names and namespace; never link them into the same executable.

Third-party dependencies and benchmark tooling are shared with the active repository, not
duplicated here. For provenance, the snapshot commit pinned asio to
`a892f73dc96bfaf92db98a3fe219f920fad007ea`, recycle to
`c5425709b2273ef6371647247d1a1d86aa75c2e6`, and GoogleTest to
`d144031940543e15423a25ae5a8a74141044862f`. Record the dependency revisions actually used
alongside comparison results.

See the [benchmark workflow](../../docs/rewrite/05-benchmark.md) for running comparable
measurements and recording the snapshot commit in result labels.
