#!/bin/sh
# Owner-staged wake model selection in init/libreecho-waked.init: a valid
# selection runs the stock detector against it, and every kind of bad
# selection, including one that fails to load, falls back to the stock model.
set -eu

T=$(mktemp -d /tmp/libreecho-waked-init-XXXXXX)
INIT=$(pwd)/init/libreecho-waked.init
cleanup() {
    if [ -s "$T/waked.pid" ]; then kill "$(cat "$T/waked.pid")" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/bin" "$T/root/usr/local/sbin" "$T/models"
STOCK=$T/root/usr/local/share/libreecho/wakeword
STOCK_WORD=$STOCK/alexa
# word <dir> <name>: the microWakeWord pair the contract expects.
word() {
    mkdir -p "$1"
    echo tflite >"$1/$2.tflite"
    echo json >"$1/$2.json"
}
word "$STOCK_WORD" alexa
word "$STOCK/vad" vad
: >"$T/payload"
printf '#!/bin/sh\nexit 0\n' >"$T/bin/mount"
cp "$T/bin/mount" "$T/bin/umount"
cat >"$T/bin/start-stop-daemon" <<'EOF'
#!/bin/sh
mode=; pidfile=; daemon=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -S) mode=start ;; -K) mode=stop ;;
        -p) pidfile=$2; shift ;; -x) daemon=$2; shift ;;
        -R) shift ;; --) shift; break ;;
    esac
    shift
done
if [ "$mode" = stop ]; then
    kill "$(cat "$pidfile")" 2>/dev/null || true
    exit 0
fi
"$daemon" "$@" </dev/null >/dev/null 2>&1 &
echo "$!" >"$pidfile"
EOF
# Records its arguments; a model directory named bad-load fails like an
# unloadable model does, by exiting at startup.
cat >"$T/root/usr/local/sbin/libreecho-waked" <<EOF
#!/bin/sh
echo "\$*" >>"$T/args"
case "\$*" in *"/bad-load "*) exit 1 ;; esac
exec sleep 20
EOF
chmod +x "$T/bin/"* "$T/root/usr/local/sbin/libreecho-waked"
python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$T/mic.sock"

run() {
    PATH="$T/bin:$PATH" PAYLOAD="$T/payload" RUNTIME_ROOT="$T/root" \
    PIDFILE="$T/waked.pid" LOGFILE="$T/waked.log" SOCKET="$T/wake.sock" \
    MIC_SOCKET="$T/mic.sock" REFERENCE_SOCKET="$T/ref.sock" \
    VAD_FLOOR_FILE="$T/none" AEC_FILE="$T/none" DUMP_FILE="$T/none" \
    WAKE_MODEL_FILE="$T/wake-model" WAKE_MODEL_BASE="$T/models" \
    WAKE_SECOND_FILE="$T/wake-second" \
    WAKE_MODEL_SETTLE_SECONDS=1 timeout 20 sh "$INIT" "$1" 2>>"$T/stderr"
}

# The fake start-stop-daemon backgrounds the daemon, which writes its argv a
# moment later; wait for that many records instead of racing it. A start that
# falls back on the stock model adds a second record after its settle delay.
wait_args() {
    need=${1:-1}
    count=0
    while [ "$(wc -l <"$T/args")" -lt "$need" ] && [ "$count" -lt 100 ]; do
        sleep 0.1
        count=$((count + 1))
    done
}

# expect <case> <model-dir> [<model-dir> ...]: each start's --model-dir, in order.
expect() {
    name=$1; shift
    : >"$T/args"
    run start
    wait_args "$#"
    count=0
    for dir in "$@"; do
        count=$((count + 1))
        line=$(sed -n "${count}p" "$T/args")
        case "$line" in
            *"--model-dir $dir "*) ;;
            *) echo "FAIL $name: start $count used: $line" >&2; cat "$T/stderr" >&2; exit 1 ;;
        esac
    done
    [ "$(wc -l <"$T/args")" -eq "$count" ] || {
        echo "FAIL $name: expected $count start(s)" >&2; cat "$T/args" >&2; exit 1; }
    run status >/dev/null || { echo "FAIL $name: waked not running" >&2; exit 1; }
    run stop
    echo "waked init: $name"
}

expect "no selection runs the stock model" "$STOCK_WORD"
[ ! -e "$T/models/vad" ] || { echo "FAIL: VAD seeded without an owner model" >&2; exit 1; }

word "$T/models/mine" mine
echo mine >"$T/wake-model"
expect "a staged model is selected" "$T/models/mine"
# The engine takes the shared VAD model from the selected word's parent.
[ -f "$T/models/vad/vad.tflite" ] && [ -f "$T/models/vad/vad.json" ] || {
    echo "FAIL: the owner VAD model was not seeded" >&2; exit 1; }
rm -rf "$T/models/vad"

echo "../mine" >"$T/wake-model"
expect "a path-like model name falls back to stock" "$STOCK_WORD"

word "$T/models/partial" partial
rm "$T/models/partial/partial.tflite"
echo partial >"$T/wake-model"
expect "an incomplete model falls back to stock" "$STOCK_WORD"

word "$T/models/linked" linked
rm "$T/models/linked/linked.tflite"
ln -s "$STOCK_WORD/alexa.tflite" "$T/models/linked/linked.tflite"
echo linked >"$T/wake-model"
expect "a symlinked model file falls back to stock" "$STOCK_WORD"

# A half-seeded owner VAD copy is completed from the payload, so an
# interrupted first run does not strand the owner model.
mkdir -p "$T/models/vad"
echo stale >"$T/models/vad/vad.tflite"
echo mine >"$T/wake-model"
expect "a partial owner VAD copy is refreshed" "$T/models/mine"
[ -f "$T/models/vad/vad.json" ] || { echo "FAIL: the VAD manifest was not seeded" >&2; exit 1; }
rm -rf "$T/models/vad"

word "$T/models/bad-load" bad-load
echo bad-load >"$T/wake-model"
expect "a model that fails to load restarts on stock" "$T/models/bad-load" "$STOCK_WORD"
grep -q "owner-staged model failed to start; using stock model" "$T/stderr"

# expect_second <case> <folder-or-empty>: the last start's --second-model-dir,
# or none when empty. The second word never changes the primary model.
expect_second() {
    name=$1; folder=$2
    : >"$T/args"
    run start
    wait_args 1
    line=$(tail -n 1 "$T/args")
    if [ -n "$folder" ]; then
        case "$line" in
            *"--second-model-dir $folder"*) ;;
            *) echo "FAIL $name: $line" >&2; cat "$T/stderr" >&2; exit 1 ;;
        esac
    else
        case "$line" in
            *--second-model-dir*) echo "FAIL $name: $line" >&2; exit 1 ;;
        esac
    fi
    run stop
    echo "waked init: $name"
}

rm -f "$T/wake-model"
rm -f "$T/wake-second"
expect_second "no second word file runs one word" ""
echo mine >"$T/wake-second"
expect_second "a staged folder is the second word" "$T/models/mine"
echo "../mine" >"$T/wake-second"
expect_second "a path-like second word is ignored" ""
word "$T/models/linked" linked
rm "$T/models/linked/linked.tflite"
ln -s "$STOCK_WORD/alexa.tflite" "$T/models/linked/linked.tflite"
echo linked >"$T/wake-second"
expect_second "a second word with a symlinked model is ignored" ""
echo missing >"$T/wake-second"
expect_second "a second word with no folder is ignored" ""
