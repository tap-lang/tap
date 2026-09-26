#!/bin/sh

set -u

# PowerShell can launch Cygwin sh with System32 ahead of /usr/bin. Without
# normalizing PATH, commands such as find and sort resolve to Windows tools.
case $(uname -s) in
    CYGWIN*)
        PATH=/usr/bin:$PATH
        export PATH
        ;;
esac

TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$TEST_ROOT/.." && pwd)
COMPILER=${1:-"$PROJECT_ROOT/build/tap"}
FILTER=${TEST_FILTER:-${2:-}}
TEST_JOBS=${TEST_JOBS:-4}
TEST_RUN_MODE=${TEST_RUN_MODE:-fast}

case "$TEST_JOBS" in
    ''|*[!0-9]*|0)
        printf 'TEST_JOBS must be a positive integer: %s\n' "$TEST_JOBS" >&2
        exit 2
        ;;
esac
case "$TEST_RUN_MODE" in
    fast|native) ;;
    *)
        printf 'TEST_RUN_MODE must be fast or native: %s\n' "$TEST_RUN_MODE" >&2
        exit 2
        ;;
esac

case "$COMPILER" in
    /*|[A-Za-z]:/*) ;;
    *) COMPILER="$PROJECT_ROOT/${COMPILER#./}" ;;
esac

if [ ! -x "$COMPILER" ]; then
    printf 'test compiler is not executable: %s\n' "$COMPILER" >&2
    exit 2
fi

TEMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tap-tests.XXXXXX") || exit 2
BATCH_FILE="$TEMP_ROOT/workers"

cleanup() {
    if [ -f "$BATCH_FILE" ]; then
        while IFS= read -r worker_pid; do
            [ -n "$worker_pid" ] && kill "$worker_pid" 2>/dev/null || true
        done < "$BATCH_FILE"
    fi
    rm -rf "$TEMP_ROOT"
}
trap cleanup EXIT HUP INT TERM

# Native test workers reuse one SDK lookup instead of running xcrun for every link.
if [ "$(uname -s)" = "Darwin" ]; then
    if [ -z "${TAP_MACOS_SDK_PATH:-}" ]; then
        TAP_MACOS_SDK_PATH=$(xcrun --sdk macosx --show-sdk-path) || exit 2
        export TAP_MACOS_SDK_PATH
    fi
    if [ -z "${TAP_MACOS_SDK_VERSION:-}" ]; then
        TAP_MACOS_SDK_VERSION=$(xcrun --sdk macosx --show-sdk-version) || exit 2
        export TAP_MACOS_SDK_VERSION
    fi
fi

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
    sed -e 's/\r$//' -e 's/[[:blank:]]*$//' "$1"
}

extract_expectation() {
    section=$1
    source_file=$2
    awk -v target=".$section" '
        { sub(/\r$/, "") }
        $0 == "/** -- test" {
            in_test = 1
            next
        }
        in_test && $0 == "-- */" {
            in_test = 0
            current = ""
            next
        }
        in_test && $0 ~ /^- \.(stdout|stderr|exit|args)$/ {
            current = substr($0, 3)
            if (current == target) found = 1
            next
        }
        in_test && current == target {
            values[++count] = $0
        }
        END {
            while (count > 0 && values[count] == "") count--
            for (line_number = 1; line_number <= count; line_number++) {
                print values[line_number]
            }
            if (!found) exit 1
        }
    ' "$source_file"
}

load_expectation() {
    section=$1
    source_file=$2
    output_file=$3

    embedded_output_file="$output_file.embedded"
    if extract_expectation "$section" "$source_file" > "$embedded_output_file"; then
        read_text_file "$embedded_output_file" > "$output_file"
        return 0
    fi

    external_file="${source_file%.*}.$section"
    if [ -f "$external_file" ]; then
        read_text_file "$external_file" > "$output_file"
        return 0
    fi

    return 1
}

check_stdout() {
    if [ "$has_expected_stdout" -eq 1 ]; then
        expected_output=$(read_text_file "$expected_stdout_file")
        actual_output=$(read_text_file "$stdout_file")
        if [ "$actual_output" != "$expected_output" ]; then
            mark_failure "stdout does not match .stdout expectation"
            diff -u --strip-trailing-cr "$expected_stdout_file" "$stdout_file" >&2 || true
        fi
    elif [ -s "$stdout_file" ]; then
        mark_failure "unexpected stdout"
        sed 's/^/    /' "$stdout_file" >&2
    fi
}

check_stderr() {
    if [ "$case_mode" = "compile-fail" ]; then
        if [ "$has_expected_stderr" -ne 1 ]; then
            mark_failure "missing .stderr expectation"
            return
        fi

        while IFS= read -r expected_line || [ -n "$expected_line" ]; do
            expected_line=$(printf '%s' "$expected_line" | tr -d '\r')
            [ -z "$expected_line" ] && continue
            if ! read_text_file "$stderr_file" | grep -Fq -- "$expected_line"; then
                mark_failure "missing stderr text: $expected_line"
            fi
        done < "$expected_stderr_file"
    elif [ "$has_expected_stderr" -eq 1 ]; then
        expected_output=$(read_text_file "$expected_stderr_file")
        actual_output=$(read_text_file "$stderr_file")
        if [ "$actual_output" != "$expected_output" ]; then
            mark_failure "stderr does not match .stderr expectation"
            diff -u --strip-trailing-cr "$expected_stderr_file" "$stderr_file" >&2 || true
        fi
    elif [ -s "$stderr_file" ]; then
        mark_failure "unexpected stderr"
        sed 's/^/    /' "$stderr_file" >&2
    fi
}

