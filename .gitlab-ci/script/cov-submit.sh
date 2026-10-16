#!/bin/bash

set -xeu -o pipefail

if [ ! -d "${COVERITY_DIR}" ]; then
    echo "No ${COVERITY_DIR} directory found!"
    exit 1
fi

# Set the CURL variables
version="$(git describe --tags --always)"
date="$(date +%Y-%m-%d)"
tarball=grub-master-$date-$version.tgz
description="Coverity Scan - $date - $version"

# generate the tarball from previous stage
tar czf $tarball $COVERITY_DIR

curl --form token=$COVERITY_SCAN_TOKEN \
  --form email=$COVERITY_SCAN_EMAIL \
  --form file=@$tarball \
  --form version=$version \
  --form description="$description" \
  https://scan.coverity.com/builds?project=GRUB
