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
check_file wiliwili.log
check_file dmesg.before.txt
check_file dmesg.after.txt
check_file launch_cmdline.txt

# 设备重启后 /tmp/wiliwili.log 本来就不存在，debug.sh 会生成空 prestart.log。
if [ -f "$RUN_DIR/prestart.log" ]; then
    pass "prestart.log 存在（空文件表示重启后的干净基线）"
else
    fail "prestart.log 缺失"
fi

RUN_ID=$(sed -n 's/^run_id=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
SCENE=$(sed -n 's/^scene=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
FAIL_AUDIO_SEEK=$(sed -n 's/^fail_audio_seek=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
FAIL_AUDIO_CREATE=$(sed -n 's/^fail_audio_create=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
NO_AUDIO=$(sed -n 's/^no_audio=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
FORCE_FAILOVER=$(sed -n 's/^force_failover=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
FORCE_WATCHDOG=$(sed -n 's/^force_watchdog=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
PRIMARY_ERROR=$(sed -n 's/^primary_error=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
FAIL_QUALITY_START=$(sed -n 's/^fail_quality_start=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
EXPECT_FAILURE=0
if [ "$FAIL_AUDIO_SEEK" = "1" ] || [ "$FAIL_AUDIO_CREATE" = "1" ] || [ "$FAIL_QUALITY_START" = "1" ]; then
    EXPECT_FAILURE=1
fi
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
for key in no_audio fail_audio_seek fail_audio_create force_failover force_watchdog primary_error fail_quality_start; do
    if grep -q "^$key=" "$RUN_DIR/manifest.txt" 2>/dev/null; then
        pass "manifest 含 $key"
    else
        pass "manifest 未设置 $key（历史 run 或无故障注入）"
    fi
done
check_file "dmesg.$SCENE.start.txt"
if [ "$EXPECT_FAILURE" = "1" ]; then
    pass "失败注入 run 不要求 dmesg.$SCENE.ok.txt"
else
    check_file "dmesg.$SCENE.ok.txt"
fi

check_pattern wiliwili.log "[boot] run=$RUN_ID" "boot run id"
check_pattern wiliwili.log "][CMD][run=$RUN_ID][session=" "统一 CMD 行"
check_both wiliwili.log "scene=$SCENE" "status=start" "CMD status=start"
if [ "$EXPECT_FAILURE" = "1" ]; then
    if grep -F "status=error reason=playback_error" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        pass "失败注入 run 得到播放器 error 终态"
    elif grep -F "scene=$SCENE" "$RUN_DIR/wiliwili.log" 2>/dev/null |
         grep -E "status=timeout|status=error" >/dev/null 2>&1; then
        pass "失败注入 run 得到场景失败终态"
    else
        fail "失败注入 run 缺少播放器 error 或场景 timeout/error 终态"
    fi
else
    check_both wiliwili.log "scene=$SCENE" "status=ok" "CMD status=ok"
fi

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

SESSION_STATUS="status=ok"
if [ "$EXPECT_FAILURE" = "1" ]; then
    SESSION_STATUS="status=start"
fi
SESSION=$(grep -F "scene=$SCENE" "$RUN_DIR/wiliwili.log" 2>/dev/null |
    grep -F "$SESSION_STATUS" |
    sed -n 's/.*\[session=\([^]]*\)\].*/\1/p' | grep -v '^-' | head -1)
if [ -z "$SESSION" ]; then
    fail "场景没有有效 session"
else
    pass "场景 session=$SESSION"
    PREFIX="[run=$RUN_ID][session=$SESSION]"

    case "$SCENE" in
        play|seek_forward|seek_backward|pause_resume|pause_repeat|pause_race|pause_long|pause_seek|quality_change|pause_quality|continuous_seek)
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
                seek_forward|seek_backward|pause_seek|continuous_seek)
                    check_both wiliwili.log "$PREFIX" "seek mode=" "CMD seek"
                    check_both wiliwili.log "$PREFIX" "seeked" "EVT seeked"
                    SCENE_ARGS=$(sed -n 's/^args=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
                    if [ "$SCENE" = "continuous_seek" ]; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=continuous" "continuous seek sent"
                        if printf '%s' "$SCENE_ARGS" | grep -q 'target2_ms='; then
                            check_both wiliwili.log "$PREFIX" "scene=$SCENE seek_status=ok" "continuous seek first target"
                        else
                            fail "continuous_seek 缺少 target2_ms"
                        fi
                    elif printf '%s' "$SCENE_ARGS" | grep -q 'target_ms='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=target" "seek target_ms mode"
                    elif printf '%s' "$SCENE_ARGS" | grep -q 'delta_ms='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=delta" "seek delta_ms mode"
                    elif printf '%s' "$SCENE_ARGS" | grep -q 'target='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=target" "seek target mode"
                    elif printf '%s' "$SCENE_ARGS" | grep -q 'delta='; then
                        check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=delta" "seek delta mode"
                    else
                        fail "seek 场景缺少 target/delta 或 target_ms/delta_ms"
                    fi
                    ;;
                quality_change)
                    SCENE_ARGS=$(sed -n 's/^args=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
                    REQUESTED_QN=$(printf '%s' "$SCENE_ARGS" | tr '; ' '\n\n' |
                        sed -n 's/^qn=\([^;]*\)$/\1/p' | head -1)
                    if [ -z "$REQUESTED_QN" ]; then
                        fail "quality_change 缺少 qn"
                    else
                        check_both wiliwili.log "[run=$RUN_ID]" "scene=quality_change qn=$REQUESTED_QN old_qn=" "quality qn 请求"
                        if [ "$FAIL_QUALITY_START" = "1" ]; then
                            pass "升档失败 run 跳过原档位完成判据，改验自动降级"
                        else
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
                pause_repeat)
                    check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=second_pause" "second pause sent"
                    check_both wiliwili.log "$PREFIX" "set pause=yes status=duplicate" "重复 pause 幂等"
                    check_both wiliwili.log "$PREFIX" "paused status=ok" "EVT paused"
                    check_both wiliwili.log "$PREFIX" "resumed status=ok" "EVT resumed"
                    ;;
                pause_race)
                    check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=pause_resume_no_wait" "pause/resume 竞态 sent"
                    check_both wiliwili.log "$PREFIX" "set pause=yes status=sent" "CMD pause"
                    check_both wiliwili.log "$PREFIX" "set pause=no status=sent" "CMD resume"
                    check_both wiliwili.log "$PREFIX" "SYNC_START_PREPARE" "竞态后恢复准备"
                    check_both wiliwili.log "$PREFIX" "epoch_reused=1" "竞态后复用 epoch"
                    check_both wiliwili.log "$PREFIX" "SYNC_READY" "竞态后重新准备完成"
                    check_both wiliwili.log "$PREFIX" "SYNC_START epoch=" "竞态后 PLAY 启动"
                    ;;
                pause_long)
                    check_both wiliwili.log "$PREFIX" "scene=$SCENE paused_hold_seconds=" "长时间暂停参数"
                    check_both wiliwili.log "$PREFIX" "paused status=ok" "EVT paused"
                    check_both wiliwili.log "$PREFIX" "resumed status=ok" "EVT resumed"
                    if grep -F "watchdog stall" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
                        fail "长时间暂停触发 watchdog"
                    else
                        pass "长时间暂停未触发 watchdog"
                    fi
                    ;;
                pause_seek)
                    check_both wiliwili.log "$PREFIX" "scene=$SCENE mode=target target_ms=" "paused seek sent"
                    check_both wiliwili.log "$PREFIX" "scene=$SCENE seek_status=ok paused=1" "paused seek target reached"
                    check_both wiliwili.log "$PREFIX" "paused status=ok" "EVT paused"
                    check_both wiliwili.log "$PREFIX" "resumed status=ok" "EVT resumed"
                    check_pattern wiliwili.log "SYNC_TARGET" "paused seek SYNC_TARGET"
                    ;;
                pause_quality)
                    RUN_PREFIX="[run=$RUN_ID]"
                    SCENE_ARGS=$(sed -n 's/^args=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
                    REQUESTED_QN=$(printf '%s' "$SCENE_ARGS" | tr '; ' '\n\n' |
                        sed -n 's/^qn=\([^;]*\)$/\1/p' | head -1)
                    if [ -z "$REQUESTED_QN" ]; then
                        fail "pause_quality 缺少 qn"
                    else
                        check_both wiliwili.log "$RUN_PREFIX" "scene=$SCENE qn=$REQUESTED_QN old_qn=" "paused quality request"
                        check_both wiliwili.log "$RUN_PREFIX" "scene=$SCENE qn=$REQUESTED_QN" "quality_status=ok" "paused quality new session"
                        check_both wiliwili.log "$RUN_PREFIX" "paused status=ok" "EVT paused"
                        check_both wiliwili.log "$RUN_PREFIX" "resumed status=ok" "EVT resumed"
                    fi
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
    if grep -F "health stall detected reason=soft" "$RUN_DIR/wiliwili.log" 2>/dev/null |
       grep -E "bp=[01]( |$)" >/dev/null 2>&1; then
        pass "软卡顿日志含反压状态"
    else
        fail "软卡顿日志缺少 bp 字段"
    fi
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
    WATCHDOG_LINES=$(grep -c -F "watchdog stall" "$RUN_DIR/wiliwili.log" 2>/dev/null || true)
    WATCHDOG_FIELD_LINES=$(grep -F "watchdog stall" "$RUN_DIR/wiliwili.log" 2>/dev/null |
        grep -c -E "bp=[01] vq=[0-9]+ms pos=[0-9]+(\.[0-9]+)?ms" || true)
    if [ "$WATCHDOG_LINES" -gt 0 ] && [ "$WATCHDOG_FIELD_LINES" -eq "$WATCHDOG_LINES" ]; then
        pass "watchdog 硬卡顿日志含 bp/vq/pos"
    else
        fail "watchdog 硬卡顿日志字段不完整: $WATCHDOG_FIELD_LINES/$WATCHDOG_LINES"
    fi
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

# D28：统一日志中的同步事件必须带 epoch；START 不得早于 READY。
SYNC_COUNT=$(grep -c -E 'SYNC_(BEGIN|TARGET|READY|START|DRIFT|END)' "$RUN_DIR/wiliwili.log" 2>/dev/null || true)
if [ "${SYNC_COUNT:-0}" -eq 0 ]; then
    pass "本 run 无 D28 SYNC_* 日志"
else
    pass "D28 SYNC_* 日志数量=$SYNC_COUNT"
    SYNC_NO_EPOCH=$(grep -E 'SYNC_(BEGIN|TARGET|READY|START|DRIFT|END)' "$RUN_DIR/wiliwili.log" 2>/dev/null |
        grep -vc 'epoch=' || true)
    if [ "${SYNC_NO_EPOCH:-0}" -eq 0 ]; then
        pass "所有 SYNC_* 行带 epoch"
    else
        fail "有 $SYNC_NO_EPOCH 行 SYNC_* 缺少 epoch"
    fi
    SYNC_END_NO_STATUS=$(grep -F 'SYNC_END' "$RUN_DIR/wiliwili.log" 2>/dev/null |
        grep -vc 'status=' || true)
    if [ "${SYNC_END_NO_STATUS:-0}" -eq 0 ]; then
        pass "所有 SYNC_END 带 status"
    else
        fail "有 $SYNC_END_NO_STATUS 行 SYNC_END 缺少 status"
    fi
    READY_LINE=$(grep -nF 'SYNC_READY' "$RUN_DIR/wiliwili.log" 2>/dev/null |
        head -1 | cut -d: -f1)
    START_LINE=$(grep -nE 'SYNC_START ' "$RUN_DIR/wiliwili.log" 2>/dev/null |
        head -1 | cut -d: -f1)
    if [ -z "$START_LINE" ] || { [ -n "$READY_LINE" ] && [ "$READY_LINE" -le "$START_LINE" ]; }; then
        pass "SYNC_START 不早于 SYNC_READY"
    else
        fail "SYNC_START 早于 SYNC_READY"
    fi
    if [ "$SCENE" = "pause_resume" ] || [ "$SCENE" = "pause_repeat" ] || \
       [ "$SCENE" = "pause_long" ]; then
        check_pattern wiliwili.log "SYNC_TARGET" "pause 场景 SYNC_TARGET"
    fi
    if [ "$SCENE" = "seek_forward" ] || [ "$SCENE" = "seek_backward" ]; then
        if grep -E 'target_ms=|delta_ms=' "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
            pass "seek 场景毫秒目标"
        else
            fail "seek 场景缺少毫秒目标"
        fi
    fi
fi

if [ "$NO_AUDIO" = "1" ]; then
    check_pattern wiliwili.log "debug fault no-audio injected" "no-audio 故障注入"
    check_pattern wiliwili.log "disabled by source spec, skip" "播放器跳过音频"
    check_pattern wiliwili.log "no usable audio, silent playback" "silent 路径"
    check_pattern wiliwili.log "SYNC_READY" "无音频仍完成准备屏障"
fi
if [ "$FAIL_AUDIO_SEEK" = "1" ] || [ "$FAIL_AUDIO_CREATE" = "1" ]; then
    if [ "$FAIL_AUDIO_SEEK" = "1" ]; then
        check_pattern wiliwili.log "debug fault audio-seek failed" "audio seek 故障注入"
    else
        check_pattern wiliwili.log "debug fault audio-create failed" "audio create 故障注入"
    fi
    if grep -E "seek done .* result=failed muted=1" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        pass "失败 ACK 保持 mute"
    else
        fail "失败 ACK 未保持 mute"
    fi
    check_pattern wiliwili.log "status=error reason=playback_error" "失败后 compat 写 error 终态"
fi
if [ "$FORCE_FAILOVER" = "1" ]; then
    check_pattern wiliwili.log "debug fault force-failover at playback start" "failover 故障注入"
    if grep -F "failover to backup gen=" "$RUN_DIR/wiliwili.log" |
       grep -E "source=(heartbeat|resume_position|active_target)" >/dev/null 2>&1; then
        pass "failover 使用故障时位置"
    else
        fail "failover 未使用故障时位置"
    fi
    check_pattern wiliwili.log "reason=failover" "failover 新 epoch"
fi
if [ "$PRIMARY_ERROR" = "1" ]; then
    check_pattern wiliwili.log "debug fault primary video error" "主源错误故障注入"
    check_pattern wiliwili.log "error message=debug fault primary video error" "daemon 上报真实 error 事件"
    if grep -F "failover to backup gen=" "$RUN_DIR/wiliwili.log" |
       grep -E "source=(heartbeat|resume_position|active_target)" >/dev/null 2>&1; then
        pass "主源错误后使用故障时位置"
    else
        fail "主源错误后未使用故障时位置"
    fi
    check_pattern wiliwili.log "reason=failover" "主源错误新 epoch"
    if grep -F "SYNC_END" "$RUN_DIR/wiliwili.log" | grep -F "status=error reason=playback_error" >/dev/null 2>&1; then
        fail "备用源接管失败，主源错误未被 failover 消化"
    else
        pass "备用源接管成功"
    fi
fi
if [ "$FORCE_WATCHDOG" = "1" ]; then
    check_pattern wiliwili.log "debug fault force-watchdog at playback stable" "watchdog 故障注入"
    check_pattern wiliwili.log "action=forced_debug status=kill" "watchdog 强杀"
    check_pattern wiliwili.log "status=error reason=watchdog" "旧 epoch error 终态"
    check_pattern wiliwili.log "replay open pos=" "新 daemon replay"
    check_pattern wiliwili.log "reason=watchdog_replay" "replay 新 epoch"
fi

if [ "$FAIL_QUALITY_START" = "1" ]; then
    check_pattern wiliwili.log "debug fault quality start failed" "升档起播失败注入"
    check_pattern wiliwili.log "error message=debug fault quality start failed" "daemon 上报升档失败"
    if grep -E "scene=auto_quality_downgrade reason=quality_change_failure from_qn=(80|112|116) to_qn=64 old_session=" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        pass "升档失败自动降级请求"
    else
        fail "升档失败自动降级请求"
    fi
    if grep -E "scene=auto_quality_downgrade reason=quality_change_failure from_qn=(80|112|116) to_qn=64" "$RUN_DIR/wiliwili.log" | grep -F "status=ok" >/dev/null 2>&1; then
        pass "升档失败自动降级完成"
    else
        fail "升档失败自动降级完成"
    fi
    if grep -E "content start .* qn=64 " "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        pass "降级后新 session qn=64"
    else
        fail "降级后新 session 未记录 qn=64"
    fi
    if grep -F "[PLAYER]" "$RUN_DIR/wiliwili.log" | grep -F "debug fault quality start failed" | wc -l | grep -q '^1$'; then
        pass "升档失败注入只触发一次"
    else
        fail "升档失败注入触发次数不为 1"
    fi
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "RESULT: PASS $RUN_DIR"
    exit 0
fi
echo "RESULT: FAIL $RUN_DIR ($FAILURES)"
exit 1
