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

# MinGW / MSYS2 的 mktemp -d 默认用 Windows 风格 $TMPDIR（C:\Users\...），
# awk 把反斜杠当转义序列导致内嵌期望解析失败。强制用 POSIX /tmp 规避。
case $(uname -s) in
    MINGW*|MSYS*)
        TMPDIR=/tmp
        export TMPDIR
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

# 两个普通根：第二个用来覆盖 TAP_MODULE_PATH 的多目录支持。
# 第三个是别名条目（`name=path`）：`aliasmod` 前缀映射到 fixtures/aliasroot。
MODULE_PATH="$TEST_ROOT/fixtures:$TEST_ROOT/fixtures-extra:aliasmod=$TEST_ROOT/fixtures/aliasroot"
STD_PATH="$PROJECT_ROOT/std"
EXE_SUFFIX=
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*) EXE_SUFFIX=.exe ;;
esac

# 用例可以用 `.skip-on` 声明自己不适用的平台（见 run_case）。
# 粗粒度就够了：MSVC / MinGW / Cygwin 在「哪些第三方库装得上」这件事上是同一回事，
# 所以三者统一成 windows，用例不用为每个工具链各写一行。
case $(uname -s) in
    Darwin) TEST_PLATFORM=darwin ;;
    Linux) TEST_PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*) TEST_PLATFORM=windows ;;
    *) TEST_PLATFORM=unknown ;;
esac

total=0
passed=0
failed=0
skipped=0
planned_total=0
completed_tests=0

render_progress() {
    [ "$planned_total" -gt 0 ] || return

    progress_percent=$((completed_tests * 100 / planned_total))
    progress_width=24
    progress_filled=$((progress_percent * progress_width / 100))
    progress_bar=
    progress_index=0
    while [ "$progress_index" -lt "$progress_filled" ]; do
        progress_bar="${progress_bar}#"
        progress_index=$((progress_index + 1))
    done
    while [ "$progress_index" -lt "$progress_width" ]; do
        progress_bar="${progress_bar}."
        progress_index=$((progress_index + 1))
    done

    printf '%s\n' '------------------------------------'
    worker_index=1
    while [ "$worker_index" -le "$TEST_JOBS" ]; do
        worker_file="$TEMP_ROOT/worker.$worker_index"
        if [ -f "$worker_file" ]; then
            worker_case=
            IFS= read -r worker_case < "$worker_file" || true
            printf 'worker %d running: %s\n' "$worker_index" "$worker_case"
        else
            printf 'worker %d idle\n' "$worker_index"
        fi
        worker_index=$((worker_index + 1))
    done
    printf 'Testing [%s] %d/%d (%d%%)\n' \
        "$progress_bar" "$completed_tests" "$planned_total" "$progress_percent"
}

mark_failure() {
    case_failed=1
    printf '  %s\n' "$1" >&2
}

read_text_file() {
    sed -e 's/\r$//' -e 's/[[:blank:]]*$//' "$1"
}

# 从源码里的 /** -- test ... -- */ 块中抽出各类期望，写进 $2 加后缀的文件里。
# 只有出现过的段落才会留下 .found 标记，没出现的段落由调用方回落到同名外部文件。
extract_expectations() {
    extract_source_file=$1
    embedded_base=$2

    awk \
        -v stdout_file="$embedded_base.stdout" \
        -v stderr_file="$embedded_base.stderr" \
        -v exit_file="$embedded_base.exit" \
        -v args_file="$embedded_base.args" \
        -v flags_file="$embedded_base.flags" \
        -v skip_file="$embedded_base.skip" \
        -v stdout_found_file="$embedded_base.stdout.found" \
        -v stderr_found_file="$embedded_base.stderr.found" \
        -v exit_found_file="$embedded_base.exit.found" \
        -v args_found_file="$embedded_base.args.found" \
        -v flags_found_file="$embedded_base.flags.found" \
        -v skip_found_file="$embedded_base.skip.found" '
        BEGIN {
            output["stdout"] = stdout_file
            output["stderr"] = stderr_file
            output["exit"] = exit_file
            output["args"] = args_file
            output["flags"] = flags_file
            output["skip"] = skip_file
            found_output["stdout"] = stdout_found_file
            found_output["stderr"] = stderr_found_file
            found_output["exit"] = exit_found_file
            found_output["args"] = args_found_file
            found_output["flags"] = flags_found_file
            found_output["skip"] = skip_found_file
        }
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
        in_test && $0 ~ /^- \.(stdout|stderr|exit|args|flags|skip-on)$/ {
            current = substr($0, 3)
            current = substr(current, 2)
            if (current == "skip-on") current = "skip"
            found[current] = 1
            next
        }
        in_test && current != "" {
            values[current, ++count[current]] = $0
        }
        END {
            for (section in output) {
                if (!found[section]) continue
                print "1" > found_output[section]
                while (count[section] > 0 &&
                       values[section, count[section]] == "") {
                    count[section]--
                }
                if (count[section] == 0) {
                    print "" > output[section]
                } else {
                    for (line_number = 1; line_number <= count[section]; line_number++) {
                        print values[section, line_number] > output[section]
                    }
                }
                close(found_output[section])
                close(output[section])
            }
        }
    ' "$extract_source_file"
}

