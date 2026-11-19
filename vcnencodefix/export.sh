#!/bin/zsh
# export.sh <repo> - copies the VCNEncodeFix source and the last build into <repo>/vcnencodefix of the
# vcn-video-decoders repository, whose installer app embeds it. Run ./build.sh first.
#
# <repo>/vcnencodefix/ gets the source (no build/ or dist/), and prebuilt/VCNEncodeFix-<version>.zip, the
# package built here, which installer/build-app.sh uses when the kext toolchain (Lilu + MacKernelSDK) is not
# available, as on the release CI. Older prebuilt zips are removed from there.
emulate -L zsh
setopt err_exit no_unset
here=${0:A:h}
repo=${1:?usage: export.sh <vcn-video-decoders repo>}
[[ -d $repo/installer && -d $repo/vtav1 ]] || { print "$repo is not the vcn-video-decoders repository"; exit 1; }
version=$(<$here/VERSION)
zip=$here/dist/VCNEncodeFix-$version.zip
[[ -f $zip ]] || { print "build first: $zip is missing"; exit 1; }
dst=$repo/vcnencodefix
mkdir -p $dst
if [[ ${dst:A} == ${here:A} ]]; then  # run from inside the repository: the source is already there
  rm -f $dst/prebuilt/VCNEncodeFix-*.zip(N)
  mkdir -p $dst/prebuilt && cp $zip $dst/prebuilt/
  print "refreshed prebuilt/${zip:t} in $dst"
  exit 0
fi
for d in kext tool tests scripts launchd newsyslog vtenc; do
  rm -rf $dst/$d
  ditto --norsrc --noextattr $here/$d $dst/$d
done
cp $here/build.sh $here/export.sh $here/README.md $here/CHANGELOG.md $here/VERSION $dst/
mkdir -p $dst/prebuilt
rm -f $dst/prebuilt/VCNEncodeFix-*.zip(N)
cp $zip $dst/prebuilt/
find $dst -name .DS_Store -delete
print "exported VCNEncodeFix $version source and prebuilt/${zip:t} to $dst"
