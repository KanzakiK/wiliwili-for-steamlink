//
// Created by fang on 2023/1/3.
//

#include <utility>
#include <cmath>
#include <borealis/core/thread.hpp>
#include <borealis/core/touch/tap_gesture.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>

#include "activity/player_activity.hpp"
#include "fragment/player_collection.hpp"
#include "fragment/player_coin.hpp"
#include "fragment/player_single_comment.hpp"
#include "utils/config_helper.hpp"
#include "utils/debug_log.hpp"
#include "utils/dialog_helper.hpp"
#include "utils/number_helper.hpp"
#include "presenter/comment_related.hpp"
#include "utils/shortcut_helper.hpp"
#include "view/qr_image.hpp"
#include "view/video_view.hpp"
#include "view/grid_dropdown.hpp"
#include "view/subtitle_core.hpp"
#include "view/mpv_core.hpp"

class DataSourceCommentList : public RecyclingGridDataSource, public CommentAction {
public:
    DataSourceCommentList(bilibili::VideoCommentListResult result, uint64_t aid, int mode, std::function<void(void)> cb)
        : dataList(std::move(result)), aid(aid), commentMode(mode), switchModeCallback(cb) {}
    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        if (index == 0) {
            VideoCommentSort* item = (VideoCommentSort*)recycler->dequeueReusableCell("Sort");
            item->setHeight(30);
            item->setFocusable(false);
            if (commentMode == 3) {
                item->hintLabel->setText("wiliwili/player/comment_sort/top"_i18n);
                item->sortLabel->setText("wiliwili/player/comment_sort/sort_top"_i18n);
            } else {
                item->hintLabel->setText("wiliwili/player/comment_sort/new"_i18n);
                item->sortLabel->setText("wiliwili/player/comment_sort/sort_new"_i18n);
            }
            return item;
        }
        if (index == 1) {
            VideoCommentReply* item = (VideoCommentReply*)recycler->dequeueReusableCell("Reply");
            item->setHeight(40);
            return item;
        }

        //从缓存列表中取出 或者 新生成一个表单项
        VideoComment* item = (VideoComment*)recycler->dequeueReusableCell("Cell");

        item->setData(this->dataList[index - 2]);
        return item;
    }

    size_t getItemCount() override { return dataList.size() + 2; }

    void onItemSelected(RecyclingGrid* recycler, size_t index) override {
        if (index == 0) {
            if (switchModeCallback) switchModeCallback();
            return;
        }
        if (index == 1) {
            if (!DialogHelper::checkLogin()) return;
            // 回复评论
            brls::Application::getImeManager()->openForText(
                [this, recycler](const std::string& text) {
                    if (text.empty()) return;
                    this->commentReply(text, std::to_string(aid), 0, 0, 1,
                                       [this, recycler](const bilibili::VideoCommentAddResult& result) {
                                           this->dataList.insert(dataList.begin(), result.reply);
                                           recycler->reloadData();
                                       });
                },
                "wiliwili/player/single_comment/hint"_i18n, "", 500, "", 0);
            return;
        }

        auto* item = dynamic_cast<VideoComment*>(recycler->getGridItemByIndex(index));
        if (!item) return;

        auto* view = new PlayerSingleComment();
        view->setCommentData(dataList[index - 2], item->getY(), 1);
        auto container = new brls::AppletFrame(view);
        container->setHeaderVisibility(brls::Visibility::GONE);
        container->setFooterVisibility(brls::Visibility::GONE);
        container->setInFadeAnimation(true);
        brls::Application::pushActivity(new brls::Activity(container));

        view->likeStateEvent.subscribe([this, item, index](size_t value) {
            auto& itemData  = dataList[index - 2];
            itemData.action = value;
            item->setLiked(value);
        });
        view->likeNumEvent.subscribe([this, item, index](size_t value) {
            auto& itemData = dataList[index - 2];
            itemData.like  = value;
            item->setLikeNum(value);
        });
        view->replyNumEvent.subscribe([this, item, index](size_t value) {
            auto& itemData  = dataList[index - 2];
            itemData.rcount = value;
            item->setReplyNum(value);
        });
        view->deleteEvent.subscribe([this, recycler, index]() {
            dataList.erase(dataList.begin() + index - 2);
            recycler->reloadData();
            // 重新设置一下焦点到 recycler 的默认 cell （顶部）
            brls::Application::giveFocus(recycler);
        });
    }

    void appendData(const bilibili::VideoCommentListResult& data) {
        bool skip = false;
        for (auto& i : data) {
            skip = false;
            for (auto& j : this->dataList) {
                if (j.rpid == i.rpid) {
                    skip = true;
                    break;
                }
            }
            if (!skip) this->dataList.push_back(i);
        }
    }

    void clearData() override { this->dataList.clear(); }

private:
    bilibili::VideoCommentListResult dataList;
    uint64_t aid;
    int commentMode                              = 3;  // 2: 按时间；3: 按热度
    std::function<void(void)> switchModeCallback = nullptr;
};

class QualityCell : public RecyclingGridItem {
public:
    QualityCell() { this->inflateFromXMLRes("xml/views/player_quality_cell.xml"); }

