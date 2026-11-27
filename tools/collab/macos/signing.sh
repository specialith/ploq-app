#!/bin/bash
# Sourced by build.sh and package.sh: make the Developer ID usable for codesign for the rest of the script.
#
# The identity lives in its own keychain (~/Library/Keychains/ploq-signing.keychain-db, password in
# ~/.ploq/keychain-pass). It is kept OUT of the user's keychain search list: apps that read the
# Keychain (Ploq's own sign-in) search every keychain in that list, and a locked signing keychain made
# macOS ask for its password and Ploq hang. codesign only trusts the identity while its keychain (which
# also holds Apple's Developer ID intermediate) is in the list, so it goes in now and the original list
# comes back when the script exits, even on failure.
PLOQ_KC="$HOME/Library/Keychains/ploq-signing.keychain-db"
if [ -f "$PLOQ_KC" ]; then
    [ -f "$HOME/.ploq/keychain-pass" ] && security unlock-keychain -p "$(cat "$HOME/.ploq/keychain-pass")" "$PLOQ_KC"
    PLOQ_KC_ORIG=()
    while IFS= read -r k; do
        k="${k#"${k%%[![:space:]]*}"}"; k="${k%\"}"; k="${k#\"}"
        [ -n "$k" ] && [ "$k" != "$PLOQ_KC" ] && PLOQ_KC_ORIG+=("$k")
    done < <(security list-keychains -d user)
    security list-keychains -d user -s "${PLOQ_KC_ORIG[@]}" "$PLOQ_KC"
    trap 'security list-keychains -d user -s "${PLOQ_KC_ORIG[@]}"' EXIT
fi
