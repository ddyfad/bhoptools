# Staging

This repo (ddyfad/bhoptools-staging, private) is where client changes are built and tried before
they reach ddyfad/bhoptools. There are two clients:

- **bhoptools**: the public client. Updates from the public repo's releases, settings in
  `%APPDATA%\bhoptools` (Linux `~/.config/bhoptools`).
- **bhoptools-dev**: built from this repo on every push to `main`. Updates from this
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

Every push to `main` publishes the pre-release `dev-<run number>` with:

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

A public release takes two runs of "Staging (dev builds, promote)", with the release key signing
in between. Nothing goes public until the second run has checked the signature.

**Step 1: build.** When a dev build is good (cmd.exe, one line):

```
gh workflow run staging.yml -R ddyfad/bhoptools-staging --ref main -f dev_tag=dev-48 -f version=1.13 -f notes="what changed" -f required_update=false -f prerelease=false
```

`-f leave_out=rawprecise` builds the public files without a feature (space separated for more). Each
name `x` is the define `BHOPTOOLS_NO_X`, and the run fails if no client code checks it. The dev
client always has everything.

It:

1. checks the inputs, that `1.13` isn't released yet, and that the public `main` branch is
   part of `dev-48`'s history (fast-forward only; if public has commits staging doesn't, merge them
   here first and promote the dev build that makes);
2. builds the public variant from `dev-48`'s commit with `version.h` set to `1.13`;
3. writes `SHA256SUMS`: the line `# bhoptools release 1.13`, then the sha256 of `bhoptools.exe`,
   `bhoptools-linux.tar.gz`, `bhoptools.so` and `bhoptools`;
4. keeps the files, `SHA256SUMS`, the notes and the inputs as the run's `release` artifact, and
   puts the commands for the next steps, with its run id filled in, in the run's summary.

**Step 2: sign** `SHA256SUMS` with the YubiKey. In cmd.exe, with the run id from step 1 (or
`gh run list -R ddyfad/bhoptools-staging -w staging.yml -L 5`):

```
gh run download <run id> -R ddyfad/bhoptools-staging -n release -D %USERPROFILE%\bhoptools-release\1.13
type %USERPROFILE%\bhoptools-release\1.13\out\SHA256SUMS
ssh-keygen -Y sign -n bhoptools-release -f %USERPROFILE%\.ssh\bhoptools-release %USERPROFILE%\bhoptools-release\1.13\out\SHA256SUMS
```

Check that the first line says the version you mean before signing. `ssh-keygen` asks for the
PIN and a touch, and writes `SHA256SUMS.sig` next to it. Optionally check it, from a checkout of
this repo:

```
ssh-keygen -Y verify -f .github\release_signers -I release -n bhoptools-release -s %USERPROFILE%\bhoptools-release\1.13\out\SHA256SUMS.sig < %USERPROFILE%\bhoptools-release\1.13\out\SHA256SUMS
```

**Step 3: publish.**

```
gh workflow run staging.yml -R ddyfad/bhoptools-staging --ref main -f build_run=<run id> -F sums_signature=@%USERPROFILE%\bhoptools-release\1.13\out\SHA256SUMS.sig
```

`-F ...=@file` makes gh read the value from the file. The web form works too (paste the whole
`.sig`; it joins the lines, and the workflow puts them back). The run:

1. takes the files from step 1's `release` artifact (they're not rebuilt, so they're the files
   that were signed);
2. checks `SHA256SUMS.sig` with `ssh-keygen -Y verify` against `.github/release_signers`, then
   with the client's own check (`client/tests/check_sums`): a pinned key, the YubiKey's
   user-presence and PIN flags, the version line, and every file's sha256;
3. pushes the commit to the public `main` branch;
4. publishes release `1.13` on ddyfad/bhoptools with the four files, `SHA256SUMS` and
   `SHA256SUMS.sig`, and the notes in the format the public client reads (`[required]` when ticked,
   the notes, then `---` and the install text). The line under `---` records the commit and dev
   tag.

The public repo's own `build-windows.yml` has the same two steps: `release_tag` (step 1), then
`build_run` and `sums_signature` (`gh workflow run build-windows.yml -R ddyfad/bhoptools ...`).


Everything in this repo goes public with the commit, this file and `staging.yml` included. The
workflow checks the repository name, so it does nothing in the public repo.

### What the client checks

From the first release built with this, the public client installs an update only when:

- the release has `SHA256SUMS` and `SHA256SUMS.sig`;
- the signature is SSHSIG in namespace `bhoptools-release`, from one of the keys pinned in
  `client/shared/release_sig.cpp` (`kReleaseKeys`), and for the YubiKey key with both the
  user-presence and user-verification (PIN) flags set;
- the first line of `SHA256SUMS` is `# bhoptools release <the release's tag>`, so a signed
  `SHA256SUMS` from an older release can't be put on a newer tag;
- the downloaded `bhoptools.exe` (Windows) or `bhoptools.so` and `bhoptools` (Linux) have the
  sha256 it lists.

It checks before asking, so nobody is offered a release that won't install; a release that fails
is logged (console / `[bhoptools]`) and shown in a popup, and the current version starts. A
`[required]` release gets no exception.

Clients older than this don't check anything, so the first signed release reaches them as any
release did. Every release after it must be signed or nobody on a current version gets it.

### Release keys

The release key is on a YubiKey (`sk-ssh-ed25519@openssh.com`, application
`ssh:bhoptools-release`, made with `verify-required`); the private key never touches GitHub or
this repo. The keys are pinned in two places that must agree: `kReleaseKeys` in
`client/shared/release_sig.cpp` (what clients trust) and `.github/release_signers` (what the
workflows check with). Both have a marked slot for a backup key. A backup YubiKey key must also
be made with `-O verify-required`, or the client refuses its signatures; a plain `ssh-ed25519`
key has no flags to check.

Changing the keys only takes effect for clients that have installed a release with the new list,
so sign the release that changes them with a key the previous release already trusts.

### Dev builds

Dev builds are not signed: they come from this private repo through a token, and signing every
push with the YubiKey would make them useless. `bhoptools-dev` checks each download's size and
sha256 against what the GitHub API lists for the asset (`size`, `digest`), and logs when the API
gives no digest. That catches a broken or swapped download. It does not protect against anyone who
can publish releases in this repo (a leaked `GITHUB_TOKEN` in a workflow, a compromised account or
PAT with write access to `ddyfad/bhoptools-staging`), or against the API itself: whoever can upload
an asset gets a matching digest from GitHub. Only the people running `bhoptools-dev` are exposed.

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