    void setSelected(bool selected) {
        brls::Theme theme = brls::Application::getTheme();

        this->selected = selected;
        this->checkbox->setVisibility(selected ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        this->title->setTextColor(selected ? theme["brls/list/listItem_value_color"] : theme["brls/text"]);
    }

    bool getSelected() { return this->selected; }

    BRLS_BIND(brls::Label, title, "cell/title");
    BRLS_BIND(brls::Box, loginLabel, "cell/login");
    BRLS_BIND(brls::Box, vipLabel, "cell/vip");
    BRLS_BIND(brls::CheckBox, checkbox, "cell/checkbox");

    static RecyclingGridItem* create() { return new GridRadioCell(); }

private:
    bool selected = false;
};

class QualityDataSource : public DataSourceDropdown {
public:
    QualityDataSource(bilibili::VideoUrlResult result, BaseDropdown* view)
        : DataSourceDropdown(view), data(std::move(result)) {
        login = !ProgramConfig::instance().getCSRF().empty();
        // Steam Link 硬解只到 1080P@60，过滤掉 4K/HDR/8K 候选项
        std::vector<int> supportedQuality;
        std::vector<std::string> supportedDescription;
        const size_t count = data.accept_quality.size() < data.accept_description.size()
                                 ? data.accept_quality.size()
                                 : data.accept_description.size();
        for (size_t i = 0; i < count; i++) {
            if (data.accept_quality[i] > 116 && data.accept_quality[i] != 192) continue;
            supportedQuality.push_back(data.accept_quality[i]);
            supportedDescription.push_back(data.accept_description[i]);
        }
        data.accept_quality     = supportedQuality;
        data.accept_description = supportedDescription;
    }

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        QualityCell* item = (QualityCell*)recycler->dequeueReusableCell("Cell");

        int quality = data.accept_quality[index];

        item->loginLabel->setVisibility(brls::Visibility::GONE);
        item->vipLabel->setVisibility(brls::Visibility::GONE);

        if (quality > 80) {
            item->vipLabel->setVisibility(brls::Visibility::VISIBLE);
        } else if (quality >= 32) {
            if (!login) item->loginLabel->setVisibility(brls::Visibility::VISIBLE);
        }

        auto r = this->data.accept_description[index];
        item->title->setText(this->data.accept_description[index]);
        item->setSelected(index == dropdown->getSelected());
        return item;
    }

    size_t getItemCount() override { return (std::min)(data.accept_quality.size(), data.accept_description.size()); }

    void clearData() override {}

private:
    bilibili::VideoUrlResult data;
    bool login;
};

/// BasePlayerActivity

void BasePlayerActivity::onContentAvailable() { this->setCommonData(); }

