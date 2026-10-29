# Unreleased changelog entries

This directory holds only the entries for the next release, one file per change.
Released entries are in [CHANGELOG.md](/CHANGELOG.md).
Cutting a release moves the entries there and deletes their files; see [docs/release.md](/docs/release.md#changelog).

What to write: see [CONTRIBUTING.md](/CONTRIBUTING.md#changelog).

## Adding an entry

A PR adds its entry as a new file, `changelog.d/<section>/<name>.md`.

- `<section>` is one of `headline`, `added`, `changed`, `deprecated`, `removed`, `fixed` or `security`.
- `<name>` is unique within the section and contains no dot.
- The file holds the list item as it will appear in CHANGELOG.md, without the PR link:

  ```markdown
  - [`upstream.tls.ca_cert`](/docs/config.md#upstreams) was ignored, so upstream certificates were checked against the system trust store.
  ```

- At release, the PR that added the file is linked after the item's first line.

[`scripts/dev/changelog-release.py`](/scripts/dev/changelog-release.py), run without arguments, prints the entries as they will appear in CHANGELOG.md.

## One entry, several PRs

- A later PR adds an empty file, `<name>.<anything>.md`, next to the entry's `<name>.md`. `<anything>` contains no dot.
- The entry then links every PR that added one of its files.
- Editing a file adds no link. A PR that rewords the entry edits `<name>.md`.
- Renaming a file counts as adding it. A PR that moves an entry gives it explicit links to its PRs.

## Explicit links

A file whose first line ends in moqx PR links, such as `([#123](https://github.com/openmoq/moqx/pull/123))`, is linked to those PRs instead of the one that added it.
This covers a change that landed without a PR.
