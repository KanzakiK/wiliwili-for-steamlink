#!/usr/bin/env bash
#
# D26/D27：只消费 debug.sh 归档，不连接设备、不启动应用。
#
# 用法: bash tools/steamlink-app/check-debug-run.sh debug-runs/<run-id>
#
# 统一日志行格式：
#   [HH:MM:SS.mmm][SOURCE][run=...][session=...][gen=...][req=...] message
#
# 检查：manifest / prestart / 统一日志 / dmesg 附件齐全；CMD、EVT、内容身份、
# session 透传、敏感信息脱敏和 D22 健康提示满足 08 文档契约。
set -u

RUN_DIR=${1:-}
if [ -z "$RUN_DIR" ]; then
    echo "用法: $0 debug-runs/<run-id>" >&2
    exit 2
fi

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }
check_file() {
    if [ -s "$RUN_DIR/$1" ]; then pass "$1 存在且非空"; else fail "$1 缺失或为空"; fi
}
check_pattern() {
    if grep -F "$2" "$RUN_DIR/$1" >/dev/null 2>&1; then
        pass "$1 含 $3"
    else
        fail "$1 缺少 $3"
    fi
}
field_from() {
    printf '%s\n' "$1" | sed -n "s/.*[ =]$2=\\([^ ]*\\).*/\\1/p" | head -1
}
check_both() {
    if grep -F "$2" "$RUN_DIR/$1" 2>/dev/null | grep -F "$3" >/dev/null 2>&1; then
        pass "$1 含 $4"
    else
        fail "$1 缺少 $4"
    fi
}

check_file manifest.txt
check_file prestart.log
check_file wiliwili.log
check_file dmesg.before.txt
check_file dmesg.after.txt
check_file launch_cmdline.txt

