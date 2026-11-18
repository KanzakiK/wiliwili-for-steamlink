/*
 * H.264 SPS 解析与改写（用于 Steam Link VPU 的参考帧门禁）
 *
 * B 站部分 360p/480p 码流会声明 refs=7 / max_dec_frame_buffering=7，
 * 超出 Marvell VPU 的参考帧池，喂给 SLVideo 约 50 帧后会把 MV_PE RPC 锁死。
 * 这里在创建 SLVideo stream 之前读取 SPS，必要时把 VUI/参考帧参数降到
 * refs=4 / reorder=3 / buffering=4（与能正常播放的老模板一致）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl {

struct H264SpsInfo {
    bool valid = false;
    int refs = -1;
    int reorder = -1;
    int buffering = -1;
};

/* nal 含 1 字节 NAL header */
H264SpsInfo parseSpsNal(const uint8_t* nal, size_t size);
H264SpsInfo parseAvcC(const uint8_t* extradata, size_t size);

/* 只降不升；返回 false 表示解析/改写失败 */
bool rewriteSpsNal(std::vector<uint8_t>& nal, int refs, int reorder, int buffering);

/* 改写 avcC 里的 SPS；changed 表示是否真的修改了数据 */
bool patchAvcC(std::vector<uint8_t>& extradata, int refs, int reorder, int buffering,
               bool* changed);

/* 检查 Annex B 帧里是否含 SPS（关键帧才会带） */
bool annexBContainsSps(const uint8_t* data, size_t size);

/* 取 Annex B 里第一个 SPS 的参数（诊断用） */
H264SpsInfo parseFirstSpsAnnexB(const uint8_t* data, size_t size);

/* 改写 Annex B 帧里的所有 SPS，patched 返回改写的个数 */
bool patchAnnexBSps(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                    int refs, int reorder, int buffering, int* patched);

} // namespace sl
