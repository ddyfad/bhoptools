# Staging

This repo (ddyfad/bhoptools-staging, private) is where client changes are built and tried before
they reach ddyfad/bhoptools. There are two clients:

- **bhoptools**: the public client. Updates from the public repo's releases, settings in
  `%APPDATA%\bhoptools` (Linux `~/.config/bhoptools`).
- **bhoptools-dev**: built from this repo on every push to `sourcejump` or `dev/*`. Updates from this
  repo's `dev-<n>` pre-releases, settings in `%APPDATA%\bhoptools-dev` (Linux
  `~/.config/bhoptools-dev`). The Options tab and the console say "bhoptools dev", and the version
  is `dev-<n> (<commit>)`.

Run one of them at a time.

The dev client is the same code built with `BHOPTOOLS_DEV` defined; without it nothing changes.
`.github/workflows/staging.yml` does the builds. `build-windows.yml` is the public repo's workflow
and is left as it is; switch it off here so pushes aren't built twice:

```
gh workflow disable build-windows.yml -R ddyfad/bhoptools-staging
```

## Tokens

**Dev client token.** The dev client reads this repo's releases, so it needs a token. Create a
fine-grained token (GitHub, Settings, Developer settings, Fine-grained tokens):

- Resource owner `ddyfad`, Repository access: Only select repositories, `ddyfad/bhoptools-staging`.
- Permissions: Contents read-only. Metadata read-only is added with it.

Save it as the only thing in `%APPDATA%\bhoptools-dev\github_token.txt`:

```powershell
New-Item -ItemType Directory -Force "$env:APPDATA\bhoptools-dev" | Out-Null
Set-Content -NoNewline "$env:APPDATA\bhoptools-dev\github_token.txt" "github_pat_..."
```

On Linux: `~/.config/bhoptools-dev/github_token.txt` (or `$XDG_CONFIG_HOME/bhoptools-dev/`).
Without it the dev client says where to put it and doesn't update. When it expires, the dev
client says it couldn't fetch the dev builds.

**PUBLIC_REPO_TOKEN.** Promote pushes to and releases on the public repo. Create a second
fine-grained token:

- Repository access: Only select repositories, `ddyfad/bhoptools`.
- Permissions: Contents read and write (pushes and releases), Workflows read and write (promote
  pushes commits that change `.github/workflows`, which GitHub refuses without it).

Store it as an Actions secret of this repo:

```
gh secret set PUBLIC_REPO_TOKEN -R ddyfad/bhoptools-staging
```

The dev releases themselves use the workflow's own `GITHUB_TOKEN`, nothing to set up.

## Dev releases

Every push to `sourcejump` or `dev/*` publishes the pre-release `dev-<run number>` with:

- `bhoptools-dev.exe`, `bhoptools-dev-linux.tar.gz`, `bhoptools-dev.so`, `bhoptools-dev`: the dev client;
- `public-bhoptools.exe`, `public-bhoptools-linux.tar.gz`, `public-bhoptools.so`, `public-bhoptools`:
  the public variant of the same commit, to try as public users would run it;
- notes: the commits since the previous dev release, then the branch and commit.

The newest 30 are kept; older ones are deleted with their tags. Other branches are built but not
released.

## First install

The dev client can't fetch itself the first time. Download it from the newest dev release:

```
gh release download dev-48 -R ddyfad/bhoptools-staging -p bhoptools-dev.exe -D C:\bhoptools-dev
```

Keep it in a folder of its own: it replaces itself there when it updates. Run it like
`bhoptools.exe`. On Linux, download `bhoptools-dev-linux.tar.gz`, extract it, run
`./bhoptools-dev` and paste the launch options it prints (they preload `bhoptools-dev.so`) into
Steam in place of the public ones.

From then on it offers each new dev build when it starts, like the public client does with
releases. Cancel in that popup (Linux: "Ignore forever") stops the offers until
`no_update_check` is deleted from the dev settings folder.

## Rolling back

In the game console:

- `bhoptools_builds` lists the last 20 dev builds with their notes, and marks the running one.
- `bhoptools_install dev-45` installs that build. Windows: the next time `bhoptools-dev.exe`
  starts. Linux: it's downloaded now and goes in when the game next starts.
- While the build `bhoptools_install` put in is running, newer builds aren't offered (it says so
  on start). `bhoptools_install latest` follows the newest again from the next start.

Only builds that still have a release (the newest 30) can be installed.

## Promoting

When a dev build is good: Actions, "Staging (dev builds, promote)", Run workflow, or

```
gh workflow run staging.yml -R ddyfad/bhoptools-staging --ref sourcejump \
  -f dev_tag=dev-48 -f version=1.13 -f notes="what changed" -f required_update=false -f prerelease=false
```

It:

1. checks the inputs, that `1.13` isn't released yet, and that the public `sourcejump` branch is
   part of `dev-48`'s history (fast-forward only; if public has commits staging doesn't, merge them
   here first and promote the dev build that makes);
2. builds the public variant from `dev-48`'s commit with `version.h` set to `1.13`;
3. pushes that commit to the public `sourcejump` branch;
4. publishes release `1.13` on ddyfad/bhoptools with `bhoptools.exe`, `bhoptools-linux.tar.gz`,
   `bhoptools.so` and `bhoptools`, and the notes in the format the public client reads (`[required]`
   when ticked, the notes, then `---` and the install text). The line under `---` records the commit and
   dev tag.

Merging public `sourcejump` into public `main` is still done there, by PR.

Everything in this repo goes public with the commit, this file and `staging.yml` included. The
workflow checks the repository name, so it does nothing in the public repo.

### Why promote rebuilds

The version is compiled in (`APP_VERSION` from `version.h`), and the client compares it with the
release tags to decide whether to update. A file built as `dev-48` would report `dev-48` and, on
the public side, always see `1.13` as newer than itself. So the dev release's `public-*` files
can't be published as they are, and patching the string into the binaries afterwards would be a
file nobody tested either.

Promote rebuilds the public variant from the exact commit that was tested, on the same runner
image and toolchain, with only `version.h` different. That's the same thing the public repo's own
`build-windows.yml` does for a release. The files aren't byte-identical to the dev release's
`public-*` files (the version string differs, and MSVC stamps a timestamp and PDB id into every
build anyway), but the source is.
