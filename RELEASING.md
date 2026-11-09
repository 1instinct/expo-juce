# Releasing

Pushing a tag `v<version>` publishes that version to npm through `.github/workflows/publish.yml`.
That workflow runs CI first: build, a package contents check and the offline audio harness.

```sh
npm version 0.9.0-rc.1 --no-git-tag-version   # or edit package.json
git commit -am "chore: 0.9.0-rc.1"
git tag v0.9.0-rc.1
git push origin HEAD v0.9.0-rc.1
```

- Versions with a prerelease part (`-rc.N`) are published under the `next` dist-tag and marked as a prerelease on GitHub. Plain versions go to `latest`.
- The workflow fails if the tag doesn't match `package.json` or if the version is already on npm.
- A GitHub release with generated notes is created for every published tag.

## npm auth (one-time)

Use one of these:

1. **Trusted publishing (preferred, no secret).** On npmjs.com, open `expo-juce` → Settings → Trusted publishing and add GitHub Actions with organization `1instinct`, repository `expo-juce` and workflow `publish.yml`. Leave the environment empty.
2. **Token.** Create a granular access token with read/write on `expo-juce` and add it as the repo secret `NPM_TOKEN`.

The repo is private, so npm provenance attestations aren't generated.
