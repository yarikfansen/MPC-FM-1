#!/bin/sh
# Writes docs/mockups/fm1-build.js from app/build_id.h so mockups show the real version.
tag=$(sed -n 's/.*FM1_VERSION_TAG "\(.*\)"/\1/p' app/build_id.h)
id=$(sed -n 's/.*FM1_BUILD_ID "\(.*\)"/\1/p' app/build_id.h)
rel=$(sed -n 's/.*FM1_RELEASE_VERSION "\(.*\)"/\1/p' app/version.h)
printf 'window.FM1_BUILD={tag:"%s",id:"%s",base:"%s"};\n' "${tag:-BETA}" "$id" "$rel" > docs/mockups/fm1-build.js