void BasePlayerActivity::setCommonData() {
    // 视频评论
    recyclingGrid->registerCell("Cell", []() { return VideoComment::create(); });

    recyclingGrid->registerCell("Reply", []() { return VideoCommentReply::create(); });

    recyclingGrid->registerCell("Sort", []() { return VideoCommentSort::create(); });

    recyclingGrid->setDefaultCellFocus(1);

    recyclingGrid->registerAction("wiliwili/home/common/switch"_i18n, brls::ControllerButton::BUTTON_X,
                                  [this](brls::View* view) -> bool {
                                      this->setCommentMode();
                                      return true;
                                  });

    recyclingGrid->registerAction(ShortcutHelper::getRefresh(),
                                  [this](brls::View* view) -> bool {
                                      this->setCommentMode();
                                      return true;
                                  });

    // 切换右侧Tab
    this->registerAction(
        "上一项", brls::ControllerButton::BUTTON_LT,
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        },
        true);
    this->registerAction(
        "上一项", brls::ControllerButton::BUTTON_LB,
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        },
        true);

    this->registerAction(
        "下一项", brls::ControllerButton::BUTTON_RT,
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        },
        true);
    this->registerAction(
        "下一项", brls::ControllerButton::BUTTON_RB,
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        },
        true);

    this->registerAction(
        ShortcutHelper::getLast(),
        [this](brls::View* view) -> bool {
            tabFrame->focus2LastTab();
            return true;
        });
    this->registerAction(
        ShortcutHelper::getNext(),
        [this](brls::View* view) -> bool {
            tabFrame->focus2NextTab();
            return true;
        });
    video->registerCommonActions(this);

    // 调整清晰度
    this->registerAction("wiliwili/player/quality"_i18n, brls::ControllerButton::BUTTON_START,
                         [this](brls::View* view) -> bool {
                             this->setVideoQuality();
                             return true;
                         });

    this->btnQR->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnQR->getParent()));

    this->btnAgree->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnAgree->getParent()));

    this->btnCoin->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnCoin->getParent()));

    this->btnFavorite->getParent()->addGestureRecognizer(
        new brls::TapGestureRecognizer(this->btnFavorite->getParent()));

    this->videoUserInfo->addGestureRecognizer(new brls::TapGestureRecognizer(this->videoUserInfo));

    this->setRelationButton(false, false, false);

    eventSubscribeID = MPV_E->subscribe([this](MpvEventEnum event) {
        // 上一次报告历史记录的时间点
        static int64_t lastProgress = MPVCore::instance().video_progress;
        switch (event) {
            case MpvEventEnum::UPDATE_PROGRESS: {
                // 每15秒同步一次进度
                if (lastProgress + 15 < MPVCore::instance().video_progress) {
                    lastProgress = MPVCore::instance().video_progress;
                    this->reportCurrentProgress(lastProgress, MPVCore::instance().duration);
                } else if (MPVCore::instance().video_progress < lastProgress) {
                    // 当前播放时间小于上一次上传历史记录的时间点
                    // 发生于向前拖拽进度的时候，此时重置lastProgress的值
                    lastProgress = MPVCore::instance().video_progress;
                }
                // 检查视频链接是否有效
                auto timeNow = std::chrono::system_clock::now();
                if (timeNow > videoDeadline) {
                    // 设置视频加载后跳转的时间
                    setProgress(MPVCore::instance().video_progress);

                    // 暂停播放
                    MPVCore::instance().pause();

                    // 10s 后重新尝试
                    videoDeadline = timeNow + std::chrono::seconds(10);

                    // 有效期已过，重新请求视频链接
                    auto self = dynamic_cast<PlayerSeasonActivity*>(this);
                    if (self) {
                        this->requestSeasonVideoUrl(episodeResult.bvid, episodeResult.cid, false);
                    } else {
                        this->requestVideoUrl(videoDetailResult.bvid, videoDetailPage.cid, false);
                    }

                    //todo: 如果有选择的字幕加载对应的字幕
                }
                break;
            }
            case MpvEventEnum::END_OF_FILE:
                // 尝试自动加载下一分集
                // 如果当前最顶层是Dialog就放弃自动播放，因为有可能是用户点开了收藏或者投币对话框
                {
                    int64_t& progress = MPVCore::instance().video_progress;
                    int64_t& duration = MPVCore::instance().duration;
                    int clipEnd       = videoUrlResult.clipEnd;
                    auto seasonCustom = ProgramConfig::instance().getSeasonCustom(seasonInfo.season_id);
                    if (seasonCustom.custom_clip) {
                        clipEnd = duration - seasonCustom.clip_end;
                    }

                    // 播放到一半没网时也会触发EOF，这里简单判断一下结束播放时的播放条位置是否在片尾或视频结尾附近
                    if ((duration - progress > 5 || progress - duration > 5) && !(clipEnd > 0 && clipEnd - progress < 5)) {
                        brls::Logger::error("EOF: video: {} duration: {} clipEnd: {}", progress, duration, clipEnd);
                        return;
                    }
                    if (PLAYER_STRATEGY == PlayerStrategy::LOOP) {
                        MPVCore::instance().seek(0);
                        MPVCore::instance().resume();
                        return;
                    }
                    auto stack    = brls::Application::getActivitiesStack();
                    Activity* top = stack[stack.size() - 1];
                    if (!dynamic_cast<BasePlayerActivity*>(top) &&
                        !dynamic_cast<VideoView*>(top->getContentView()->getView("video"))) {
                        // 最顶层没有 video 组件，说明用户打开了评论或者其他菜单
                        // 在这种情况下不执行自动播放其他视频显示重播按钮
                        APP_E->fire(VideoView::REPLAY, nullptr);
                    } else if (PLAYER_STRATEGY == PlayerStrategy::NEXT || PLAYER_STRATEGY == PlayerStrategy::RCMD) {
                        this->onIndexChangeToNext();
                    } else {
                        // 对于其他情况，显示重播按钮
                        APP_E->fire(VideoView::REPLAY, nullptr);
                    }
                }
                break;
            case MpvEventEnum::RESTART:
                this->updateVideoLink();
                break;
            case MpvEventEnum::LOADING_END:
                this->completeAutoDowngradeIfPlaying();
                break;
            case MpvEventEnum::MPV_FILE_ERROR:
                this->failAutoDowngrade("playback_error");
                break;
            default:
                break;
        }
    });

    customEventSubscribeID = APP_E->subscribe([this](const std::string& event, void* data) {
        if (event == VideoView::QUALITY_CHANGE) {
            this->setVideoQuality();
        } else if (event == VideoView::AUTO_QUALITY_DOWNGRADE) {
            if (data && this->autoDowngradeForHardStall())
                *static_cast<bool*>(data) = true;
        } else if (event == VideoView::SWITCH_TO_LAST) {
            // 历史播放进度储存在 SubtitleCore 中
            auto videoPage = SubtitleCore::instance().getSubtitleList();
            if (videoPage.last_play_cid == videoDetailPage.cid) {
                // 因为占用了切换弹幕的按键，所以在无效的情况下保持切换弹幕
                this->video->toggleDanmaku();
                return;
            }
            brls::Logger::debug("切换到历史播放进度：{}/{}", videoPage.last_play_cid, videoPage.last_play_time);
            for (auto& p : videoDetailResult.pages) {
                if (p.cid == videoPage.last_play_cid) {
                    this->onIndexChange(p.page - 1);
                    this->setProgress(videoPage.last_play_time / 1000);
                    break;
                }
            }
        } else if (event == "REQUEST_CAST_URL") {
            this->requestCastUrl();
        }
    });

    if (brls::Application::ORIGINAL_WINDOW_HEIGHT < 720) video->hideStatusLabel();

    video->hideOSDLockButton();
}

void BasePlayerActivity::showCollectionDialog(uint64_t id, int videoType) {
    if (!DialogHelper::checkLogin()) return;
    auto playerCollection = new PlayerCollection(id, videoType);
    auto dialog           = new brls::Dialog(playerCollection);
    dialog->addButton("wiliwili/home/common/save"_i18n, [this, id, videoType, playerCollection]() {
        this->addResource(id, videoType, playerCollection->isFavorite(), playerCollection->getAddCollectionList(),
                          playerCollection->getDeleteCollectionList());
    });
    playerCollection->registerAction(
        "", brls::ControllerButton::BUTTON_START,
        [this, id, videoType, playerCollection, dialog](...) {
            this->addResource(id, videoType, playerCollection->isFavorite(), playerCollection->getAddCollectionList(),
                              playerCollection->getDeleteCollectionList());
            dialog->dismiss();
            return true;
        },
        true);
    dialog->open();
}

