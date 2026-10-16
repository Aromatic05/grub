#!/bin/sh

set -xeu

RC="rc"
GRUB_CI_COMMIT_TAG="$(echo "$CI_COMMIT_TAG" | sed 's/\([^-]*-[^-]*\)-/\1~/')"

TAR_XZ_NAME="${GRUB_CI_COMMIT_TAG}.tar.xz"
TAR_XZ_SIG_NAME="${GRUB_CI_COMMIT_TAG}.tar.xz.sig"
TAR_XZ_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${TAR_XZ_NAME}"
TAR_XZ_SIG_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${TAR_XZ_SIG_NAME}"

TAR_GZ_NAME="${GRUB_CI_COMMIT_TAG}.tar.gz"
TAR_GZ_SIG_NAME="${GRUB_CI_COMMIT_TAG}.tar.gz.sig"
TAR_GZ_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${TAR_GZ_NAME}"
TAR_GZ_SIG_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${TAR_GZ_SIG_NAME}"

WIN_ZIP_NAME="${GRUB_CI_COMMIT_TAG}-for-windows.zip"
WIN_ZIP_SIG_NAME="${GRUB_CI_COMMIT_TAG}-for-windows.zip.sig"
WIN_ZIP_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${WIN_ZIP_NAME}"
WIN_ZIP_SIG_URL="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/source-assets/${CI_COMMIT_TAG}/${WIN_ZIP_SIG_NAME}"

cat > release-notes.md <<EOF
The GRUB maintainers are proud to announce \`$GRUB_CI_COMMIT_TAG\` has been released.

We would like to thank everyone who contributed to the project.

The tarball is available at [$TAR_XZ_NAME]($TAR_XZ_URL) and its signature at [$TAR_XZ_SIG_NAME]($TAR_XZ_SIG_URL)

Release is signed with the following fingerprint:
\`\`\`
  $GPG_FINGERPRINT
\`\`\`

It's also available as a signed \`$CI_COMMIT_TAG\` tag in the official git repository.

If you do not have xz support alternatively you may consider
[$TAR_GZ_NAME]($TAR_GZ_URL) and its signature at [$TAR_GZ_SIG_NAME]($TAR_GZ_SIG_URL)

If you want a binary version for Windows (i386-pc, i386-efi and x86_64-efi flavors) it is
available under [$WIN_ZIP_NAME]($WIN_ZIP_URL) and its signature at [$WIN_ZIP_SIG_NAME]($WIN_ZIP_SIG_URL)

EOF

if echo "$CI_COMMIT_TAG" | grep -q "$RC"; then
    cat >> release-notes.md <<EOF
Please test this release candidate. If we do not spot major issues we are going to the final release in a week or so.

In the meantime we will focus mostly on patches fixing tests and documentation before the final release. Once we have a release,
the standard development workflow continues.
EOF
fi

release-cli create \
 --name "$CI_COMMIT_TAG" \
 --tag-name "$CI_COMMIT_TAG" \
 --description "./release-notes.md" \
 --assets-link "{\"name\":\"${TAR_XZ_NAME}\",\"url\":\"${TAR_XZ_URL}\",\"link_type\":\"package\"}" \
 --assets-link "{\"name\":\"${TAR_GZ_NAME}\",\"url\":\"${TAR_GZ_URL}\",\"link_type\":\"package\"}" \
 --assets-link "{\"name\":\"${WIN_ZIP_NAME}\",\"url\":\"${WIN_ZIP_URL}\",\"link_type\":\"package\"}" \
 --assets-link "{\"name\":\"${TAR_XZ_SIG_NAME}\",\"url\":\"${TAR_XZ_SIG_URL}\",\"link_type\":\"other\"}" \
 --assets-link "{\"name\":\"${TAR_GZ_SIG_NAME}\",\"url\":\"${TAR_GZ_SIG_URL}\",\"link_type\":\"other\"}" \
 --assets-link "{\"name\":\"${WIN_ZIP_SIG_NAME}\",\"url\":\"${WIN_ZIP_SIG_URL}\",\"link_type\":\"other\"}"
