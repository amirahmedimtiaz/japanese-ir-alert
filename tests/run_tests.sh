#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/japanese-ir-alert.XXXXXX")
STATE_FILE="$TMP_DIR/seen.txt"
TANAKEN_FIXTURE="file://$ROOT/tests/fixtures/tanaken.html"
TANAKEN_NEW_FIXTURE="file://$ROOT/tests/fixtures/tanaken_new.html"
INUNEKO_FIXTURE="file://$ROOT/tests/fixtures/inuneko.jsonp"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

run_checker() {
    tanaken_url=$1
    shift
    TANAKEN_URL="$tanaken_url" \
    INUNEKO_URL="https://corp.inuneko-seikatsu.co.jp/ir/" \
    INUNEKO_API_URL="$INUNEKO_FIXTURE" \
    STATE_FILE="$STATE_FILE" \
    "$ROOT/tanaken-alert" "$@"
}

output=$(run_checker "$TANAKEN_FIXTURE" --dry-run)
case "$output" in
    *"Found 4 announcement(s)."*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "fixture parser test failed" >&2
        exit 1
        ;;
esac
case "$output" in
    *"[Inuneko Seikatsu] 2026/07/26 09:00:00 | 犬猫生活テスト"*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "Inuneko JSON test failed" >&2
        exit 1
        ;;
esac
case "$output" in
    *"Fixture TANAKEN New Item"*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "TANAKEN HTML test failed" >&2
        exit 1
        ;;
esac

run_checker "$TANAKEN_FIXTURE" --initialize >/dev/null
test -s "$STATE_FILE"

output=$(run_checker "$TANAKEN_FIXTURE")
case "$output" in
    *"No new announcements."*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "state repeat test failed" >&2
        exit 1
        ;;
esac

output=$(run_checker "$TANAKEN_FIXTURE" --test-latest --dry-run)
case "$output" in
    *"Latest announcement: 2026/07/26 09:00:00 | 犬猫生活テスト"*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "latest announcement test failed" >&2
        exit 1
        ;;
esac
case "$output" in
    *"inuneko-new.pdf"*) ;;
    *)
        printf '%s\n' "$output"
        printf '%s\n' "latest announcement URL test failed" >&2
        exit 1
        ;;
esac

before=$(cksum "$STATE_FILE")
set +e
failure_output=$(env -u SMTP_URL -u SMTP_USERNAME -u SMTP_PASSWORD -u ALERT_FROM -u ALERT_TO \
    TANAKEN_URL="$TANAKEN_NEW_FIXTURE" \
    INUNEKO_URL="https://corp.inuneko-seikatsu.co.jp/ir/" \
    INUNEKO_API_URL="$INUNEKO_FIXTURE" \
    STATE_FILE="$STATE_FILE" \
    "$ROOT/tanaken-alert" 2>&1)
failure_status=$?
set -e
if [ "$failure_status" -eq 0 ]; then
    printf '%s\n' "missing SMTP configuration test failed" >&2
    exit 1
fi
case "$failure_output" in
    *"SMTP_URL is required"*) ;;
    *)
        printf '%s\n' "$failure_output"
        printf '%s\n' "missing SMTP configuration test failed" >&2
        exit 1
        ;;
esac
after=$(cksum "$STATE_FILE")
test "$before" = "$after"

printf '%s\n' "All fixture and state tests passed."