load_expectation() {
    section=$1
    source_file=$2
    output_file=$3
    embedded_output_file=$4
    embedded_found_file=$5

    if [ -f "$embedded_found_file" ]; then
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
    embedded_base="$TEMP_ROOT/$case_id.embedded"
    embedded_stdout_file="$embedded_base.stdout"
    embedded_stderr_file="$embedded_base.stderr"
    embedded_exit_file="$embedded_base.exit"
    embedded_args_file="$embedded_base.args"
    embedded_flags_file="$embedded_base.flags"
    embedded_skip_file="$embedded_base.skip"
    embedded_stdout_found_file="$embedded_base.stdout.found"
    embedded_stderr_found_file="$embedded_base.stderr.found"
    embedded_exit_found_file="$embedded_base.exit.found"
    embedded_args_found_file="$embedded_base.args.found"
    embedded_flags_found_file="$embedded_base.flags.found"
    embedded_skip_found_file="$embedded_base.skip.found"
    expected_args_file="$embedded_args_file"
    case_failed=0

    extract_expectations "$case_file" "$embedded_base"

    if load_expectation stdout "$case_file" "$expected_stdout_file" \
       "$embedded_stdout_file" "$embedded_stdout_found_file"; then
        has_expected_stdout=1
    else
        has_expected_stdout=0
    fi
    if load_expectation stderr "$case_file" "$expected_stderr_file" \
       "$embedded_stderr_file" "$embedded_stderr_found_file"; then
        has_expected_stderr=1
    else
        has_expected_stderr=0
    fi
    if [ -f "$embedded_exit_found_file" ]; then
        expected_status=$(tr -d '[:space:]' < "$embedded_exit_file")
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
    if [ -f "$embedded_args_found_file" ]; then
        has_program_args=1
    else
        has_program_args=0
    fi
    if [ -f "$embedded_flags_found_file" ]; then
        has_flags=1
    else
        has_flags=0
    fi

    # `.skip-on` 列出本用例不适用的平台。命中就整条跳过 —— 这些用例依赖的
    # 第三方库在某些平台上本来就不存在，报 FAIL 只会制造噪音。
    if [ -f "$embedded_skip_found_file" ]; then
        while IFS= read -r skip_platform || [ -n "$skip_platform" ]; do
            skip_platform=$(printf '%s' "$skip_platform" | tr -d '[:space:]')
            [ -z "$skip_platform" ] && continue
            if [ "$skip_platform" = "$TEST_PLATFORM" ]; then
                : > "$TEMP_ROOT/$case_id.skipped"
                printf 'SKIP %s (platform: %s)\n' "$relative_file" "$TEST_PLATFORM"
                return 0
            fi
        done < "$embedded_skip_file"
    fi

    if [ "$case_mode" = "compile-fail" ]; then
        env "TAP_MODULE_PATH=$MODULE_PATH" "TAP_STD_PATH=$STD_PATH" \
            "$COMPILER" -ir -o "$TEMP_ROOT/$case_id.ll" "$relative_file" \
            >"$stdout_file" 2>"$stderr_file"
        case_status=$?
    else
        use_native=0
        # 带 `.flags` 的用例要真的链接（-l / -L / -Wl,），lli 走不了这条路。
        if [ "$TEST_RUN_MODE" = "native" ] || [ "$has_program_args" -eq 1 ] ||
           [ "$has_flags" -eq 1 ] ||
           [ "$relative_file" = "tests/run-pass/basics/hello.tp" ]; then
            use_native=1
        fi

        if [ "$use_native" -eq 1 ]; then
            set -- "$COMPILER" run -o "$TEMP_ROOT/$case_id-program$EXE_SUFFIX"
            if [ "$has_flags" -eq 1 ]; then
                while IFS= read -r compiler_flag || [ -n "$compiler_flag" ]; do
                    [ -z "$compiler_flag" ] && continue
                    set -- "$@" "$compiler_flag"
                done < "$embedded_flags_file"
            fi
            set -- "$@" "$relative_file"
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

    # 每批只显示一次当前 worker，避免频繁刷新终端拖慢 Cygwin。
    render_progress
    batch_target=$((completed_tests + active_workers))
    while IFS= read -r worker_pid; do
        wait "$worker_pid" || true
    done < "$BATCH_FILE"

    completed_tests=$batch_target
    render_progress
    : > "$BATCH_FILE"
    active_workers=0
}