void BasePlayerActivity::showCoinDialog(uint64_t aid) {
    if (!DialogHelper::checkLogin()) return;

    if (std::to_string(videoDetailResult.owner.mid) == ProgramConfig::instance().getUserID()) {
        DialogHelper::showDialog("wiliwili/player/coin/own"_i18n);
        return;
    }

    int coins = getCoinTolerate();
    if (coins <= 0) {
        DialogHelper::showDialog("wiliwili/player/coin/run_out"_i18n);
        return;
    }

    auto playerCoin = new PlayerCoin();
    if (coins == 1) playerCoin->hideTwoCoin();
    playerCoin->getSelectEvent()->subscribe(
        [this, playerCoin, aid](int value) { this->addCoin(aid, value, playerCoin->likeAtTheSameTime()); });
    auto dialog = new brls::Dialog(playerCoin);
    dialog->open();
}

void BasePlayerActivity::updateVideoLink() {
    // 显示进度继续用整数秒；控制目标在异步请求链接前冻结为毫秒。
    setProgress(MPVCore::instance().video_progress);
    double precise = MPVCore::instance().getPlaybackTime();
    if (precise <= 0.0)
        precise = (double)MPVCore::instance().video_progress;
    controlTargetMs = (int64_t)(precise * 1000.0 + 0.5);
    this->video->setControlTargetMs(controlTargetMs);
    int requestedQuality = sessionQualityOverride > 0 ? sessionQualityOverride
                                                      : defaultQuality;

    // dash
    if (!this->videoUrlResult.dash.video.empty()) {
        // dash格式的视频无需重复请求视频链接，这里简单的设置清晰度即可
        videoUrlResult.quality = requestedQuality;
        this->onVideoPlayUrl(videoUrlResult);
        return;
    }

    // flv
    if (dynamic_cast<PlayerSeasonActivity*>(this)) {
        this->requestSeasonVideoUrl(episodeResult.bvid, episodeResult.cid, true,
                                    requestedQuality);
    } else {
        this->requestVideoUrl(videoDetailResult.bvid, videoDetailPage.cid, true,
                              requestedQuality);
    }
}

std::string BasePlayerActivity::currentContentKey() const {
    if (seasonInfo.season_id > 0)
        return fmt::format("season:{}:{}:{}", seasonInfo.season_id, episodeResult.id,
                           episodeResult.bvid);
    return fmt::format("video:{}:{}", videoDetailResult.bvid, videoDetailPage.cid);
}

void BasePlayerActivity::resetSessionQualityOverride() {
    sessionQualityOverride          = 0;
    sessionQualityContentKey.clear();
    hardAutoDowngradeApplied        = false;
    autoDowngradePending            = false;
    autoDowngradeFromQn             = 0;
    autoDowngradeToQn               = 0;
    autoDowngradeNewSession.clear();
    autoDowngradeOldSession.clear();
    ++autoDowngradeRequestSeq;
}

bool BasePlayerActivity::autoDowngradeForHardStall() {
    if (!activityShown || hardAutoDowngradeApplied) return false;
    if (videoUrlResult.accept_quality.empty()) return false;

    int fromQn = videoUrlResult.quality;
    // 只对 1080P / 1080P+ / 1080P60 做一次性降级；720P 及以下不再继续降。
    if (fromQn != 80 && fromQn != 112 && fromQn != 116) return false;
    constexpr int kTargetQn = 64;
    bool targetAvailable = false;
    for (int q : videoUrlResult.accept_quality) {
        if (q == kTargetQn) {
            targetAvailable = true;
            break;
        }
    }
    if (!targetAvailable) return false;

    hardAutoDowngradeApplied   = true;
    sessionQualityOverride     = kTargetQn;
    sessionQualityContentKey   = currentContentKey();
    autoDowngradePending       = true;
    autoDowngradeFromQn        = fromQn;
    autoDowngradeToQn          = kTargetQn;
    autoDowngradeNewSession.clear();
    autoDowngradeOldSession    = wiliwili::d27::currentSession();
    uint64_t requestSeq        = ++autoDowngradeRequestSeq;

    char detail[256];
    snprintf(detail, sizeof(detail),
             "scene=auto_quality_downgrade reason=hard_stall from_qn=%d to_qn=%d "
             "old_session=%s status=sent",
             fromQn, kTargetQn, autoDowngradeOldSession.c_str());
    wiliwili::d27::log("CMD", detail);

    // 通知 compat：watchdog 恢复时不要再重放旧 qn 流，qn=64 新 open 即将到达。
    MPVCore::instance().command_async("set", "sl-health-auto-downgrade", "yes");
    this->updateVideoLink();

    ASYNC_RETAIN
    brls::delay(60000, [ASYNC_TOKEN, this, requestSeq]() {
        ASYNC_RELEASE
        if (autoDowngradePending && autoDowngradeRequestSeq == requestSeq)
            failAutoDowngrade("playback_timeout");
    });
    return true;
}

void BasePlayerActivity::completeAutoDowngradeIfPlaying() {
    if (!autoDowngradePending || !activityShown) return;
    if (videoUrlResult.quality != autoDowngradeToQn) return;
    if (!MPVCore::instance().isPlaying()) return;

    std::string session = wiliwili::d27::currentSession();
    if (!autoDowngradeNewSession.empty() && session != autoDowngradeNewSession) return;

    char detail[320];
    snprintf(detail, sizeof(detail),
             "scene=auto_quality_downgrade reason=hard_stall from_qn=%d to_qn=%d "
             "old_session=%s new_session=%s status=ok",
             autoDowngradeFromQn, autoDowngradeToQn,
             autoDowngradeOldSession.c_str(), session.c_str());
    wiliwili::d27::log("CMD", detail);
    autoDowngradePending = false;

    static const char* hint = "播放卡顿，已自动降低清晰度";
    wiliwili::d27::log("UI", "health hint shown reason=hard action=auto_downgrade");
    APP_E->fire(VideoView::HINT, (void*)hint);
}

