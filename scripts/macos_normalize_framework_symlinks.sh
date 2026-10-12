#!/usr/bin/env bash
set -euo pipefail

frameworks="${1:?usage: macos_normalize_framework_symlinks.sh <Contents/Frameworks>}"
frameworks="$(realpath -m "$frameworks")"

if [[ ! -d "$frameworks" ]]; then
  echo "error: framework directory does not exist: $frameworks" >&2
  exit 1
fi

# Nix can encode a framework's Versions/Current (or another internal alias) as
# an absolute symlink back into /nix/store. Map any such link to the equivalent
# path in the copied Contents/Frameworks tree. Do this for the entire tree
# before resolving any relative aliases: a normal top-level link such as
# QtGui -> Versions/Current/QtGui may depend on Current being fixed first.
while IFS= read -r -d '' framework_link; do
  link_target="$(readlink "$framework_link")"
  case "$link_target" in
    /nix/store/*.framework/*)
      source_framework="${link_target%%.framework/*}.framework"
      target_framework_name="${source_framework##*/}"
      target_suffix="${link_target#"$source_framework"/}"
      local_target="$frameworks/$target_framework_name/$target_suffix"
      relative_target="$(realpath -m --relative-to="$(dirname "$framework_link")" "$local_target")"
      ln -sfn "$relative_target" "$framework_link"
      ;;
  esac
done < <(find "$frameworks" -type l -print0)

# After normalization, every framework symlink must resolve somewhere under
# Contents/Frameworks. This catches both residual /nix/store links and any
# unexpected absolute/relative escape from the application bundle.
bad=0
while IFS= read -r -d '' framework_link; do
  link_target="$(readlink "$framework_link")"
  resolved_link="$(realpath -m "$(dirname "$framework_link")/$link_target")"
  case "$resolved_link" in
    "$frameworks"|"$frameworks"/*) ;;
    *)
      echo "error: copied Qt framework symlink escapes app Frameworks: $framework_link -> $link_target ($resolved_link)" >&2
      bad=1
      ;;
  esac
done < <(find "$frameworks" -type l -print0)

exit "$bad"
