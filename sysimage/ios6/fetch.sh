#!/bin/bash
# Get the files of iOS 6.1.3 (10B329, iPhone 4 GSM) that the app edition's
# install pipeline is made of, straight from Apple's servers, the same
# contract as ../fetch.sh has for the 16.5 image of the native path: nothing
# of iOS is in this repository, your machine downloads it from Apple, to
# your machine only.
#
#   kernelcache.release.n90   the kernel, IMG3-wrapped, AES-256 encrypted
#   048-2748-005.dmg          the root filesystem, an encrcdsa-encrypted DMG
#                             (~860 MB), kept whole as the guest's future
#                             system volume -- this release's emulator has no
#                             HFSX driver yet, so the volume is installed,
#                             reported on (encrcdsa.py) and nothing more
#
# Why 6.1.3 and not 6.1.6: the roadmap said "6.1.6, exact build verified in
# stage 4" -- and the verification came out differently. Apple shipped 6.1.6
# only for the iPhone 3GS and the iPod touch 4G; the iPhone 4 (iPhone3,1,
# the machine this emulator is) ends at 6.1.3 (10B329). See README.md here.
#
#   KEEP=1   keep the partially-downloadable rootfs DMG across runs
#   KERNEL_ONLY=1   skip the ~860 MB rootfs (CI does; it needs no filesystem)
set -e
cd "$(dirname "$0")"

URL=https://secure-appldnld.apple.com/iOS6.1/091-2610.20130319.Bedr4/iPhone3,1_6.1.3_10B329_Restore.ipsw
SIZE=938891219
SHA1=f8c53328656f2708e1d2bcb793f12ae46dd4eb23
KERNEL=kernelcache.release.n90
ROOTFS=048-2748-005.dmg

command -v python3 >/dev/null || { echo "fetch.sh: python3 is not installed"; exit 1; }
command -v sha1sum >/dev/null || SHA1CHECK=skip

# the directory is read first: a changed member name fails here, before
# hundreds of megabytes have moved
echo "fetch.sh: reading the archive directory"
python3 ../fetch-from-ipsw.py --url "$URL" --size "$SIZE" --list > directory.txt
grep -q "^$KERNEL " directory.txt || { echo "fetch.sh: $KERNEL is not in the archive"; exit 1; }
grep -q "^$ROOTFS " directory.txt || { echo "fetch.sh: $ROOTFS is not in the archive"; exit 1; }

if [ ! -f .done-kernel ]; then
        echo "fetch.sh: fetching the kernel cache (7.4 MB)"
        python3 ../fetch-from-ipsw.py --url "$URL" --size "$SIZE" "$KERNEL" "$KERNEL.part"
        mv "$KERNEL.part" "$KERNEL"
        touch .done-kernel
fi
echo "fetch.sh: $KERNEL: $(wc -c < "$KERNEL") bytes, IMG3 -- the loader (native/image) reports the rest"

if [ -z "$KERNEL_ONLY" ] && [ ! -f .done-rootfs ]; then
        if [ ! -f "$ROOTFS" ]; then
                echo "fetch.sh: fetching the root filesystem (902 MB compressed, 860 MB as stored)"
                python3 ../fetch-from-ipsw.py --url "$URL" --size "$SIZE" "$ROOTFS" "$ROOTFS.part"
                mv "$ROOTFS.part" "$ROOTFS"
        fi
        echo "fetch.sh: $ROOTFS:"
        python3 encrcdsa.py --describe "$ROOTFS"
        touch .done-rootfs
        [ -n "$KEEP" ] || true    # the DMG stays: it is the guest's system volume
fi
echo "fetch.sh: done. The kernel cache is the install pipeline's input; the DMG is the guest's system volume, stored and reported, not unpacked (README.md)."