void BasePlayerActivity::failAutoDowngrade(const char* reason) {
    if (!autoDowngradePending) return;
    std::string session = wiliwili::d27::currentSession();
    if (!autoDowngradeNewSession.empty() && session != autoDowngradeNewSession) return;

    char detail[256];
    snprintf(detail, sizeof(detail),
             "scene=auto_quality_downgrade reason=hard_stall from_qn=%d to_qn=%d "
             "old_session=%s status=error failure=%s",
             autoDowngradeFromQn, autoDowngradeToQn,
             autoDowngradeOldSession.c_str(), reason);
    wiliwili::d27::log("CMD", detail);
    autoDowngradePending = false;

    static const char* hint = "播放卡顿，已尝试自动恢复";
    wiliwili::d27::log("UI", "health hint shown reason=hard action=recovery_only");
    APP_E->fire(VideoView::HINT, (void*)hint);
}

void BasePlayerActivity::setVideoQuality() {
    if (this->videoUrlResult.accept_description.empty()) return;

    // getQualityIndex() 返回原始 accept_quality 序号；下拉列表已过滤 >116。
    const int originalSelected = getQualityIndex();
    int visibleSelected = 0;
    for (size_t i = 0; i < this->videoUrlResult.accept_quality.size(); i++) {
        if (this->videoUrlResult.accept_quality[i] > 116 && this->videoUrlResult.accept_quality[i] != 192) continue;
        if ((int)i == originalSelected) break;
        visibleSelected++;
    }
    size_t visibleCount = 0;
    for (int q : this->videoUrlResult.accept_quality) {
        if (q <= 116 || q == 192) visibleCount++;
    }
    if ((size_t)visibleSelected >= visibleCount) visibleSelected = 0;

    auto* dropdown = new BaseDropdown(
        "wiliwili/player/quality"_i18n,
        [this](int selected) {
            // selected 是过滤后的下拉序号，重新映射到原始 qn
            int code       = 116;
            size_t visible = 0;
            for (int q : this->videoUrlResult.accept_quality) {
                if (q > 116 && q != 192) continue;
                if (visible == (size_t)selected) { code = q; break; }
                visible++;
            }
            BasePlayerActivity::defaultQuality = code;
            ProgramConfig::instance().setSettingItem(SettingItem::VIDEO_QUALITY, code);

            // 如果未登录选择了大于等于480P清晰度的视频
            if (ProgramConfig::instance().getCSRF().empty() && defaultQuality >= 32) {
                DialogHelper::showDialog("wiliwili/home/common/no_login"_i18n);
                return;
            }

            sessionQualityOverride = 0;
            sessionQualityContentKey.clear();
            autoDowngradePending = false;
            this->updateVideoLink();
        },
        visibleSelected);
    auto* recycler = dropdown->getRecyclingList();
    recycler->registerCell("Cell", []() { return new QualityCell(); });
    dropdown->setDataSource(new QualityDataSource(this->videoUrlResult, dropdown));
    dropdown->registerAction(
        "", brls::ControllerButton::BUTTON_START,
        [dropdown](...) {
            dropdown->dismiss();
            return true;
        },
        true);
    dropdown->registerAction(ShortcutHelper::getVideoQuality(), [dropdown](...) {
        dropdown->dismiss();
        return true;
    });

    // 因为触摸的问题 视频组件上开启新的 activity 需要同步执行
    // 不然在某些情况下焦点会错乱
    ASYNC_RETAIN
    brls::sync([ASYNC_TOKEN, dropdown]() {
        ASYNC_RELEASE
        brls::Application::pushActivity(new brls::Activity(dropdown));
    });
}

bool BasePlayerActivity::setQualityCodeForDebug(int qn) {
    if (qn <= 0 || this->videoUrlResult.accept_quality.empty()) return false;

    bool supported = false;
    for (int code : this->videoUrlResult.accept_quality) {
        if (code > 116 && code != 192) continue;
        if (code == qn) {
            supported = true;
            break;
        }
    }
    if (!supported) return false;

    BasePlayerActivity::defaultQuality = qn;
    ProgramConfig::instance().setSettingItem(SettingItem::VIDEO_QUALITY, qn);
    if (ProgramConfig::instance().getCSRF().empty() && defaultQuality >= 32)
        return false;

    sessionQualityOverride = 0;
    sessionQualityContentKey.clear();
    autoDowngradePending = false;
    this->updateVideoLink();
    return true;
}

int BasePlayerActivity::getQualityCodeForDebug() const {
    return this->videoUrlResult.quality;
}

void BasePlayerActivity::setCommentMode() {
    this->recyclingGrid->estimatedRowHeight = 100;
    this->recyclingGrid->showSkeleton();
    tabFrame->focusTab(0);
    requestVideoComment(std::to_string(this->getAid()), 0, getVideoCommentMode() == 3 ? 2 : 3);
}