RUN_ID=$(sed -n 's/^run_id=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
SCENE=$(sed -n 's/^scene=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
if [ -z "$RUN_ID" ]; then
    fail "manifest 缺少 run_id"
elif [ "$RUN_ID" != "$(basename "$RUN_DIR")" ]; then
    fail "manifest run_id=$RUN_ID 与目录名不一致"
else
    pass "run_id 与目录一致: $RUN_ID"
fi
if [ -z "$SCENE" ] || [ "$SCENE" = "none" ]; then
    fail "manifest 缺少有效 scene"
fi
for key in created_at collected_at archive_size_bytes; do
    if grep -q "^$key=" "$RUN_DIR/manifest.txt" 2>/dev/null; then
        pass "manifest 含 $key"
    else
        fail "manifest 缺少 $key"
    fi
done
check_file "dmesg.$SCENE.start.txt"
check_file "dmesg.$SCENE.ok.txt"

check_pattern wiliwili.log "[boot] run=$RUN_ID" "boot run id"
check_pattern wiliwili.log "][CMD][run=$RUN_ID][session=" "统一 CMD 行"
check_both wiliwili.log "scene=$SCENE" "status=start" "CMD status=start"
check_both wiliwili.log "scene=$SCENE" "status=ok" "CMD status=ok"

if grep -Eq '^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\]\[[A-Z]+\]\[run=' \
        "$RUN_DIR/wiliwili.log"; then
    pass "统一日志时间戳与 source/run 前缀"
else
    fail "统一日志缺少时间戳/source/run 前缀"
fi
if grep -Eq '\[mpv-compat\]|\[SlPlayer\]|\[SlAudio\]|\[daemon\]\[life\]' \
        "$RUN_DIR/wiliwili.log"; then
    fail "统一日志仍含旧 source 前缀"
else
    pass "统一日志无旧 source 前缀"
fi

SESSION=$(grep -F "scene=$SCENE" "$RUN_DIR/wiliwili.log" 2>/dev/null |
    grep -F "status=ok" |
    sed -n 's/.*\[session=\([^]]*\)\].*/\1/p' | grep -v '^-' | head -1)
if [ -z "$SESSION" ]; then
    fail "场景没有有效 session"
else
    pass "场景 session=$SESSION"
    PREFIX="[run=$RUN_ID][session=$SESSION]"

    case "$SCENE" in
        play|seek_forward|seek_backward|pause_resume|quality_change)
            check_pattern wiliwili.log "][UI]$PREFIX" "UI 统一行"
            check_pattern wiliwili.log "][UI]$PREFIX[gen=-][req=-] content start" "UI content start"
            check_pattern wiliwili.log "][COMPAT]$PREFIX" "compat 统一行"
            check_both wiliwili.log "$PREFIX" "meta ok" "daemon meta ok"
            check_both wiliwili.log "$PREFIX" "open enter" "daemon open"
            check_pattern wiliwili.log "][PLAYER]$PREFIX" "player 统一行"
            check_pattern wiliwili.log "][AUDIO]$PREFIX" "audio 统一行"

            START_LINE=$(grep -F "][UI]$PREFIX[gen=-][req=-] content start" "$RUN_DIR/wiliwili.log" 2>/dev/null | head -1)
            META_LINE=$(grep -F "$PREFIX" "$RUN_DIR/wiliwili.log" 2>/dev/null | grep -F "meta ok" | head -1)
            LOAD_LINE=$(grep -F "$PREFIX" "$RUN_DIR/wiliwili.log" 2>/dev/null | grep -F " loadfile url=" | head -1)

            CONTENT_TYPE=$(field_from "$START_LINE" content)
            UI_ID=$(field_from "$START_LINE" id)
            UI_BV=$(field_from "$START_LINE" bv)
            UI_CID=$(field_from "$START_LINE" cid)
            UI_QN=$(field_from "$START_LINE" qn)
            META_BV=$(field_from "$META_LINE" bv)
            META_CID=$(field_from "$META_LINE" cid)
            META_QN=$(field_from "$META_LINE" qn)
            LOAD_BV=$(field_from "$LOAD_LINE" bv)
            LOAD_CID=$(field_from "$LOAD_LINE" cid)
            LOAD_QN=$(field_from "$LOAD_LINE" qn)

            if [ -n "$LOAD_LINE" ]; then pass "loadfile 带权威内容字段"; else fail "loadfile 缺少权威内容字段"; fi
            case "$CONTENT_TYPE" in
                video|season)
                    for spec in "UI BV:$UI_BV" "meta BV:$META_BV" "loadfile BV:$LOAD_BV"; do
                        value=${spec#*: }
                        if [ -z "$value" ] || [ "$value" = "-" ]; then
                            fail "$spec 缺失"
                        else
                            pass "$spec"
                        fi
                    done
                    if [ "$UI_BV" != "$META_BV" ] || [ "$UI_BV" != "$LOAD_BV" ]; then
                        fail "video/season BV 不一致: ui=$UI_BV meta=$META_BV loadfile=$LOAD_BV"
                    else
                        pass "video/season BV 三处一致: $UI_BV"
                    fi
                    ;;
                *)
                    pass "content=$CONTENT_TYPE 不要求 BV"
                    ;;
            esac
            if [ -n "$UI_CID" ] && [ "$UI_CID" != "-" ] && [ "$UI_CID" != "0" ]; then
                pass "content start cid=$UI_CID"
            else
                fail "content start 缺少有效 cid"
            fi
            if [ -n "$UI_QN" ] && [ "$UI_QN" != "-" ]; then
                pass "content start qn=$UI_QN"
            else
                fail "content start 缺少 qn"
            fi
            if [ -n "$UI_ID" ] && [ "$UI_ID" != "-" ]; then
                pass "content start id=$UI_ID"
            else
                fail "content start 缺少 id"
            fi
            if [ "$META_CID" = "$UI_CID" ] && [ "$META_QN" = "$UI_QN" ] && \
               [ "$LOAD_CID" = "$UI_CID" ] && [ "$LOAD_QN" = "$UI_QN" ]; then
                pass "cid/qn 在 UI/meta/loadfile 一致"
            else
                fail "cid/qn 不一致: ui=$UI_CID/$UI_QN meta=$META_CID/$META_QN loadfile=$LOAD_CID/$LOAD_QN"
            fi

            case "$SCENE" in
                seek_forward|seek_backward)
                    check_both wiliwili.log "$PREFIX" "seek mode=" "CMD seek"
                    check_both wiliwili.log "$PREFIX" "seeked" "EVT seeked"
                    SCENE_ARGS=$(sed -n 's/^args=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
                    if printf '%s' "$SCENE_ARGS" | grep -q 'target='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=target" "seek target mode"
                    elif printf '%s' "$SCENE_ARGS" | grep -q 'delta='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=delta" "seek delta mode"
                    else
                        fail "seek 场景缺少 target 或 delta"
                    fi
                    ;;
                quality_change)
                    SCENE_ARGS=$(sed -n 's/^args=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
                    REQUESTED_QN=$(printf '%s' "$SCENE_ARGS" | sed -n 's/.*qn=\([^;]*\).*/\1/p')
                    if [ -z "$REQUESTED_QN" ]; then
                        fail "quality_change 缺少 qn"
                    else
                        check_both wiliwili.log "[run=$RUN_ID]" "scene=quality_change qn=$REQUESTED_QN old_qn=" "quality qn 请求"
                        check_both wiliwili.log "$PREFIX" "scene=quality_change qn=$REQUESTED_QN old_session=" "quality 新 session 完成"
                        OLD_SESSION=$(grep -F "scene=quality_change qn=$REQUESTED_QN" "$RUN_DIR/wiliwili.log" 2>/dev/null |
                            grep -F "status=sent" | sed -n 's/.* old_session=\([^ ]*\).*/\1/p' | head -1)
                        if [ -z "$OLD_SESSION" ]; then
                            fail "quality_change 缺少旧 session"
                        elif [ "$OLD_SESSION" = "$SESSION" ]; then
                            fail "quality_change 未创建新 session"
                        else
                            pass "quality_change 旧 session=$OLD_SESSION，新 session=$SESSION"
                            check_both wiliwili.log "[run=$RUN_ID][session=$OLD_SESSION]" "content start" "旧 session content start"
                        fi
                        if [ -n "$UI_QN" ] && [ "$UI_QN" = "$REQUESTED_QN" ]; then
                            pass "新 session qn=$UI_QN 与请求一致"
                        else
                            fail "新 session qn=$UI_QN 与请求 qn=$REQUESTED_QN 不一致"
                        fi
                    fi
                    ;;
                pause_resume)
                    check_both wiliwili.log "$PREFIX" "set pause=" "CMD pause/resume"
                    check_both wiliwili.log "$PREFIX" "paused" "EVT paused"
                    check_both wiliwili.log "$PREFIX" "resumed" "EVT resumed"
                    ;;
            esac

            if grep -F "END_FILE" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
                check_pattern wiliwili.log "][UI]$PREFIX[gen=-][req=-] content end reason=" "content end"
            fi
            ;;
        page_nav|none)
            check_pattern wiliwili.log "][UI]$PREFIX[gen=-][req=-] content start content=page" "page content start"
            check_pattern wiliwili.log "][UI]$PREFIX[gen=-][req=-] content end reason=user_stop" "page content end"
            PAGE_CONTENT=$(sed -n 's/^content=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
            if [ -n "$PAGE_CONTENT" ]; then
                check_both wiliwili.log "$PREFIX" "content start content=page id=$PAGE_CONTENT" "page 目标与 manifest 一致"
            fi
            ;;
        *)
            pass "scene=$SCENE 暂无额外内容契约"
            ;;
    esac
