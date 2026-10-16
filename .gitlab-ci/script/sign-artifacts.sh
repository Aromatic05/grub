#!/bin/bash

set -eu -o pipefail

# 2. Decode and import the private key non-interactively
printf '%s' "$GPG_PRIVATE_KEY" | base64 -d | gpg --batch --import -

# turn grub-2.XX-rcY into grub-2.XX~rcY as in configurea.ac
GRUB_CI_COMMIT_TAG="$(echo "$CI_COMMIT_TAG" | sed 's/\([^-]*-[^-]*\)-/\1~/')"

cd artifacts
gpg --batch --pinentry-mode loopback --passphrase-file "$GPG_PASSPHRASE_FILE" --detach-sign $GRUB_CI_COMMIT_TAG-for-windows.zip
gpg --batch --pinentry-mode loopback --passphrase-file "$GPG_PASSPHRASE_FILE" --detach-sign $GRUB_CI_COMMIT_TAG.tar.gz
gpg --batch --pinentry-mode loopback --passphrase-file "$GPG_PASSPHRASE_FILE" --detach-sign $GRUB_CI_COMMIT_TAG.tar.xz