void BasePlayerActivity::onVideoPlayUrl(const bilibili::VideoUrlResult& result) {
    brls::Logger::debug("onVideoPlayUrl quality: {}", result.quality);

    // 新内容或用户手动选清晰度时清掉上一次的自动降级覆盖。
    std::string contentKey = this->currentContentKey();
    if (!sessionQualityContentKey.empty() && sessionQualityContentKey != contentKey)
        this->resetSessionQualityOverride();

    if (result.accept_quality.empty() || result.accept_description.empty()) {
        // 通常是返回了其他报错信息, 比如验证码
        brls::Logger::error("onVideoPlayUrl: no video url available");
        auto dialog = new brls::Dialog("Error: No video url available");
        dialog->setCancelable(false);
        dialog->addButton("hints/ok"_i18n, []() {
            brls::sync([]() { brls::Application::popActivity(); });
        });
        dialog->open();
        return;
    }

    // 有效期 110 分钟
    videoDeadline = std::chrono::system_clock::now() + std::chrono::seconds(6600);

    // 获取预设的跳转位置；控制目标保留亚秒，展示进度仍用整数秒。
    double start = controlTargetMs >= 0
                       ? (double)controlTargetMs / 1000.0
                       : (double)this->getProgress();
    if (controlTargetMs < 0) {
        controlTargetMs = (int64_t)(start * 1000.0 + 0.5);
        this->video->setControlTargetMs(controlTargetMs);
    }
    int end      = -1;
    int clipOpen = result.clipOpen, clipEnd = result.clipEnd;
    int time_sec = result.timelength / 1000;
    std::string customAspect;

    // 加载覆盖设置
    if (seasonInfo.season_id > 0) {
        auto seasonSetting = ProgramConfig::instance().getSeasonCustom(seasonInfo.season_id);
        customAspect       = seasonSetting.player_aspect;

        if (seasonSetting.custom_clip) {
            // 设置了自定义的片头片尾
            // 判断是否是正片
            for (auto& e : seasonInfo.episodes) {
                if (episodeResult.id == e.id) {
                    // 如果当前播放的是正片，则使用自定义的片头片尾
                    clipOpen = seasonSetting.clip_start;
                    if (clipOpen > time_sec - 5) clipOpen = 0;
                    clipEnd = time_sec - seasonSetting.clip_end;
                    if (seasonSetting.clip_end <= 0 || clipEnd < clipOpen) clipEnd = 0;
                    break;
                }
            }
        }
    }
    if (customAspect.empty()) {
        customAspect = ProgramConfig::instance().getSettingItem(SettingItem::PLAYER_ASPECT, std::string{"-1"});
    }
    MPVCore::instance().setAspect(customAspect);

    // 当要跳转的进度距离尾部只有 5s，就重新播放
    if (start > 0 && std::fabs((double)time_sec - start) <= 5.0) start = 0;

    // 跳过片头片尾
    if (PLAYER_SKIP_OPENING_CREDITS) {
        start = std::max(start, (double)clipOpen);
        if (clipEnd > 0) start = std::min(start, (double)clipEnd);
        end = clipEnd;
        brls::Logger::debug("片头片尾: {}/{}", clipOpen, clipEnd);
    }

    // 针对用户上传的视频，尝试加载上一次播放的进度
    if (videoDetailPage.cid && start < 0) {
        auto data = SubtitleCore::instance().getSubtitleList();
        if (data.last_play_cid == videoDetailPage.cid && data.last_play_time > 0) {
            APP_E->fire(VideoView::LAST_TIME, (void*)&(data.last_play_time));
        }
    } else {
        // 设置为 POSITION_DISCARD 后，不会加载网络历史记录，而是直接使用 setProgress 指定的位置
        // 番剧视频或指定了进度的用户视频
        int64_t position = 0;
        APP_E->fire(VideoView::LAST_TIME, (void*)&(position));
        if (start > 0) {
            std::string hint = fmt::format("已为您定位至: {}", wiliwili::sec2Time((int64_t)start));
            APP_E->fire(VideoView::HINT, (void*)hint.c_str());
        }
    }

    controlTargetMs = start > 0 ? (int64_t)(start * 1000.0 + 0.5) : 0;
    this->video->setControlTargetMs(controlTargetMs);

    // D27: 在真正产生 loadfile 前，由 UI 侧设置权威内容标识。
    {
        std::string content = "video";
        std::string bvid =
            videoDetailResult.bvid.empty() ? episodeResult.bvid : videoDetailResult.bvid;
        std::string id = bvid.empty() ? "unknown" : bvid;
        int width = 0, height = 0;
        std::string fps;
        for (const auto& media : result.dash.video) {
            if (media.id != result.quality) continue;
            width  = media.width;
            height = media.height;
            fps    = media.frameRate;
            break;
        }
        if (width == 0 && !result.dash.video.empty()) {
            width  = result.dash.video.front().width;
            height = result.dash.video.front().height;
            fps    = result.dash.video.front().frameRate;
        }
        std::string detail = fmt::format("cid={} aid={} qn={} width={} height={} fps={} start={}",
                                         videoDetailPage.cid, videoDetailResult.aid,
                                         result.quality, width, height,
                                         fps.empty() ? "-" : fps, start);
        if (seasonInfo.season_id > 0) {
            content = "season";
            id = episodeResult.id > 0 ? std::to_string(episodeResult.id)
                                      : std::to_string(seasonInfo.season_id);
            detail = fmt::format("season_id={} ep_id={} cid={} aid={} qn={} width={} height={} fps={} start={}",
                                 seasonInfo.season_id, episodeResult.id, episodeResult.cid,
                                 episodeResult.aid, result.quality, width, height,
                                 fps.empty() ? "-" : fps, start);
        }
        this->video->setDebugSession(content, id, detail, bvid,
                                     std::to_string(seasonInfo.season_id > 0
                                                        ? episodeResult.cid
                                                        : videoDetailPage.cid),
                                     std::to_string(result.quality));

        if (autoDowngradePending)
            autoDowngradeNewSession = wiliwili::d27::currentSession();
    }

    if (!result.dash.video.empty()) {
        // dash
        brls::Logger::debug("Video type: dash");

        // 找到当前可用的清晰度
        for (const auto& i : result.dash.video) {
            int desiredQuality = result.quality;
            // 192 是旧 MP4 支线的 720P 内部编号，不属于 >116 的高档位
            if (desiredQuality != 192) {
                if (i.height > i.width) {
                    desiredQuality = std::min(desiredQuality, portraitQualityMax);
                } else {
                    desiredQuality = std::min(desiredQuality, landscapeQualityMax);
                }
            }

            if (desiredQuality >= i.id) {
                videoUrlResult.quality = i.id;
                break;
            }
        }

        // 找到当前清晰度下可用的视频
        std::vector<bilibili::DashMedia> codecs;
        for (const auto& i : result.dash.video) {
            if (i.id == videoUrlResult.quality) {
                codecs.emplace_back(i);
            }
        }

        // 匹配当前设定的视频编码
        bilibili::DashMedia v = codecs[0];  // 默认是 AVC/H.264
        for (const auto& i : codecs) {
            if (BILI::VIDEO_CODEC == i.codecid) {
                v = i;
                break;
            }
        }

        // 将主音频和备份音频链接合并，当作不同的音轨传给播放器，可以实现在播放失败时自动切换
        std::vector<std::string> audios;
        if (!result.dash.audio.empty()) {
            // 选择音轨，支持杜比/无损优先和多级回退
            auto pickDolby = [&]() -> std::optional<bilibili::DashMedia> {
                if (result.dash.dolby_audio.empty()) return std::nullopt;
                bilibili::DashMedia best = result.dash.dolby_audio[0];
                for (auto& dm : result.dash.dolby_audio) if (dm.bandwidth > best.bandwidth) best = dm;
                return best;
            };
            auto pickFlac = [&]() -> std::optional<bilibili::DashMedia> {
                if (!result.dash.has_flac) return std::nullopt;
                return result.dash.flac_audio;
            };
            auto pickStandard = [&](int qid) -> std::optional<bilibili::DashMedia> {
                for (auto& i : result.dash.audio) if (i.id == qid) return i;
                return std::nullopt;
            };
            auto pickFirstAvailableStandard = [&]() -> std::optional<bilibili::DashMedia> {
                int candidates[] = {30280, 30232, 30216};
                for (int q : candidates) {
                    auto m = pickStandard(q);
                    if (m) return m;
                }
                // fallback to first item if none matched
                if (!result.dash.audio.empty()) return result.dash.audio[0];
                return std::nullopt;
            };

            bilibili::DashMedia a = result.dash.audio[0];
            bool selected = false;
            if (BILI::AUDIO_QUALITY == 30250) {
                // Dolby → Lossless → High → Medium → Low
                if (auto m = pickDolby()) { a = *m; selected = true; brls::Logger::debug("Picked Dolby audio (type {}), bw {}", result.dash.dolby_type, a.bandwidth);} else
                if (auto m = pickFlac()) { a = *m; selected = true; brls::Logger::debug("Picked FLAC audio, bw {}", a.bandwidth);} else
                if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
            } else if (BILI::AUDIO_QUALITY == 30251) {
                // Lossless → Dolby → High → Medium → Low
                if (auto m = pickFlac()) { a = *m; selected = true; brls::Logger::debug("Picked FLAC audio, bw {}", a.bandwidth);} else
                if (auto m = pickDolby()) { a = *m; selected = true; brls::Logger::debug("Picked Dolby audio (type {}), bw {}", result.dash.dolby_type, a.bandwidth);} else
                if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
            } else {
                // Try user-selected standard, then fallback High→Medium→Low
                if (auto m = pickStandard(BILI::AUDIO_QUALITY)) { a = *m; selected = true; }
                if (!selected) {
                    if (auto m = pickFirstAvailableStandard()) { a = *m; selected = true; }
                }
            }
            // 生成音频列表
            audios.emplace_back(a.base_url);
            audios.insert(audios.end(), a.backup_url.begin(), a.backup_url.end());
            brls::Logger::debug("Dash quality: {}; video: {}; audio: {}", videoUrlResult.quality, v.codecid, a.id);
        }

        // 给播放器设置链接
        this->video->setUrl(v.base_url, start, end, audios);

        // 设置备份视频链接
        for (const auto& backup_url : v.backup_url) {
            this->video->setBackupUrl(backup_url, start, end, audios);
        }
    } else {
        // flv
        brls::Logger::debug("Video type: flv");
        if (result.durl.empty()) {
            brls::Logger::error("No media");
        } else if (result.durl.size() == 1) {
            this->video->setUrl(result.durl[0].url, start, end);
        } else {
            std::vector<EDLUrl> urls;
            urls.reserve(result.durl.size());
            for (auto& i : result.durl) {
                urls.emplace_back(i.url, i.length / 1000.0f);
            }
            this->video->setUrl(urls, start, end);
        }
    }

    // 设置mpv事件
    // 1.更新清晰度
    std::string quality = videoUrlResult.accept_description[getQualityIndex()];
    APP_E->fire(VideoView::SET_QUALITY, (void*)quality.c_str());
    // 2.绘制进度条标记点（例如：片头片尾）
    if (clipOpen > 0) {
        float data = clipOpen * 1.0f / time_sec;
        APP_E->fire(VideoView::CLIP_INFO, (void*)&data);
    }
    if (clipEnd > 0) {
        float data2 = clipEnd * 1.0f / time_sec;
        APP_E->fire(VideoView::CLIP_INFO, (void*)&data2);
    }
    // 3. 设置视频时长
    APP_E->fire(VideoView::REAL_DURATION, (void*)&time_sec);

    brls::Logger::debug("BasePlayerActivity::onVideoPlayUrl done");

    // 根据配置决定是否自动全屏
    if (ProgramConfig::instance().getBoolOption(SettingItem::PLAYER_AUTO_FULLSCREEN) &&
        !this->video->isFullscreen()) {
        this->video->setFullScreen(true);
    }
}

