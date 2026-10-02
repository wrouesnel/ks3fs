# Project instructions

## Approved GPG key exception

PPA uploads are signed in CI with the user's shared Launchpad signing key. The user approved
exporting it to this repository's CI secrets on 2026-10-03.

- Key: `2A12 8435 A6FE 8BD7 51AA 5787 2095 9AB8 0709 6ADB`, "Will Rouesnel (GPG key for launchpad
  signing)", registered on Launchpad as `~w-rouesnel` and used for all of their PPAs, here
  `ppa:w-rouesnel/ks3fs`.
- It is exported only to the GitHub repository secrets `PACKAGE_SIGNING_KEY` and
  `PACKAGE_SIGNING_KEY_PASSPHRASE` of `wrouesnel/ks3fs`, with the fingerprint in the repository
  variable `PACKAGE_SIGNING_KEY_FINGERPRINT`. The `ppa` workflow uses them to sign source uploads
  on release tags.
- The passphrase lives in the login keyring and is looked up only as
  `secret-tool lookup service gpg-passphrase fingerprint 2A128435A6FE8BD751AA578720959AB807096ADB`.
  Never print it.
- The earlier per-project key `54E3 4439 8C87 82B1 EB60 767B 6576 48BA 04CD D7C7` (signed the
  0.3.0 upload) is no longer used. It stays in the keyring and on Launchpad; changing or revoking
  it needs the user's OK.
- This exception covers the shared key and those secrets only. Any other export of a release key
  still needs the user's approval first.
