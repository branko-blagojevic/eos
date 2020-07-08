#!/bin/bash

#-------------------------------------------------------------------------------
# Publish debian artifacts on CERN Gitlab CI
# Author: Jozsef Makai <jmakai@cern.ch> (11.08.2017)
#-------------------------------------------------------------------------------

set -e

script_loc=$(dirname "$0")
prefix=$1
component=$2

for artifacts_dir in *_artifacts; do
  dist=${artifacts_dir%_*}
  path=$prefix/pool/$dist/$component/e/eos/
  mkdir -p $path

  echo "Publishing for $dist -- $path"

  cp ${dist}_artifacts/*.deb $path
  $script_loc/generate_debian_metadata.sh $prefix $dist $component amd64
  echo "CI_COMMIT_TAG: $CI_COMMIT_TAG - CI_COMMIT_SHORT_SHA : $CI_COMMIT_SHORT_SHA"
  if [[ -n "$CI_COMMIT_TAG" ]]; then echo "true"; $script_loc/sign_debian_repository.sh $prefix $dist; fi
done