void BasePlayerActivity::onCommentInfo(const bilibili::VideoCommentResultWrapper& result) {
    auto* datasource = dynamic_cast<DataSourceCommentList*>(recyclingGrid->getDataSource());
    if (!datasource && result.requestIndex == 0) {
        // 第一页评论
        //整合置顶评论
        std::vector<bilibili::VideoCommentResult> comments(result.top_replies);
        comments.insert(comments.end(), result.replies.begin(), result.replies.end());
        // 为了加载骨架屏美观，设置为了100，在加载评论时手动修改回来
        // 这里限制的是评论的最大高度，实际评论高度还受评论组件的最大行数限制
        this->recyclingGrid->estimatedRowHeight = 600;
        this->recyclingGrid->setDataSource(new DataSourceCommentList(
            comments, this->getAid(), this->getVideoCommentMode(), [this]() { this->setCommentMode(); }));
        this->recyclingGrid->selectRowAt(comments.empty() ? 1 : 2, false);
        // 设置评论数量提示
        auto item = this->tabFrame->getTab("wiliwili/player/comment"_i18n);
        if (item) item->setSubtitle(wiliwili::num2w(result.cursor.all_count));
    } else if (datasource) {
        // 第N页评论
        if (!result.replies.empty()) {
            datasource->appendData(result.replies);
            recyclingGrid->notifyDataChanged();
        }
    } else {
        brls::Logger::error("onCommentInfo ds: {} index: {} end: {}", (bool)datasource, result.requestIndex,
                            result.cursor.is_end);
    }
}