run_case() {
    case_mode=$1
    case_file=$2
    case_id=$3
    relative_file=${case_file#"$PROJECT_ROOT/"}

    stdout_file="$TEMP_ROOT/$case_id.stdout"
    stderr_file="$TEMP_ROOT/$case_id.stderr"
    expected_stdout_file="$TEMP_ROOT/$case_id.expected.stdout"
    expected_stderr_file="$TEMP_ROOT/$case_id.expected.stderr"
    expected_exit_file="$TEMP_ROOT/$case_id.expected.exit"
    expected_args_file="$TEMP_ROOT/$case_id.expected.args"
    case_failed=0

    if load_expectation stdout "$case_file" "$expected_stdout_file"; then
        has_expected_stdout=1
    else
        has_expected_stdout=0
    fi
    if load_expectation stderr "$case_file" "$expected_stderr_file"; then
        has_expected_stderr=1
    else
        has_expected_stderr=0
    fi
    if extract_expectation exit "$case_file" > "$expected_exit_file"; then
        expected_status=$(tr -d '[:space:]' < "$expected_exit_file")
        case "$expected_status" in
            ''|*[!0-9]*)
                mark_failure "invalid embedded .exit expectation"
                expected_status=
                ;;
        esac
    else
        mark_failure "missing embedded .exit expectation"
        expected_status=
    fi
    if extract_expectation args "$case_file" > "$expected_args_file"; then
        has_program_args=1
    else
        has_program_args=0
    fi

    if [ "$case_mode" = "compile-fail" ]; then
        env "TAP_MODULE_PATH=$MODULE_PATH" "TAP_STD_PATH=$STD_PATH" \
            "$COMPILER" -ir -o "$TEMP_ROOT/$case_id.ll" "$relative_file" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?
    else
        use_native=0
        if [ "$TEST_RUN_MODE" = "native" ] || [ "$has_program_args" -eq 1 ] ||
           [ "$relative_file" = "tests/run-pass/basics/hello.tp" ]; then
            use_native=1
        fi

        if [ "$use_native" -eq 1 ]; then
            set -- "$COMPILER" run -o "$TEMP_ROOT/$case_id-program$EXE_SUFFIX" "$relative_file"
            if [ "$has_program_args" -eq 1 ]; then
                set -- "$@" --
                while IFS= read -r program_arg || [ -n "$program_arg" ]; do
                    set -- "$@" "$program_arg"
                done < "$expected_args_file"
            fi
        else
            set -- "$COMPILER" -run-lli "$relative_file"
        fi
        env "TAP_MODULE_PATH=$MODULE_PATH" "TAP_STD_PATH=$STD_PATH" \
            "$@" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?
    fi

    if [ -n "$expected_status" ] && [ "$case_status" -ne "$expected_status" ]; then
        mark_failure "exit status $case_status, expected $expected_status"
    fi

    check_stdout
    check_stderr

    if [ "$case_failed" -eq 0 ]; then
        printf 'PASS %s\n' "$relative_file"
        return 0
    else
        printf 'FAIL %s\n' "$relative_file" >&2
        return 1
    fi
}

active_workers=0

wait_for_batch() {
    [ -s "$BATCH_FILE" ] || return
    while IFS= read -r worker_pid; do
        wait "$worker_pid" || true
    done < "$BATCH_FILE"
    : > "$BATCH_FILE"
    active_workers=0
}

launch_case() {
    launch_mode=$1
    launch_file=$2
    total=$((total + 1))
    launch_id=$total

    (
        trap - EXIT HUP INT TERM
        if run_case "$launch_mode" "$launch_file" "$launch_id"; then
            worker_status=0
        else
            worker_status=1
        fi
        printf '%s\n' "$worker_status" > "$TEMP_ROOT/$launch_id.status"
    ) > "$TEMP_ROOT/$launch_id.result" 2>&1 &
    printf '%s\n' "$!" >> "$BATCH_FILE"
    active_workers=$((active_workers + 1))
    if [ "$active_workers" -ge "$TEST_JOBS" ]; then
        wait_for_batch
    fi
}

run_group() {
    group_mode=$1
    group_directory=$2
    list_file="$TEMP_ROOT/$group_mode.list"

    # 源文件后缀 .tp 和 .tap 都支持，用例文件两种都能写。
    find "$TEST_ROOT/$group_directory" -type f \( -name '*.tp' -o -name '*.tap' \) \
        | LC_ALL=C sort > "$list_file"
    while IFS= read -r test_file; do
        relative_file=${test_file#"$PROJECT_ROOT/"}
        if [ -n "$FILTER" ]; then
            case "$relative_file" in
                *"$FILTER"*) ;;
                *) continue ;;
            esac
        fi
        launch_case "$group_mode" "$test_file"
    done < "$list_file"
}

cd "$PROJECT_ROOT" || exit 2
run_group run-pass run-pass
run_group run-fail run-fail
run_group compile-fail compile-fail
wait_for_batch

if [ "$total" -eq 0 ]; then
    printf 'no tests matched filter: %s\n' "$FILTER" >&2
    exit 2
fi

case_id=1
while [ "$case_id" -le "$total" ]; do
    if [ -f "$TEMP_ROOT/$case_id.result" ]; then
        cat "$TEMP_ROOT/$case_id.result"
    fi
    if [ -f "$TEMP_ROOT/$case_id.status" ] &&
       [ "$(sed -n '1p' "$TEMP_ROOT/$case_id.status")" = "0" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        if [ ! -f "$TEMP_ROOT/$case_id.result" ]; then
            printf 'FAIL test worker %d did not produce a result\n' "$case_id" >&2
        fi
    fi
    case_id=$((case_id + 1))
done

printf '\n%d tests: %d passed, %d failed\n' "$total" "$passed" "$failed"
[ "$failed" -eq 0 ]