launch_case() {
    launch_mode=$1
    launch_file=$2
    total=$((total + 1))
    launch_id=$total
    launch_relative_file=${launch_file#"$PROJECT_ROOT/"}
    worker_slot=$((active_workers + 1))
    printf '%s\n' "$launch_relative_file" > "$TEMP_ROOT/worker.$worker_slot"

    (
        trap - EXIT HUP INT TERM
        if run_case "$launch_mode" "$launch_file" "$launch_id"; then
            worker_status=0
        else
            worker_status=1
        fi
        rm -f "$TEMP_ROOT/worker.$worker_slot"
        printf '%s\n' "$worker_status" > "$TEMP_ROOT/$launch_id.status"
    ) > "$TEMP_ROOT/$launch_id.result" 2>&1 &
    printf '%s\n' "$!" >> "$BATCH_FILE"
    active_workers=$((active_workers + 1))
    if [ "$active_workers" -ge "$TEST_JOBS" ]; then
        wait_for_batch
    fi
}

prepare_group() {
    group_mode=$1
    group_directory=$2
    list_file="$TEMP_ROOT/$group_mode.list"
    raw_list_file="$TEMP_ROOT/$group_mode.raw.list"

    # 源文件后缀 .tp 和 .tap 都支持，用例文件两种都能写。
    find "$TEST_ROOT/$group_directory" -type f \( -name '*.tp' -o -name '*.tap' \) \
        | LC_ALL=C sort > "$raw_list_file"
    : > "$list_file"
    while IFS= read -r test_file; do
        relative_file=${test_file#"$PROJECT_ROOT/"}
        if [ -n "$FILTER" ]; then
            case "$relative_file" in
                *"$FILTER"*) ;;
                *) continue ;;
            esac
        fi
        printf '%s\n' "$test_file" >> "$list_file"
        planned_total=$((planned_total + 1))
    done < "$raw_list_file"
}

run_group() {
    group_mode=$1
    list_file="$TEMP_ROOT/$group_mode.list"

    while IFS= read -r test_file; do
        launch_case "$group_mode" "$test_file"
    done < "$list_file"
}

cd "$PROJECT_ROOT" || exit 2
prepare_group run-pass run-pass
prepare_group run-fail run-fail
prepare_group compile-fail compile-fail

if [ "$planned_total" -gt 0 ]; then
    printf 'Running %d tests with %d workers\n' "$planned_total" "$TEST_JOBS"
fi
run_group run-pass run-pass
run_group run-fail run-fail
run_group compile-fail compile-fail
wait_for_batch

if [ "$total" -eq 0 ]; then
    printf 'no tests matched filter: %s\n' "$FILTER" >&2
    exit 2
fi

printf '\n'
case_id=1
while [ "$case_id" -le "$total" ]; do
    if [ -f "$TEMP_ROOT/$case_id.result" ]; then
        cat "$TEMP_ROOT/$case_id.result"
    fi
    if [ -f "$TEMP_ROOT/$case_id.skipped" ]; then
        skipped=$((skipped + 1))
    elif [ -f "$TEMP_ROOT/$case_id.status" ] &&
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

printf '\n%d tests: %d passed, %d skipped, %d failed\n' \
    "$total" "$passed" "$skipped" "$failed"
[ "$failed" -eq 0 ]
