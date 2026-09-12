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
    expected_stdout_file="$TEMP_ROOT/$total.expected.stdout"
    expected_stderr_file="$TEMP_ROOT/$total.expected.stderr"
    expected_exit_file="$TEMP_ROOT/$total.expected.exit"
    expected_args_file="$TEMP_ROOT/$total.expected.args"
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
        env "4YUE_MODULE_PATH=$MODULE_PATH" "4YUE_STD_PATH=$STD_PATH" \
            "$COMPILER" -ir -o "$TEMP_ROOT/$total.ll" "$relative_file" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?
    else
        set -- "$COMPILER" run -o "$TEMP_ROOT/$total-program$EXE_SUFFIX" "$relative_file"
        if [ "$has_program_args" -eq 1 ]; then
            set -- "$@" --
            while IFS= read -r program_arg || [ -n "$program_arg" ]; do
                set -- "$@" "$program_arg"
            done < "$expected_args_file"
        fi
        env "4YUE_MODULE_PATH=$MODULE_PATH" "4YUE_STD_PATH=$STD_PATH" \
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
