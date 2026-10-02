# Project instructions

## Approved GPG key exception

The PPA signing key leaves the personal keyring as a CI secret. The user approved this on
2026-10-03.

- Key: `54E3 4439 8C87 82B1 EB60 767B 6576 48BA 04CD D7C7`, "Will Rouesnel (ks3fs PPA uploads
  from GitHub Actions)". It is a signing key used only for uploads to `ppa:w-rouesnel/ks3fs`
  and is registered on Launchpad as `~w-rouesnel`.
- It is exported only to the GitHub repository secrets `PPA_GPG_PRIVATE_KEY` and
  `PPA_GPG_PASSPHRASE` of `wrouesnel/ks3fs`, which the `ppa` workflow uses to sign source
  uploads on release tags.
- The passphrase lives in the login keyring:
  `secret-tool lookup service ks3fs-ppa key 54E344398C8782B1EB60767B657648BA04CDD7C7`.
- This exception covers that key and those secrets only. Any other export of a release key still
  needs the user's approval first.
