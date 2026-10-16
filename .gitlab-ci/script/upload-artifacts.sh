#!/bin/bash

set -xeu -o pipefail

# turn grub-2.XX-rcY into grub-2.XX~rcY
GRUB_CI_COMMIT_TAG="$(echo "$CI_COMMIT_TAG" | sed 's/\([^-]*-[^-]*\)-/\1~/')"

cd artifacts

# Upload zip and tarballs
for f in $GRUB_CI_COMMIT_TAG-for-windows.zip $GRUB_CI_COMMIT_TAG.tar.gz $GRUB_CI_COMMIT_TAG.tar.xz; do
	 curl --header "JOB-TOKEN: $CI_JOB_TOKEN" \
	      --fail \
	      --upload-file "$f" \
	      "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/$f"  | tee upload.txt
	 grep '201 Created' upload.txt
done

# Upload the corresponding sig files
for f in $GRUB_CI_COMMIT_TAG-for-windows.zip.sig $GRUB_CI_COMMIT_TAG.tar.gz.sig $GRUB_CI_COMMIT_TAG.tar.xz.sig; do
	 curl --header "JOB-TOKEN: $CI_JOB_TOKEN" \
	      --fail \
	      --upload-file "$f" \
	      "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/$f"  | tee upload.txt
	 grep '201 Created' upload.txt
done
