#!/usr/bin/env bash
# CI orchestration only: invoke the selected revision's build.sh without editing it.
set -euo pipefail
SOURCE=$(realpath "${1:?driver checkout}")
OUT=$(realpath -m "${2:?output directory}")
CONTROL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
mkdir -p "$OUT" "$OUT/packages" "$OUT/verification" "$OUT/logs"
START=$(date +%s)
SAMPLER=
finish() {
  local status=$?
  if [[ -n "$SAMPLER" ]]; then kill "$SAMPLER" 2>/dev/null || :; wait "$SAMPLER" 2>/dev/null || :; fi
  printf 'elapsed_seconds=%s\nexit_status=%s\n' "$(( $(date +%s) - START ))" "$status" > "$OUT/timing.txt"
  df -B1 "$SOURCE" >> "$OUT/disk-final.txt"
  if [[ -f "$OUT/logs/build.log" ]]; then
    rg -n -i 'warning:|deprecated|WARNING:' "$OUT/logs/build.log" > "$OUT/logs/warnings.txt" || :
  fi
  if [[ -f "$GITHUB_STEP_SUMMARY" || -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
      echo "## $VARIANT / $CODE_SHA"
      echo "Build and verification exit status: $status; elapsed: $(( $(date +%s) - START )) seconds."
      echo '```'
      [[ ! -f "$OUT/SHA256SUMS" ]] || cat "$OUT/SHA256SUMS"
      echo '```'
      echo 'Source-built package, labeled installable ZIP, verification, warnings, provenance and logs are in the 7-day artifact.'
    } >> "$GITHUB_STEP_SUMMARY"
  fi
}
trap finish EXIT
CODE_SHA=$(git -C "$SOURCE" rev-parse HEAD)
export CODE_SHA
[[ "$VARIANT" != baseline || "$CODE_SHA" == fe9b8943aaeec11ed9fccbfcc382da07379afbe5 ]]
git -C "$SOURCE" diff --exit-code HEAD --
{
  echo "code_commit=$CODE_SHA"
  echo "workflow_commit=$GITHUB_SHA"
  echo "variant=$VARIANT"
  echo "run_url=https://github.com/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID"
  echo "run_attempt=$GITHUB_RUN_ATTEMPT"
  echo "runner_os=$RUNNER_OS runner_arch=$RUNNER_ARCH runner_image=${ImageOS:-unknown}/${ImageVersion:-unknown}"
  uname -a
  cat "$ANDROID_NDK_HOME/source.properties"
  "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version
  meson --version
  ninja --version
  glslangValidator --version
  python --version
  python -m pip freeze
  free -b
  df -B1 "$SOURCE"
} > "$OUT/provenance.txt" 2>&1
# Bound build parallelism without changing upstream build.sh or its Meson configuration.
mkdir -p "$OUT/wrappers"
printf '#!/bin/sh\nexec /usr/bin/ninja -j3 "$@"\n' > "$OUT/wrappers/ninja"
chmod +x "$OUT/wrappers/ninja"
export PATH="$OUT/wrappers:$PATH"
# Samples runner filesystem used bytes, not an assertion about exact process peak disk/RSS.
(while :; do date -u +%FT%TZ; df -B1 --output=used,avail "$SOURCE" | tail -1; sleep 15; done) > "$OUT/disk-samples.txt" &
SAMPLER=$!
cd "$SOURCE"
/usr/bin/time -v sh ./build.sh "$ANDROID_NDK_HOME" 2>&1 | tee "$OUT/logs/build.log"
git diff --exit-code HEAD --
# Preserve the exact upstream packaging output as well as a distinctly named test ZIP.
mkdir -p "$OUT/upstream-package"
cp dist/*.zip "$OUT/upstream-package/"
curl --fail --location --retry 3 --output "$OUT/verification/official-1.9.zip" \
  https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0-RC1/radv-xclipse-27.0.0-devel-fe9b894.zip
printf '9325769617812ddca2051c40364e33a2d85c6b3fa8c6c3e6208eb251549d4745  %s\n' "$OUT/verification/official-1.9.zip" | sha256sum --check
python "$CONTROL/verify-package.py" \
  --package "$(printf '%s\n' dist/*.zip)" \
  --reference "$OUT/verification/official-1.9.zip" \
  --out "$OUT" --variant "$VARIANT" --source "$SOURCE" \
  --readelf "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
# Keep the full build log; don't upload the build tree, NDK, source, or game data.
rm -rf "$OUT/wrappers"
cd "$OUT"
sha256sum packages/*.zip upstream-package/*.zip > SHA256SUMS
