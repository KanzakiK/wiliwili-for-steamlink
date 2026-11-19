//
// Created by fang on 2023/4/26.
//

#include <borealis/views/image.hpp>

#include "activity/gallery_activity.hpp"
#include "view/gallery_view.hpp"
#include "utils/image_helper.hpp"
#include "utils/debug_log.hpp"

const std::string ImageGalleryItemXML = R"xml(
    <brls:Box
        width="100%"
        height="100%"
        axis="row"
        grow="1"
        justifyContent="center"
        alignItems="center">

        <brls:Image
                margin="40"
                id="gallery/image"/>
    </brls:Box>
)xml";

class NetImageGalleryItem : public GalleryItem {
public:
    explicit NetImageGalleryItem(const std::string& url) {
        this->inflateFromXMLString(ImageGalleryItemXML);
        this->image->setImageFromRes("icon/bilibili_video.png");
        ImageHelper::with(this->image)->load(url);
    }
    ~NetImageGalleryItem() override { ImageHelper::clear(this->image); }

private:
    BRLS_BIND(brls::Image, image, "gallery/image");
};

GalleryActivity::GalleryActivity(const std::vector<std::string>& data) : galleryData(data) {
    brls::Logger::debug("GalleryActivity: create");
    std::string source;
    for (const auto& url : data) {
        auto end = url.find('?');
        auto slash = url.rfind('/', end == std::string::npos ? url.size() : end);
        if (slash != std::string::npos && slash + 1 < url.size())
            source = url.substr(slash + 1, (end == std::string::npos ? url.size() : end) - slash - 1);
        if (!source.empty()) break;
    }
    wiliwili::d27::reuseOrBeginSession(
        "gallery", source.empty() ? "gallery" : source,
        fmt::format("count={} source={}", data.size(), source.empty() ? "-" : source));
}

void GalleryActivity::onContentAvailable() {
    brls::Logger::debug("GalleryActivity: onContentAvailable");
    gallery->setIndicatorPosition(0.97);
    for (auto& i : galleryData) {
        gallery->addCustomView(new NetImageGalleryItem(i));
    }
}

GalleryActivity::~GalleryActivity() { brls::Logger::debug("GalleryActivity: delete"); }

bool GalleryActivity::isTranslucent() { return true; }
