## What and why

<!-- What does this change do, and what problem does it solve? One or two paragraphs.
     If it fixes an open issue, link it: "Fixes #123". -->

## Checklist

- [ ] Builds on **UE 5.7** (the compile floor)
- [ ] Builds on **UE 5.8**
- [ ] Any engine API newer than 5.7 sits behind an `ENGINE_MINOR_VERSION` gate, with a working 5.7 path
- [ ] No unrelated whitespace, line-ending, or reformatting churn in the diff
- [ ] Specs updated (`Docs/specs/SPEC_<Module>.md`) if the public surface changed
- [ ] `CHANGELOG.md` updated under `## [Unreleased]`
- [ ] New or changed actions verified in-editor (not just compiled)

## Testing notes

<!-- How did you verify this? Editor version, the actions you called, what you saw. -->