fi

if grep -E 'upsig=|trid=|buvid=|[?][^ ]*[=]' "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
    fail "统一日志含签名 URL 参数"
else
    pass "统一日志未发现签名 URL 参数"
fi

HEALTH_FORCE=$(sed -n 's/^health_force=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
if [ "$HEALTH_FORCE" = "1" ]; then
    check_pattern wiliwili.log "][COMPAT][run=$RUN_ID]" "compat 统一行"
    check_pattern wiliwili.log "health stall detected reason=soft" "compat 软卡顿检测"
    check_pattern wiliwili.log "health hint shown reason=soft" "播放页软卡顿提示"
    HEALTH_LINES=$(grep -c -F 'health stall detected reason=soft' "$RUN_DIR/wiliwili.log" 2>/dev/null || true)
    if [ "$HEALTH_LINES" -eq 1 ]; then
        pass "软卡顿提示每个 run 只发生一次"
    else
        fail "软卡顿提示次数=$HEALTH_LINES，期望 1"
    fi
    if grep -E "watchdog stall|END_FILE\(ERROR\)" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        fail "软卡顿提示不应触发 watchdog 或 ERROR"
    else
        pass "软卡顿提示未触发 watchdog/ERROR"
    fi
fi

if grep -F "watchdog stall" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
    check_pattern wiliwili.log "health stall detected reason=hard" "compat 硬卡顿检测"
    check_pattern wiliwili.log "health hint shown reason=hard" "播放页硬卡顿提示"
    if grep -F "scene=auto_quality_downgrade" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        if grep -F "scene=auto_quality_downgrade reason=hard_stall from_qn=" "$RUN_DIR/wiliwili.log" 2>/dev/null | grep -F "status=sent" >/dev/null 2>&1; then
            pass "硬卡顿自动降级请求"
        else
            fail "硬卡顿自动降级请求缺失"
        fi
        if grep -F "scene=auto_quality_downgrade reason=hard_stall from_qn=" "$RUN_DIR/wiliwili.log" 2>/dev/null | grep -F "status=ok" >/dev/null 2>&1; then
            pass "硬卡顿自动降级完成"
        else
            fail "硬卡顿自动降级完成缺失"
        fi
        AUTO_DOWNGRADE_QN=$(grep -F "scene=auto_quality_downgrade reason=hard_stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
            sed -n 's/.* to_qn=\([^ ]*\).*/\1/p' | sort -u | head -1)
        AUTO_DOWNGRADE_FROM_QN=$(grep -F "scene=auto_quality_downgrade reason=hard_stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
            sed -n 's/.* from_qn=\([^ ]*\).*/\1/p' | sort -u | head -1)
        case "$AUTO_DOWNGRADE_FROM_QN" in
            80|112|116) pass "自动降级来源 qn=$AUTO_DOWNGRADE_FROM_QN" ;;
            *) fail "自动降级来源 qn 非 1080P 三档: $AUTO_DOWNGRADE_FROM_QN" ;;
        esac
        if [ "$AUTO_DOWNGRADE_QN" = "64" ]; then
            pass "自动降级目标 qn=64"
            if grep -F "scene=auto_quality_downgrade reason=hard_stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
               grep -E "status=error|status=timeout" >/dev/null 2>&1; then
                fail "自动降级后出现失败/超时"
            else
                pass "自动降级无失败/超时"
            fi
            AUTO_SENT_LINE=$(grep -nF "scene=auto_quality_downgrade reason=hard_stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
                grep -F "status=sent" | head -1 | cut -d: -f1)
            if [ -n "$AUTO_SENT_LINE" ]; then
                check_pattern wiliwili.log "watchdog replay cancelled reason=" "watchdog 旧流重放已取消"
                if awk -v n="$AUTO_SENT_LINE" 'NR > n && /replay open/ { found = 1; exit } END { exit !found }' \
                    "$RUN_DIR/wiliwili.log"; then
                    fail "自动降级后仍重放旧流"
                else
                    pass "自动降级后无旧流重放"
                fi
            fi
            check_pattern wiliwili.log "detail=cid=" "降级后仍有权威内容详情"
            if grep -E "content start .* qn=64 " "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
                pass "新 session content start qn=64"
            else
                fail "新 session content start 未记录 qn=64"
            fi
            AUTO_DOWNGRADE_NEW_SESSION=$(grep -F "scene=auto_quality_downgrade reason=hard_stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
                grep -F "status=ok" | sed -n 's/.* new_session=\([^ ]*\).*/\1/p' | head -1)
            if [ -n "$AUTO_DOWNGRADE_NEW_SESSION" ]; then
                check_both wiliwili.log "[session=$AUTO_DOWNGRADE_NEW_SESSION]" "playback start status=ok" "降级后真实起播"
                if grep -F "[session=$AUTO_DOWNGRADE_NEW_SESSION]" "$RUN_DIR/wiliwili.log" 2>/dev/null |
                   grep -F "media " | grep -F "qn=30064 " >/dev/null 2>&1; then
                    pass "降级后 player media qn=64"
                else
                    fail "降级后 player media 未记录 qn=64"
                fi
            else
                fail "自动降级完成日志缺少 new_session"
            fi
        else
            fail "自动降级目标错误: qn=$AUTO_DOWNGRADE_QN"
        fi
    fi
else
    pass "本 run 无 watchdog 硬卡顿"
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "RESULT: PASS $RUN_DIR"
    exit 0
fi
echo "RESULT: FAIL $RUN_DIR ($FAILURES)"
exit 1
