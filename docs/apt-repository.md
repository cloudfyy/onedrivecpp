# GitHub Pages APT repository

The `APT repository` workflow publishes signed Ubuntu `noble` and `resolute`
repositories at <https://cloudfyy.github.io/onedrivecpp/>. Only `amd64` packages
are currently built. It runs after a stable GitHub Release is published, or
manually on the default branch. Drafts, prereleases, and ordinary pushes do not
deploy packages. Publishing a release requires the packaging review described
in the README; this workflow does not rebuild or approve release binaries.

## One-time maintainer setup

1. Enable **Settings → Pages → Build and deployment → Source: GitHub Actions**.
2. Create a dedicated OpenPGP signing key on a trusted machine, not a CI runner:

   ```bash
   gpg --quick-generate-key "onedrive-cpp archive signing" ed25519 sign 2y
   gpg --list-secret-keys --keyid-format long --with-fingerprint
   ```

3. Use the complete primary-key fingerprint to export the private key directly
   into an Actions secret, without creating a private-key file in the checkout:

   ```bash
   gpg --armor --export-secret-keys YOUR_FULL_FINGERPRINT |
     gh secret set APT_SIGNING_KEY --repo cloudfyy/onedrivecpp
   gh secret set APT_SIGNING_PASSPHRASE --repo cloudfyy/onedrivecpp
   gh variable set APT_SIGNING_KEY_FINGERPRINT \
     --repo cloudfyy/onedrivecpp --body YOUR_FULL_FINGERPRINT
   ```

   The second command prompts for the key's passphrase; omit that secret only
   for an unencrypted key. Never paste private keys or passphrases into issues,
   chat, logs, or tracked files. Keep a secure offline backup and revocation
   certificate, and publish the public fingerprint through an independent
   trusted channel so users can verify it.
4. Ensure the `github-pages` deployment environment permits the default branch
   for manual runs and release tags (`v*`) for release-triggered runs. Consider
   required reviewers, since publishing changes software users install.
5. After merging the workflow into the default branch, run it once:

   ```bash
   gh workflow run apt-repository.yml --repo cloudfyy/onedrivecpp --ref main
   ```

   The first deployment also imports existing stable releases. A failure leaves
   the last successfully deployed site in place.

See the English or Chinese README for client installation commands. Clients use
a dedicated `signed-by` keyring; do not use `apt-key`, `trusted=yes`, or disable
signature checks.

## Publication and retention

Every deployment reconstructs the repository from **all currently published
stable releases** using the GitHub API, with pagination. Each release must have
`SHA256SUMS` and matching `onedrive-cpp_*.deb` assets. Missing or mismatched
checksums fail publication. Checksums guard against accidental corruption;
release permissions and review are still the trust boundary for those binaries.

Package contents determine the Ubuntu suite and architecture. Both historical
`.ubuntu24.04` / `.ubuntu26.04` and current `~ubuntu24.04` / `~ubuntu26.04` version
suffixes are accepted. Renamed or unsupported packages and conflicting contents
for an existing package filename fail publication. Debug packages and source
archives remain in GitHub Releases, not the APT site.

The site includes `Packages`, `Packages.gz`, SHA256 by-hash indexes, `Release`,
`InRelease`, `Release.gpg`, and the ASCII-armored public key. Both suites must be
present before deployment. No signing secret is included in the Pages artifact.
CI checks use disposable keys and packages, verify both signature forms, and run
an isolated APT update to check version selection and suite isolation.

Old packages are retained while their releases remain published. Do not delete
or mutate published release assets casually: the next deployment mirrors those
changes. GitHub Pages storage and bandwidth limits still apply. Monitor site
size as releases accumulate and move hosting or explicitly define a retention
policy before reaching those limits. Do not silently drop old packages.

APT clients updating during a site replacement can need to retry if an older
index's by-hash URL is no longer present. The repository has no `Valid-Until`
expiry so a quiet release schedule does not break installation; consequently,
signatures alone do not provide time-based protection against replaying an old
repository snapshot.

## Key renewal and rotation

The setup command creates a key valid for two years (`2y`). Using `0` instead
would create a key without an expiry date, but a finite lifetime is recommended.
Monitor the expiry and renew before it is reached. Expiry does not remove
installed software, but can prevent APT from accepting repository updates.

To extend the same key for another two years from the renewal date, run these
commands on the trusted machine holding the private key:

```bash
FINGERPRINT="YOUR_FULL_FINGERPRINT"
gpg --list-secret-keys --with-fingerprint "$FINGERPRINT"
gpg --quick-set-expire "$FINGERPRINT" 2y
set -o pipefail
gpg --armor --export-secret-keys "$FINGERPRINT" |
  gh secret set APT_SIGNING_KEY --repo cloudfyy/onedrivecpp
gh workflow run apt-repository.yml --repo cloudfyy/onedrivecpp --ref main
```

The fingerprint stays the same, so the repository variable does not change.
Update `APT_SIGNING_PASSPHRASE` only if the passphrase has changed. Wait for the
deployment to succeed, then notify users to refresh their installed public key:
the expiry information is also part of that key.

Clients can download the renewed key, inspect its fingerprint and expiry, and
install it after confirming the fingerprint still matches their trusted key:

```bash
curl -fsSLo onedrive-cpp.asc https://cloudfyy.github.io/onedrivecpp/onedrive-cpp.asc
gpg --show-keys --with-fingerprint onedrive-cpp.asc
sudo install -m 0644 onedrive-cpp.asc /etc/apt/keyrings/onedrive-cpp.asc
sudo apt update
```

For a different key, distribute the replacement public key through a trusted
channel and update client keyrings **before** switching the workflow's signing
key and fingerprint. Existing clients will reject an unannounced replacement.

The workflow imports the key into a temporary GnuPG home, uses the passphrase
only through standard input, and removes that home before uploading the site.
Never enable shell tracing around secret handling.
