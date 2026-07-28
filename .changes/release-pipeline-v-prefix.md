---
kind: changed
pr: null
---
Switch release tags to a v-prefixed scheme and fix the release pipeline

## Release Note
No user-facing change

Release tags now use a "v" prefix (e.g. v0.2.0). Also fixed a release
pipeline bug that prevented the GitHub Release from being published
after tagging, and added develop as a prerelease branch.
