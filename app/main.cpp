/*
 * Steam Link 原生 B 站客户端 — 热门页 + 视频详情
 *
 * API 来自 wiliwili；UI 使用 borealis。播放器尚未嵌入。
 */

#include <borealis.hpp>

#include <bilibili.h>
#include <bilibili/result/home_hots_all_result.h>
#include <bilibili/result/video_detail_result.h>
#include <utils/number_helper.hpp>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

static brls::Label* g_status = nullptr;
static brls::Box* g_list     = nullptr;

class DetailActivity : public brls::Activity {
  public:
    explicit DetailActivity(std::string bvid)
        : bvid(std::move(bvid)), alive(std::make_shared<std::atomic<bool>>(true))
    {
    }

    brls::View* createContentView() override
    {
        brls::Box* box = new brls::Box(brls::Axis::COLUMN);
        box->setPadding(32, 40, 32, 40);

        backLabel = new brls::Label();
        backLabel->setText("← 返回");
        backLabel->setFontSize(20.0f);
        backLabel->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        backLabel->setMarginBottom(12.0f);
        backLabel->setFocusable(true);
        backLabel->registerAction("返回", brls::BUTTON_A, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        backLabel->registerAction("返回", brls::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        box->addView(backLabel);

        titleLabel = new brls::Label();
        titleLabel->setText("加载中…");
        titleLabel->setFontSize(30.0f);
        titleLabel->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        titleLabel->setMarginBottom(10.0f);
        titleLabel->setFocusable(false);
        box->addView(titleLabel);

        metaLabel = new brls::Label();
        metaLabel->setFontSize(18.0f);
        metaLabel->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        metaLabel->setMarginBottom(16.0f);
        metaLabel->setFocusable(false);
        box->addView(metaLabel);

        descLabel = new brls::Label();
        descLabel->setFontSize(18.0f);
        descLabel->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        descLabel->setWidth(1100.0f);
        descLabel->setFocusable(false);
        box->addView(descLabel);

        return box;
    }

    void onContentAvailable() override
    {
        load();
    }

    ~DetailActivity() override
    {
        *alive = false;
    }

  private:
    void load()
    {
        auto keep = alive;
        std::thread([this, keep]() {
            bilibili::BilibiliClient::get_video_detail(
                bvid,
                [this, keep](bilibili::VideoDetailResult result) {
                    brls::sync([this, keep, result]() {
                        if (!*keep)
                            return;
                        titleLabel->setText(result.title);
                        metaLabel->setText("UP：" + result.owner.name + "    " +
                                           std::to_string(result.stat.view) + " 播放    " +
                                           wiliwili::sec2Time(result.duration));
                        descLabel->setText(result.desc);
                    });
                },
                [this, keep](const std::string& message, int code) {
                    brls::sync([this, keep, message, code]() {
                        if (!*keep)
                            return;
                        titleLabel->setText("详情加载失败 " + std::to_string(code));
                        metaLabel->setText(message);
                    });
                });
        }).detach();
    }

    std::string bvid;
    std::shared_ptr<std::atomic<bool>> alive;
    brls::Label* titleLabel = nullptr;
    brls::Label* metaLabel  = nullptr;
    brls::Label* descLabel  = nullptr;
    brls::Label* backLabel  = nullptr;
};

class VideoRow : public brls::Box {
  public:
    VideoRow(const std::string& title, const std::string& meta, std::string bvid)
        : brls::Box(brls::Axis::COLUMN), bvid(std::move(bvid))
    {
        setPadding(10, 14, 10, 14);
        setMarginBottom(6);
        setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        setFocusable(true);

        brls::Label* title_label = new brls::Label();
        title_label->setText(title);
        title_label->setFontSize(21.0f);
        title_label->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        title_label->setFocusable(false);
        addView(title_label);

        brls::Label* meta_label = new brls::Label();
        meta_label->setText(meta);
        meta_label->setFontSize(14.0f);
        meta_label->setTextColor(nvgRGBA(170, 170, 170, 255));
        meta_label->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        meta_label->setFocusable(false);
        addView(meta_label);

        registerAction("详情", brls::BUTTON_A, [this](brls::View*) {
            brls::Application::pushActivity(new DetailActivity(this->bvid));
            return true;
        });
    }

    void onFocusGained() override
    {
        setBackgroundColor(nvgRGBA(0, 170, 255, 80));
        brls::Box::onFocusGained();
    }

    void onFocusLost() override
    {
        setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        brls::Box::onFocusLost();
    }

  private:
    std::string bvid;
};

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("Bilibili Steam Link");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(32, 40, 32, 40);

    brls::Label* title = new brls::Label();
    title->setText("热门");
    title->setFontSize(36.0f);
    title->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    title->setMarginBottom(8.0f);
    root->addView(title);

    g_status = new brls::Label();
    g_status->setText("正在加载 wiliwili 热门列表…");
    g_status->setFontSize(20.0f);
    g_status->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    g_status->setMarginBottom(12.0f);
    root->addView(g_status);

    brls::ScrollingFrame* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    g_list = new brls::Box(brls::Axis::COLUMN);
    scroll->setContentView(g_list);
    root->addView(scroll);

    brls::Application::pushActivity(new brls::Activity(root));

    bool api_started = false;
    while (brls::Application::mainLoop()) {
        if (api_started)
            continue;
        api_started = true;

        /* API 调用可能包含同步的 WBI key 请求，不能阻塞 UI 首帧 */
        std::thread([]() {
            bilibili::BilibiliClient::get_hots_all(
                1, 20,
                [](bilibili::HotsAllVideoListResult list, bool no_more) {
                    (void)no_more;
                    brls::sync([list]() {
                        if (!g_status || !g_list)
                            return;
                        g_status->setText("共 " + std::to_string(list.size()) + " 条（B 站热门 API）");
                        g_list->clearViews(true);
                        brls::View* first_row = nullptr;
                        for (const auto& video : list) {
                            std::string meta = video.owner.name + "  ·  " +
                                               wiliwili::sec2Time(video.duration);
                            VideoRow* row = new VideoRow(video.title, meta, video.bvid);
                            if (!first_row)
                                first_row = row;
                            g_list->addView(row);
                        }
                        if (first_row)
                            brls::Application::giveFocus(first_row);
                    });
                },
                [](const std::string& message, int code) {
                    brls::sync([message, code]() {
                        if (!g_status)
                            return;
                        g_status->setText("API 错误 " + std::to_string(code) + ": " + message);
                    });
                });
        }).detach();
    }

    return EXIT_SUCCESS;
}
