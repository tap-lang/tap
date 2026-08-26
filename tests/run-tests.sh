#!/bin/sh

set -u

TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$TEST_ROOT/.." && pwd)
COMPILER=${1:-"$PROJECT_ROOT/build/4yue"}
FILTER=${TEST_FILTER:-${2:-}}

case "$COMPILER" in
    /*|[A-Za-z]:/*) ;;
    *) COMPILER="$PROJECT_ROOT/${COMPILER#./}" ;;
esac

if [ ! -x "$COMPILER" ]; then
    printf 'test compiler is not executable: %s\n' "$COMPILER" >&2
    exit 2
fi

TEMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/4yue-tests.XXXXXX") || exit 2
trap 'rm -rf "$TEMP_ROOT"' EXIT HUP INT TERM

MODULE_PATH="$TEST_ROOT/fixtures"
STD_PATH="$PROJECT_ROOT/std"
EXE_SUFFIX=
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*) EXE_SUFFIX=.exe ;;
esac

total=0
passed=0
failed=0

mark_failure() {
    case_failed=1
    printf '  %s\n' "$1" >&2
}

read_text_file() {
    tr -d '\r' < "$1"
}

check_stdout() {
    expected_file=${case_file%.tp}.stdout
    if [ -f "$expected_file" ]; then
        expected_output=$(read_text_file "$expected_file")
        actual_output=$(read_text_file "$stdout_file")
        if [ "$actual_output" != "$expected_output" ]; then
            mark_failure "stdout does not match $expected_file"
            diff -u --strip-trailing-cr "$expected_file" "$stdout_file" >&2 || true
        fi
    elif [ -s "$stdout_file" ]; then
        mark_failure "unexpected stdout"
        sed 's/^/    /' "$stdout_file" >&2
    fi
}

check_stderr() {
    expected_file=${case_file%.tp}.stderr
    if [ "$case_mode" = "compile-fail" ]; then
        if [ ! -f "$expected_file" ]; then
            mark_failure "missing expected diagnostic file $expected_file"
            return
        fi

        while IFS= read -r expected_line || [ -n "$expected_line" ]; do
            expected_line=$(printf '%s' "$expected_line" | tr -d '\r')
            [ -z "$expected_line" ] && continue
            if ! read_text_file "$stderr_file" | grep -Fq -- "$expected_line"; then
                mark_failure "missing stderr text: $expected_line"
            fi
        done < "$expected_file"
    elif [ -f "$expected_file" ]; then
        expected_output=$(read_text_file "$expected_file")
        actual_output=$(read_text_file "$stderr_file")
        if [ "$actual_output" != "$expected_output" ]; then
            mark_failure "stderr does not match $expected_file"
            diff -u --strip-trailing-cr "$expected_file" "$stderr_file" >&2 || true
        fi
    elif [ -s "$stderr_file" ]; then
        mark_failure "unexpected stderr"
        sed 's/^/    /' "$stderr_file" >&2
    fi
}

run_case() {
    case_mode=$1
    case_file=$2
    relative_file=${case_file#"$PROJECT_ROOT/"}

    if [ -n "$FILTER" ]; then
        case "$relative_file" in
            *"$FILTER"*) ;;
            *) return ;;
        esac
    fi

    total=$((total + 1))
    stdout_file="$TEMP_ROOT/$total.stdout"
    stderr_file="$TEMP_ROOT/$total.stderr"
    case_failed=0

    if [ "$case_mode" = "compile-fail" ]; then
        env "4YUE_MODULE_PATH=$MODULE_PATH" "4YUE_STD_PATH=$STD_PATH" \
            "$COMPILER" -ir -o "$TEMP_ROOT/$total.ll" "$relative_file" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?
        if [ "$case_status" -eq 0 ]; then
            mark_failure "expected compilation to fail"
        fi
    else
        env "4YUE_MODULE_PATH=$MODULE_PATH" "4YUE_STD_PATH=$STD_PATH" \
            "$COMPILER" run -o "$TEMP_ROOT/$total-program$EXE_SUFFIX" "$relative_file" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?

        if [ "$case_mode" = "run-pass" ]; then
            expected_status=0
        else
            exit_file=${case_file%.tp}.exit
            if [ ! -f "$exit_file" ]; then
                mark_failure "missing expected exit file $exit_file"
                expected_status=
            else
                expected_status=$(tr -d '[:space:]' < "$exit_file")
                case "$expected_status" in
                    ''|*[!0-9]*)
                        mark_failure "invalid exit status in $exit_file"
                        expected_status=
                        ;;
                esac
            fi
        fi

        if [ -n "$expected_status" ] && [ "$case_status" -ne "$expected_status" ]; then
            mark_failure "exit status $case_status, expected $expected_status"
        fi
    fi

    check_stdout
    check_stderr

    if [ "$case_failed" -eq 0 ]; then
        passed=$((passed + 1))
        printf 'PASS %s\n' "$relative_file"
    else
        failed=$((failed + 1))
        printf 'FAIL %s\n' "$relative_file" >&2
    fi
}

run_group() {
    group_mode=$1
    group_directory=$2
    list_file="$TEMP_ROOT/$group_mode.list"

    find "$TEST_ROOT/$group_directory" -type f -name '*.tp' | LC_ALL=C sort > "$list_file"
    while IFS= read -r test_file; do
        run_case "$group_mode" "$test_file"
    done < "$list_file"
}

cd "$PROJECT_ROOT" || exit 2
run_group run-pass run-pass
run_group run-fail run-fail
run_group compile-fail compile-fail

if [ "$total" -eq 0 ]; then
    printf 'no tests matched filter: %s\n' "$FILTER" >&2
    exit 2
fi

printf '\n%d tests: %d passed, %d failed\n' "$total" "$passed" "$failed"
[ "$failed" -eq 0 ]