void BasePlayerActivity::onRequestCommentError(const std::string& error) {
    brls::sync([this, error]() { this->recyclingGrid->setError(error); });
}

void BasePlayerActivity::onVideoOnlineCount(const bilibili::VideoOnlineTotal& result) {
    std::string count = result.total + "wiliwili/player/current"_i18n;
    this->videoPeopleLabel->setText(count);
    APP_E->fire(VideoView::SET_ONLINE_NUM, (void*)count.c_str());
}

void BasePlayerActivity::onVideoRelationInfo(const bilibili::VideoRelation& result) {
    brls::Logger::debug("onVideoRelationInfo: {} {} {}", result.like, result.coin, result.favorite);
    this->setRelationButton(result.like, result.coin, result.favorite);
}

void BasePlayerActivity::onHighlightProgress(const bilibili::VideoHighlightProgress& result) {
    brls::Logger::debug("highlight: {}/{}", result.step_sec, result.data.size());
    VideoHighlightData data{result.step_sec, result.data};
    APP_E->fire(VideoView::HIGHLIGHT_INFO, (void*)&data);
}

void BasePlayerActivity::setRelationButton(bool liked, bool coin, bool favorite) {
    if (liked) {
        btnAgree->setImageFromSVGRes("svg/bpx-svg-sprite-liked-active.svg");
    } else {
        btnAgree->setImageFromSVGRes("svg/bpx-svg-sprite-liked.svg");
    }
    if (coin) {
        btnCoin->setImageFromSVGRes("svg/bpx-svg-sprite-coin-active.svg");
    } else {
        btnCoin->setImageFromSVGRes("svg/bpx-svg-sprite-coin.svg");
    }
    if (favorite) {
        btnFavorite->setImageFromSVGRes("svg/bpx-svg-sprite-collection-active.svg");
    } else {
        btnFavorite->setImageFromSVGRes("svg/bpx-svg-sprite-collection.svg");
    }
}

void BasePlayerActivity::onError(const std::string& error) {
    if (!activityShown) return;
    MPVCore::instance().stop();
    MPVCore::instance().reset();
    bool forceClose = true;
    std::string msg = error;
    if (pystring::count(error, "87007") > 0 || pystring::count(error, "87008") > 0) {
        forceClose = false;
        msg        = "该视频为「充电」专属视频";
    } else if (pystring::count(error, "10403") > 0) {
        forceClose = false;
        msg        = "大会员专享限制";
    } else if (pystring::count(error, "404") > 0) {
        msg = "啥都木有";
    } else if (pystring::count(error, "62002") > 0) {
        msg = "稿件不可见";
    }
    auto dialog = new brls::Dialog(msg);
    dialog->setCancelable(false);
    dialog->addButton("hints/ok"_i18n, [forceClose]() {
        if (forceClose) brls::sync([]() { brls::Application::popActivity(); });
    });
    dialog->open();
}

void BasePlayerActivity::willDisappear(bool resetState) {
    activityShown = false;
    brls::Activity::willDisappear(resetState);
}

void BasePlayerActivity::willAppear(bool resetState) {
    activityShown = true;
    brls::Activity::willAppear(resetState);
}

BasePlayerActivity::~BasePlayerActivity() {
    brls::Logger::debug("del BasePlayerActivity");
    // 取消监控mpv
    MPV_E->unsubscribe(eventSubscribeID);
    APP_E->unsubscribe(customEventSubscribeID);
    // 停止视频播放
    this->video->stop();
}
