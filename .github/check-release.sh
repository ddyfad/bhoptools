#!/bin/bash
# bash .github/check-release.sh <dir> <tag>, from the repository root, before a client release
# is published: writes $SUMS_SIGNATURE to <dir>/SHA256SUMS.sig, checks it against
# .github/release_signers with ssh-keygen, then runs the client's own check (check_sums: the
# pinned keys, the security key's PIN flag, the tag and every file). See STAGING.md.
set -euo pipefail
dir=$1
tag=$2
root=$PWD
assets="bhoptools.exe bhoptools-linux.tar.gz bhoptools.so bhoptools"

# The web form joins the signature's lines into one; put the armour back as ssh-keygen wants it.
b64=$(printf '%s' "${SUMS_SIGNATURE:-}" | sed 's/-----BEGIN SSH SIGNATURE-----//; s/-----END SSH SIGNATURE-----//' | tr -d ' \r\n\t')
[ -n "$b64" ] || { echo "::error::sums_signature is empty"; exit 1; }
{ echo '-----BEGIN SSH SIGNATURE-----'; echo "$b64" | fold -w 70; echo '-----END SSH SIGNATURE-----'; } > "$dir/SHA256SUMS.sig"

[ "$(head -n 1 "$dir/SHA256SUMS")" = "# bhoptools release $tag" ] \
	|| { echo "::error::SHA256SUMS is for '$(head -n 1 "$dir/SHA256SUMS")', not $tag"; exit 1; }
listed=$(tail -n +2 "$dir/SHA256SUMS" | awk '{ sub(/^[*]/, "", $2); print $2 }' | sort | tr '\n' ' ')
want=$(printf '%s\n' $assets | sort | tr '\n' ' ')
[ "$listed" = "$want" ] || { echo "::error::SHA256SUMS lists '$listed', expected '$want'"; exit 1; }

principals=$(ssh-keygen -Y find-principals -f .github/release_signers -s "$dir/SHA256SUMS.sig") \
	|| { echo "::error::SHA256SUMS.sig isn't from a key in .github/release_signers"; exit 1; }
ssh-keygen -Y verify -f .github/release_signers -I "${principals%%$'\n'*}" -n bhoptools-release \
	-s "$dir/SHA256SUMS.sig" < "$dir/SHA256SUMS" \
	|| { echo "::error::SHA256SUMS.sig doesn't verify"; exit 1; }

make -s -C client/tests check_sums
(cd "$dir" && "$root/client/tests/check_sums" SHA256SUMS SHA256SUMS.sig "$tag" $assets) \
	|| { echo "::error::the client wouldn't install this release (see above)"; exit 1; }
