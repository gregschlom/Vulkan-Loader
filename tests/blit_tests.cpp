// VK tests
//
// Copyright (C) 2014 LunarG, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// Blit (copy, clear, and resolve) tests

#include "test_common.h"
#include "vktestbinding.h"
#include "test_environment.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

namespace vk_testing {

size_t get_format_size(VK_FORMAT format);

class ImageChecker {
public:
    explicit ImageChecker(const VK_IMAGE_CREATE_INFO &info, const std::vector<VK_BUFFER_IMAGE_COPY> &regions)
        : info_(info), regions_(regions), pattern_(HASH) {}
    explicit ImageChecker(const VK_IMAGE_CREATE_INFO &info, const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges);
    explicit ImageChecker(const VK_IMAGE_CREATE_INFO &info);

    void set_solid_pattern(const std::vector<uint8_t> &solid);

    VK_GPU_SIZE buffer_size() const;
    bool fill(Buffer &buf) const { return walk(FILL, buf); }
    bool fill(Image &img) const { return walk(FILL, img); }
    bool check(Buffer &buf) const { return walk(CHECK, buf); }
    bool check(Image &img) const { return walk(CHECK, img); }

    const std::vector<VK_BUFFER_IMAGE_COPY> &regions() const { return regions_; }

    static void hash_salt_generate() { hash_salt_++; }

private:
    enum Action {
        FILL,
        CHECK,
    };

    enum Pattern {
        HASH,
        SOLID,
    };

    size_t buffer_cpp() const;
    VK_SUBRESOURCE_LAYOUT buffer_layout(const VK_BUFFER_IMAGE_COPY &region) const;

    bool walk(Action action, Buffer &buf) const;
    bool walk(Action action, Image &img) const;
    bool walk_region(Action action, const VK_BUFFER_IMAGE_COPY &region, const VK_SUBRESOURCE_LAYOUT &layout, void *data) const;

    std::vector<uint8_t> pattern_hash(const VK_IMAGE_SUBRESOURCE &subres, const VK_OFFSET3D &offset) const;

    static uint32_t hash_salt_;

    VK_IMAGE_CREATE_INFO info_;
    std::vector<VK_BUFFER_IMAGE_COPY> regions_;

    Pattern pattern_;
    std::vector<uint8_t> pattern_solid_;
};


uint32_t ImageChecker::hash_salt_;

ImageChecker::ImageChecker(const VK_IMAGE_CREATE_INFO &info)
    : info_(info), regions_(), pattern_(HASH)
{
    // create a region for every mip level in array slice 0
    VK_GPU_SIZE offset = 0;
    for (uint32_t lv = 0; lv < info_.mipLevels; lv++) {
        VK_BUFFER_IMAGE_COPY region = {};

        region.bufferOffset = offset;
        region.imageSubresource.mipLevel = lv;
        region.imageSubresource.arraySlice = 0;
        region.imageExtent = Image::extent(info_.extent, lv);

        if (info_.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
            if (info_.format != VK_FMT_S8_UINT) {
                region.imageSubresource.aspect = VK_IMAGE_ASPECT_DEPTH;
                regions_.push_back(region);
            }

            if (info_.format == VK_FMT_D16_UNORM_S8_UINT ||
                info_.format == VK_FMT_D32_SFLOAT_S8_UINT ||
                info_.format == VK_FMT_S8_UINT) {
                region.imageSubresource.aspect = VK_IMAGE_ASPECT_STENCIL;
                regions_.push_back(region);
            }
        } else {
            region.imageSubresource.aspect = VK_IMAGE_ASPECT_COLOR;
            regions_.push_back(region);
        }

        offset += buffer_layout(region).size;
    }

    // arraySize should be limited in our tests.  If this proves to be an
    // issue, we can store only the regions for array slice 0 and be smart.
    if (info_.arraySize > 1) {
        const VK_GPU_SIZE slice_pitch = offset;
        const uint32_t slice_region_count = regions_.size();

        regions_.reserve(slice_region_count * info_.arraySize);

        for (uint32_t slice = 1; slice < info_.arraySize; slice++) {
            for (uint32_t i = 0; i < slice_region_count; i++) {
                VK_BUFFER_IMAGE_COPY region = regions_[i];

                region.bufferOffset += slice_pitch * slice;
                region.imageSubresource.arraySlice = slice;
                regions_.push_back(region);
            }
        }
    }
}

ImageChecker::ImageChecker(const VK_IMAGE_CREATE_INFO &info, const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges)
    : info_(info), regions_(), pattern_(HASH)
{
    VK_GPU_SIZE offset = 0;
    for (std::vector<VK_IMAGE_SUBRESOURCE_RANGE>::const_iterator it = ranges.begin();
         it != ranges.end(); it++) {
        for (uint32_t lv = 0; lv < it->mipLevels; lv++) {
            for (uint32_t slice = 0; slice < it->arraySize; slice++) {
                VK_BUFFER_IMAGE_COPY region = {};
                region.bufferOffset = offset;
                region.imageSubresource = Image::subresource(*it, lv, slice);
                region.imageExtent = Image::extent(info_.extent, lv);

                regions_.push_back(region);

                offset += buffer_layout(region).size;
            }
        }
    }
}

void ImageChecker::set_solid_pattern(const std::vector<uint8_t> &solid)
{
    pattern_ = SOLID;
    pattern_solid_.clear();
    pattern_solid_.reserve(buffer_cpp());
    for (int i = 0; i < buffer_cpp(); i++)
        pattern_solid_.push_back(solid[i % solid.size()]);
}

size_t ImageChecker::buffer_cpp() const
{
    return get_format_size(info_.format);
}

VK_SUBRESOURCE_LAYOUT ImageChecker::buffer_layout(const VK_BUFFER_IMAGE_COPY &region) const
{
    VK_SUBRESOURCE_LAYOUT layout = {};
    layout.offset = region.bufferOffset;
    layout.rowPitch = buffer_cpp() * region.imageExtent.width;
    layout.depthPitch = layout.rowPitch * region.imageExtent.height;
    layout.size = layout.depthPitch * region.imageExtent.depth;

    return layout;
}

VK_GPU_SIZE ImageChecker::buffer_size() const
{
    VK_GPU_SIZE size = 0;

    for (std::vector<VK_BUFFER_IMAGE_COPY>::const_iterator it = regions_.begin();
         it != regions_.end(); it++) {
        const VK_SUBRESOURCE_LAYOUT layout = buffer_layout(*it);
        if (size < layout.offset + layout.size)
            size = layout.offset + layout.size;
    }

    return size;
}

bool ImageChecker::walk_region(Action action, const VK_BUFFER_IMAGE_COPY &region,
                               const VK_SUBRESOURCE_LAYOUT &layout, void *data) const
{
    for (int32_t z = 0; z < region.imageExtent.depth; z++) {
        for (int32_t y = 0; y < region.imageExtent.height; y++) {
            for (int32_t x = 0; x < region.imageExtent.width; x++) {
                uint8_t *dst = static_cast<uint8_t *>(data);
                dst += layout.offset + layout.depthPitch * z +
                    layout.rowPitch * y + buffer_cpp() * x;

                VK_OFFSET3D offset = region.imageOffset;
                offset.x += x;
                offset.y += y;
                offset.z += z;

                const std::vector<uint8_t> &val = (pattern_ == HASH) ?
                    pattern_hash(region.imageSubresource, offset) :
                    pattern_solid_;
                assert(val.size() == buffer_cpp());

                if (action == FILL) {
                    memcpy(dst, &val[0], val.size());
                } else {
                    for (int i = 0; i < val.size(); i++) {
                        EXPECT_EQ(val[i], dst[i]) <<
                            "Offset is: (" << x << ", " << y << ", " << z << ")";
                        if (val[i] != dst[i])
                            return false;
                    }
                }
            }
        }
    }

    return true;
}

bool ImageChecker::walk(Action action, Buffer &buf) const
{
    void *data = buf.map();
    if (!data)
        return false;

    std::vector<VK_BUFFER_IMAGE_COPY>::const_iterator it;
    for (it = regions_.begin(); it != regions_.end(); it++) {
        if (!walk_region(action, *it, buffer_layout(*it), data))
            break;
    }

    buf.unmap();

    return (it == regions_.end());
}

bool ImageChecker::walk(Action action, Image &img) const
{
    void *data = img.map();
    if (!data)
        return false;

    std::vector<VK_BUFFER_IMAGE_COPY>::const_iterator it;
    for (it = regions_.begin(); it != regions_.end(); it++) {
        if (!walk_region(action, *it, img.subresource_layout(it->imageSubresource), data))
            break;
    }

    img.unmap();

    return (it == regions_.end());
}

std::vector<uint8_t> ImageChecker::pattern_hash(const VK_IMAGE_SUBRESOURCE &subres, const VK_OFFSET3D &offset) const
{
#define HASH_BYTE(val, b) static_cast<uint8_t>((static_cast<uint32_t>(val) >> (b * 8)) & 0xff)
#define HASH_BYTES(val) HASH_BYTE(val, 0), HASH_BYTE(val, 1), HASH_BYTE(val, 2), HASH_BYTE(val, 3)
    const unsigned char input[] = {
        HASH_BYTES(hash_salt_),
        HASH_BYTES(subres.mipLevel),
        HASH_BYTES(subres.arraySlice),
        HASH_BYTES(offset.x),
        HASH_BYTES(offset.y),
        HASH_BYTES(offset.z),
    };
    unsigned long hash = 5381;

    for (int32_t i = 0; i < ARRAY_SIZE(input); i++)
        hash = ((hash << 5) + hash) + input[i];

    const uint8_t output[4] = { HASH_BYTES(hash) };
#undef HASH_BYTES
#undef HASH_BYTE

    std::vector<uint8_t> val;
    val.reserve(buffer_cpp());
    for (int i = 0; i < buffer_cpp(); i++)
        val.push_back(output[i % 4]);

    return val;
}

size_t get_format_size(VK_FORMAT format)
{
    static const struct format_info {
        size_t size;
        uint32_t channel_count;
    } format_table[VK_NUM_FMT] = {
        [VK_FMT_UNDEFINED]            = { 0,  0 },
        [VK_FMT_R4G4_UNORM]           = { 1,  2 },
        [VK_FMT_R4G4_USCALED]         = { 1,  2 },
        [VK_FMT_R4G4B4A4_UNORM]       = { 2,  4 },
        [VK_FMT_R4G4B4A4_USCALED]     = { 2,  4 },
        [VK_FMT_R5G6B5_UNORM]         = { 2,  3 },
        [VK_FMT_R5G6B5_USCALED]       = { 2,  3 },
        [VK_FMT_R5G5B5A1_UNORM]       = { 2,  4 },
        [VK_FMT_R5G5B5A1_USCALED]     = { 2,  4 },
        [VK_FMT_R8_UNORM]             = { 1,  1 },
        [VK_FMT_R8_SNORM]             = { 1,  1 },
        [VK_FMT_R8_USCALED]           = { 1,  1 },
        [VK_FMT_R8_SSCALED]           = { 1,  1 },
        [VK_FMT_R8_UINT]              = { 1,  1 },
        [VK_FMT_R8_SINT]              = { 1,  1 },
        [VK_FMT_R8_SRGB]              = { 1,  1 },
        [VK_FMT_R8G8_UNORM]           = { 2,  2 },
        [VK_FMT_R8G8_SNORM]           = { 2,  2 },
        [VK_FMT_R8G8_USCALED]         = { 2,  2 },
        [VK_FMT_R8G8_SSCALED]         = { 2,  2 },
        [VK_FMT_R8G8_UINT]            = { 2,  2 },
        [VK_FMT_R8G8_SINT]            = { 2,  2 },
        [VK_FMT_R8G8_SRGB]            = { 2,  2 },
        [VK_FMT_R8G8B8_UNORM]         = { 3,  3 },
        [VK_FMT_R8G8B8_SNORM]         = { 3,  3 },
        [VK_FMT_R8G8B8_USCALED]       = { 3,  3 },
        [VK_FMT_R8G8B8_SSCALED]       = { 3,  3 },
        [VK_FMT_R8G8B8_UINT]          = { 3,  3 },
        [VK_FMT_R8G8B8_SINT]          = { 3,  3 },
        [VK_FMT_R8G8B8_SRGB]          = { 3,  3 },
        [VK_FMT_R8G8B8A8_UNORM]       = { 4,  4 },
        [VK_FMT_R8G8B8A8_SNORM]       = { 4,  4 },
        [VK_FMT_R8G8B8A8_USCALED]     = { 4,  4 },
        [VK_FMT_R8G8B8A8_SSCALED]     = { 4,  4 },
        [VK_FMT_R8G8B8A8_UINT]        = { 4,  4 },
        [VK_FMT_R8G8B8A8_SINT]        = { 4,  4 },
        [VK_FMT_R8G8B8A8_SRGB]        = { 4,  4 },
        [VK_FMT_R10G10B10A2_UNORM]    = { 4,  4 },
        [VK_FMT_R10G10B10A2_SNORM]    = { 4,  4 },
        [VK_FMT_R10G10B10A2_USCALED]  = { 4,  4 },
        [VK_FMT_R10G10B10A2_SSCALED]  = { 4,  4 },
        [VK_FMT_R10G10B10A2_UINT]     = { 4,  4 },
        [VK_FMT_R10G10B10A2_SINT]     = { 4,  4 },
        [VK_FMT_R16_UNORM]            = { 2,  1 },
        [VK_FMT_R16_SNORM]            = { 2,  1 },
        [VK_FMT_R16_USCALED]          = { 2,  1 },
        [VK_FMT_R16_SSCALED]          = { 2,  1 },
        [VK_FMT_R16_UINT]             = { 2,  1 },
        [VK_FMT_R16_SINT]             = { 2,  1 },
        [VK_FMT_R16_SFLOAT]           = { 2,  1 },
        [VK_FMT_R16G16_UNORM]         = { 4,  2 },
        [VK_FMT_R16G16_SNORM]         = { 4,  2 },
        [VK_FMT_R16G16_USCALED]       = { 4,  2 },
        [VK_FMT_R16G16_SSCALED]       = { 4,  2 },
        [VK_FMT_R16G16_UINT]          = { 4,  2 },
        [VK_FMT_R16G16_SINT]          = { 4,  2 },
        [VK_FMT_R16G16_SFLOAT]        = { 4,  2 },
        [VK_FMT_R16G16B16_UNORM]      = { 6,  3 },
        [VK_FMT_R16G16B16_SNORM]      = { 6,  3 },
        [VK_FMT_R16G16B16_USCALED]    = { 6,  3 },
        [VK_FMT_R16G16B16_SSCALED]    = { 6,  3 },
        [VK_FMT_R16G16B16_UINT]       = { 6,  3 },
        [VK_FMT_R16G16B16_SINT]       = { 6,  3 },
        [VK_FMT_R16G16B16_SFLOAT]     = { 6,  3 },
        [VK_FMT_R16G16B16A16_UNORM]   = { 8,  4 },
        [VK_FMT_R16G16B16A16_SNORM]   = { 8,  4 },
        [VK_FMT_R16G16B16A16_USCALED] = { 8,  4 },
        [VK_FMT_R16G16B16A16_SSCALED] = { 8,  4 },
        [VK_FMT_R16G16B16A16_UINT]    = { 8,  4 },
        [VK_FMT_R16G16B16A16_SINT]    = { 8,  4 },
        [VK_FMT_R16G16B16A16_SFLOAT]  = { 8,  4 },
        [VK_FMT_R32_UINT]             = { 4,  1 },
        [VK_FMT_R32_SINT]             = { 4,  1 },
        [VK_FMT_R32_SFLOAT]           = { 4,  1 },
        [VK_FMT_R32G32_UINT]          = { 8,  2 },
        [VK_FMT_R32G32_SINT]          = { 8,  2 },
        [VK_FMT_R32G32_SFLOAT]        = { 8,  2 },
        [VK_FMT_R32G32B32_UINT]       = { 12, 3 },
        [VK_FMT_R32G32B32_SINT]       = { 12, 3 },
        [VK_FMT_R32G32B32_SFLOAT]     = { 12, 3 },
        [VK_FMT_R32G32B32A32_UINT]    = { 16, 4 },
        [VK_FMT_R32G32B32A32_SINT]    = { 16, 4 },
        [VK_FMT_R32G32B32A32_SFLOAT]  = { 16, 4 },
        [VK_FMT_R64_SFLOAT]           = { 8,  1 },
        [VK_FMT_R64G64_SFLOAT]        = { 16, 2 },
        [VK_FMT_R64G64B64_SFLOAT]     = { 24, 3 },
        [VK_FMT_R64G64B64A64_SFLOAT]  = { 32, 4 },
        [VK_FMT_R11G11B10_UFLOAT]     = { 4,  3 },
        [VK_FMT_R9G9B9E5_UFLOAT]      = { 4,  3 },
        [VK_FMT_D16_UNORM]            = { 2,  1 },
        [VK_FMT_D24_UNORM]            = { 3,  1 },
        [VK_FMT_D32_SFLOAT]           = { 4,  1 },
        [VK_FMT_S8_UINT]              = { 1,  1 },
        [VK_FMT_D16_UNORM_S8_UINT]    = { 3,  2 },
        [VK_FMT_D24_UNORM_S8_UINT]    = { 4,  2 },
        [VK_FMT_D32_SFLOAT_S8_UINT]   = { 4,  2 },
        [VK_FMT_BC1_RGB_UNORM]        = { 8,  4 },
        [VK_FMT_BC1_RGB_SRGB]         = { 8,  4 },
        [VK_FMT_BC1_RGBA_UNORM]       = { 8,  4 },
        [VK_FMT_BC1_RGBA_SRGB]        = { 8,  4 },
        [VK_FMT_BC2_UNORM]            = { 16, 4 },
        [VK_FMT_BC2_SRGB]             = { 16, 4 },
        [VK_FMT_BC3_UNORM]            = { 16, 4 },
        [VK_FMT_BC3_SRGB]             = { 16, 4 },
        [VK_FMT_BC4_UNORM]            = { 8,  4 },
        [VK_FMT_BC4_SNORM]            = { 8,  4 },
        [VK_FMT_BC5_UNORM]            = { 16, 4 },
        [VK_FMT_BC5_SNORM]            = { 16, 4 },
        [VK_FMT_BC6H_UFLOAT]          = { 16, 4 },
        [VK_FMT_BC6H_SFLOAT]          = { 16, 4 },
        [VK_FMT_BC7_UNORM]            = { 16, 4 },
        [VK_FMT_BC7_SRGB]             = { 16, 4 },
        // TODO: Initialize remaining compressed formats.
        [VK_FMT_ETC2_R8G8B8_UNORM]    = { 0, 0 },
        [VK_FMT_ETC2_R8G8B8_SRGB]     = { 0, 0 },
        [VK_FMT_ETC2_R8G8B8A1_UNORM]  = { 0, 0 },
        [VK_FMT_ETC2_R8G8B8A1_SRGB]   = { 0, 0 },
        [VK_FMT_ETC2_R8G8B8A8_UNORM]  = { 0, 0 },
        [VK_FMT_ETC2_R8G8B8A8_SRGB]   = { 0, 0 },
        [VK_FMT_EAC_R11_UNORM]        = { 0, 0 },
        [VK_FMT_EAC_R11_SNORM]        = { 0, 0 },
        [VK_FMT_EAC_R11G11_UNORM]     = { 0, 0 },
        [VK_FMT_EAC_R11G11_SNORM]     = { 0, 0 },
        [VK_FMT_ASTC_4x4_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_4x4_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_5x4_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_5x4_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_5x5_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_5x5_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_6x5_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_6x5_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_6x6_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_6x6_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_8x5_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_8x5_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_8x6_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_8x6_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_8x8_UNORM]       = { 0, 0 },
        [VK_FMT_ASTC_8x8_SRGB]        = { 0, 0 },
        [VK_FMT_ASTC_10x5_UNORM]      = { 0, 0 },
        [VK_FMT_ASTC_10x5_SRGB]       = { 0, 0 },
        [VK_FMT_ASTC_10x6_UNORM]      = { 0, 0 },
        [VK_FMT_ASTC_10x6_SRGB]       = { 0, 0 },
        [VK_FMT_ASTC_10x8_UNORM]      = { 0, 0 },
        [VK_FMT_ASTC_10x8_SRGB]       = { 0, 0 },
        [VK_FMT_ASTC_10x10_UNORM]     = { 0, 0 },
        [VK_FMT_ASTC_10x10_SRGB]      = { 0, 0 },
        [VK_FMT_ASTC_12x10_UNORM]     = { 0, 0 },
        [VK_FMT_ASTC_12x10_SRGB]      = { 0, 0 },
        [VK_FMT_ASTC_12x12_UNORM]     = { 0, 0 },
        [VK_FMT_ASTC_12x12_SRGB]      = { 0, 0 },
        [VK_FMT_B4G4R4A4_UNORM]       = { 2, 4 },
        [VK_FMT_B5G5R5A1_UNORM]       = { 2, 4 },
        [VK_FMT_B5G6R5_UNORM]         = { 2, 3 },
        [VK_FMT_B5G6R5_USCALED]       = { 2, 3 },
        [VK_FMT_B8G8R8_UNORM]         = { 3, 3 },
        [VK_FMT_B8G8R8_SNORM]         = { 3, 3 },
        [VK_FMT_B8G8R8_USCALED]       = { 3, 3 },
        [VK_FMT_B8G8R8_SSCALED]       = { 3, 3 },
        [VK_FMT_B8G8R8_UINT]          = { 3, 3 },
        [VK_FMT_B8G8R8_SINT]          = { 3, 3 },
        [VK_FMT_B8G8R8_SRGB]          = { 3, 3 },
        [VK_FMT_B8G8R8A8_UNORM]       = { 4, 4 },
        [VK_FMT_B8G8R8A8_SNORM]       = { 4, 4 },
        [VK_FMT_B8G8R8A8_USCALED]     = { 4, 4 },
        [VK_FMT_B8G8R8A8_SSCALED]     = { 4, 4 },
        [VK_FMT_B8G8R8A8_UINT]        = { 4, 4 },
        [VK_FMT_B8G8R8A8_SINT]        = { 4, 4 },
        [VK_FMT_B8G8R8A8_SRGB]        = { 4, 4 },
        [VK_FMT_B10G10R10A2_UNORM]    = { 4, 4 },
        [VK_FMT_B10G10R10A2_SNORM]    = { 4, 4 },
        [VK_FMT_B10G10R10A2_USCALED]  = { 4, 4 },
        [VK_FMT_B10G10R10A2_SSCALED]  = { 4, 4 },
        [VK_FMT_B10G10R10A2_UINT]     = { 4, 4 },
        [VK_FMT_B10G10R10A2_SINT]     = { 4, 4 },
    };

    return format_table[format].size;
}

VK_EXTENT3D get_mip_level_extent(const VK_EXTENT3D &extent, uint32_t mip_level)
{
    const VK_EXTENT3D ext = {
        (extent.width  >> mip_level) ? extent.width  >> mip_level : 1,
        (extent.height >> mip_level) ? extent.height >> mip_level : 1,
        (extent.depth  >> mip_level) ? extent.depth  >> mip_level : 1,
    };

    return ext;
}

}; // namespace vk_testing

namespace {

#define DO(action) ASSERT_EQ(true, action);

static vk_testing::Environment *environment;

class XglCmdBlitTest : public ::testing::Test {
protected:
    XglCmdBlitTest() :
        dev_(environment->default_device()),
        queue_(*dev_.graphics_queues()[0]),
        cmd_(dev_, vk_testing::CmdBuffer::create_info(dev_.graphics_queue_node_index_))
    {
        // make sure every test uses a different pattern
        vk_testing::ImageChecker::hash_salt_generate();
    }

    bool submit_and_done()
    {
        queue_.submit(cmd_);
        queue_.wait();
        mem_refs_.clear();
        return true;
    }

    void add_memory_ref(const vk_testing::Object &obj)
    {
        const std::vector<VK_GPU_MEMORY> mems = obj.memories();
        for (std::vector<VK_GPU_MEMORY>::const_iterator it = mems.begin(); it != mems.end(); it++) {
            std::vector<VK_GPU_MEMORY>::iterator ref;
            for (ref = mem_refs_.begin(); ref != mem_refs_.end(); ref++) {
                if (*ref == *it)
                    break;
            }

            if (ref == mem_refs_.end()) {
                mem_refs_.push_back(*it);
            }
        }
    }

    vk_testing::Device &dev_;
    vk_testing::Queue &queue_;
    vk_testing::CmdBuffer cmd_;

    std::vector<VK_GPU_MEMORY> mem_refs_;
};

typedef XglCmdBlitTest XglCmdFillBufferTest;

TEST_F(XglCmdFillBufferTest, Basic)
{
    vk_testing::Buffer buf;

    buf.init(dev_, 20);
    add_memory_ref(buf);

    cmd_.begin();
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), 0, 4, 0x11111111);
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), 4, 16, 0x22222222);
    cmd_.end();

    submit_and_done();

    const uint32_t *data = static_cast<const uint32_t *>(buf.map());
    EXPECT_EQ(0x11111111, data[0]);
    EXPECT_EQ(0x22222222, data[1]);
    EXPECT_EQ(0x22222222, data[2]);
    EXPECT_EQ(0x22222222, data[3]);
    EXPECT_EQ(0x22222222, data[4]);
    buf.unmap();
}

TEST_F(XglCmdFillBufferTest, Large)
{
    const VK_GPU_SIZE size = 32 * 1024 * 1024;
    vk_testing::Buffer buf;

    buf.init(dev_, size);
    add_memory_ref(buf);

    cmd_.begin();
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), 0, size / 2, 0x11111111);
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), size / 2, size / 2, 0x22222222);
    cmd_.end();

    submit_and_done();

    const uint32_t *data = static_cast<const uint32_t *>(buf.map());
    VK_GPU_SIZE offset;
    for (offset = 0; offset < size / 2; offset += 4)
        EXPECT_EQ(0x11111111, data[offset / 4]) << "Offset is: " << offset;
    for (; offset < size; offset += 4)
        EXPECT_EQ(0x22222222, data[offset / 4]) << "Offset is: " << offset;
    buf.unmap();
}

TEST_F(XglCmdFillBufferTest, Overlap)
{
    vk_testing::Buffer buf;

    buf.init(dev_, 64);
    add_memory_ref(buf);

    cmd_.begin();
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), 0, 48, 0x11111111);
    vkCmdFillBuffer(cmd_.obj(), buf.obj(), 32, 32, 0x22222222);
    cmd_.end();

    submit_and_done();

    const uint32_t *data = static_cast<const uint32_t *>(buf.map());
    VK_GPU_SIZE offset;
    for (offset = 0; offset < 32; offset += 4)
        EXPECT_EQ(0x11111111, data[offset / 4]) << "Offset is: " << offset;
    for (; offset < 64; offset += 4)
        EXPECT_EQ(0x22222222, data[offset / 4]) << "Offset is: " << offset;
    buf.unmap();
}

TEST_F(XglCmdFillBufferTest, MultiAlignments)
{
    vk_testing::Buffer bufs[9];
    VK_GPU_SIZE size = 4;

    cmd_.begin();
    for (int i = 0; i < ARRAY_SIZE(bufs); i++) {
        bufs[i].init(dev_, size);
        add_memory_ref(bufs[i]);
        vkCmdFillBuffer(cmd_.obj(), bufs[i].obj(), 0, size, 0x11111111);
        size <<= 1;
    }
    cmd_.end();

    submit_and_done();

    size = 4;
    for (int i = 0; i < ARRAY_SIZE(bufs); i++) {
        const uint32_t *data = static_cast<const uint32_t *>(bufs[i].map());
        VK_GPU_SIZE offset;
        for (offset = 0; offset < size; offset += 4)
            EXPECT_EQ(0x11111111, data[offset / 4]) << "Buffser is: " << i << "\n" <<
                                                       "Offset is: " << offset;
        bufs[i].unmap();

        size <<= 1;
    }
}

typedef XglCmdBlitTest XglCmdCopyBufferTest;

TEST_F(XglCmdCopyBufferTest, Basic)
{
    vk_testing::Buffer src, dst;

    src.init(dev_, 4);
    uint32_t *data = static_cast<uint32_t *>(src.map());
    data[0] = 0x11111111;
    src.unmap();
    add_memory_ref(src);

    dst.init(dev_, 4);
    add_memory_ref(dst);

    cmd_.begin();
    VK_BUFFER_COPY region = {};
    region.copySize = 4;
    vkCmdCopyBuffer(cmd_.obj(), src.obj(), dst.obj(), 1, &region);
    cmd_.end();

    submit_and_done();

    data = static_cast<uint32_t *>(dst.map());
    EXPECT_EQ(0x11111111, data[0]);
    dst.unmap();
}

TEST_F(XglCmdCopyBufferTest, Large)
{
    const VK_GPU_SIZE size = 32 * 1024 * 1024;
    vk_testing::Buffer src, dst;

    src.init(dev_, size);
    uint32_t *data = static_cast<uint32_t *>(src.map());
    VK_GPU_SIZE offset;
    for (offset = 0; offset < size; offset += 4)
        data[offset / 4] = offset;
    src.unmap();
    add_memory_ref(src);

    dst.init(dev_, size);
    add_memory_ref(dst);

    cmd_.begin();
    VK_BUFFER_COPY region = {};
    region.copySize = size;
    vkCmdCopyBuffer(cmd_.obj(), src.obj(), dst.obj(), 1, &region);
    cmd_.end();

    submit_and_done();

    data = static_cast<uint32_t *>(dst.map());
    for (offset = 0; offset < size; offset += 4)
        EXPECT_EQ(offset, data[offset / 4]);
    dst.unmap();
}

TEST_F(XglCmdCopyBufferTest, MultiAlignments)
{
    const VK_BUFFER_COPY regions[] = {
        /* well aligned */
        {  0,   0,  256 },
        {  0, 256,  128 },
        {  0, 384,   64 },
        {  0, 448,   32 },
        {  0, 480,   16 },
        {  0, 496,    8 },

        /* ill aligned */
        {  7, 510,   16 },
        { 16, 530,   13 },
        { 32, 551,   16 },
        { 45, 570,   15 },
        { 50, 590,    1 },
    };
    vk_testing::Buffer src, dst;

    src.init(dev_, 256);
    uint8_t *data = static_cast<uint8_t *>(src.map());
    for (int i = 0; i < 256; i++)
        data[i] = i;
    src.unmap();
    add_memory_ref(src);

    dst.init(dev_, 1024);
    add_memory_ref(dst);

    cmd_.begin();
    vkCmdCopyBuffer(cmd_.obj(), src.obj(), dst.obj(), ARRAY_SIZE(regions), regions);
    cmd_.end();

    submit_and_done();

    data = static_cast<uint8_t *>(dst.map());
    for (int i = 0; i < ARRAY_SIZE(regions); i++) {
        const VK_BUFFER_COPY &r = regions[i];

        for (int j = 0; j < r.copySize; j++) {
            EXPECT_EQ(r.srcOffset + j, data[r.destOffset + j]) <<
                "Region is: " << i << "\n" <<
                "Offset is: " << r.destOffset + j;
        }
    }
    dst.unmap();
}

TEST_F(XglCmdCopyBufferTest, RAWHazard)
{
    vk_testing::Buffer bufs[3];
    VK_EVENT_CREATE_INFO event_info;
    VK_EVENT event;
    VK_MEMORY_REQUIREMENTS mem_req;
    size_t data_size = sizeof(mem_req);
    VK_RESULT err;

    //        typedef struct _VK_EVENT_CREATE_INFO
    //        {
    //            VK_STRUCTURE_TYPE                      sType;      // Must be VK_STRUCTURE_TYPE_EVENT_CREATE_INFO
    //            const void*                             pNext;      // Pointer to next structure
    //            VK_FLAGS                               flags;      // Reserved
    //        } VK_EVENT_CREATE_INFO;
    memset(&event_info, 0, sizeof(event_info));
    event_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;

    err = vkCreateEvent(dev_.obj(), &event_info, &event);
    ASSERT_VK_SUCCESS(err);

    err = vkGetObjectInfo(event, VK_INFO_TYPE_MEMORY_REQUIREMENTS,
                           &data_size, &mem_req);
    ASSERT_VK_SUCCESS(err);

    //        VK_RESULT VKAPI vkAllocMemory(
    //            VK_DEVICE                                  device,
    //            const VK_MEMORY_ALLOC_INFO*                pAllocInfo,
    //            VK_GPU_MEMORY*                             pMem);
    VK_MEMORY_ALLOC_INFO mem_info;
    VK_GPU_MEMORY event_mem;

    ASSERT_NE(0, mem_req.size) << "vkGetObjectInfo (Event): Failed - expect events to require memory";

    memset(&mem_info, 0, sizeof(mem_info));
    mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
    mem_info.allocationSize = mem_req.size;
    mem_info.memType = mem_req.memType;
    mem_info.memPriority = VK_MEMORY_PRIORITY_NORMAL;
    mem_info.memProps = VK_MEMORY_PROPERTY_SHAREABLE_BIT;
    err = vkAllocMemory(dev_.obj(), &mem_info, &event_mem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindObjectMemory(event, 0, event_mem, 0);
    ASSERT_VK_SUCCESS(err);

    err = vkResetEvent(event);
    ASSERT_VK_SUCCESS(err);

    for (int i = 0; i < ARRAY_SIZE(bufs); i++) {
        bufs[i].init(dev_, 4);
        add_memory_ref(bufs[i]);

        uint32_t *data = static_cast<uint32_t *>(bufs[i].map());
        data[0] = 0x22222222 * (i + 1);
        bufs[i].unmap();
    }

    cmd_.begin();

    vkCmdFillBuffer(cmd_.obj(), bufs[0].obj(), 0, 4, 0x11111111);
    // is this necessary?
    VK_BUFFER_MEMORY_BARRIER memory_barrier = bufs[0].buffer_memory_barrier(
            VK_MEMORY_OUTPUT_COPY_BIT, VK_MEMORY_INPUT_COPY_BIT, 0, 4);
    VK_BUFFER_MEMORY_BARRIER *pmemory_barrier = &memory_barrier;

    VK_PIPE_EVENT set_events[] = { VK_PIPE_EVENT_TRANSFER_COMPLETE };
    VK_PIPELINE_BARRIER pipeline_barrier = {};
    pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
    pipeline_barrier.eventCount = 1;
    pipeline_barrier.pEvents = set_events;
    pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
    pipeline_barrier.memBarrierCount = 1;
    pipeline_barrier.ppMemBarriers = (const void **)&pmemory_barrier;
    vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

    VK_BUFFER_COPY region = {};
    region.copySize = 4;
    vkCmdCopyBuffer(cmd_.obj(), bufs[0].obj(), bufs[1].obj(), 1, &region);

    memory_barrier = bufs[1].buffer_memory_barrier(
            VK_MEMORY_OUTPUT_COPY_BIT, VK_MEMORY_INPUT_COPY_BIT, 0, 4);
    pmemory_barrier = &memory_barrier;
    pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
    pipeline_barrier.eventCount = 1;
    pipeline_barrier.pEvents = set_events;
    pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
    pipeline_barrier.memBarrierCount = 1;
    pipeline_barrier.ppMemBarriers = (const void **)&pmemory_barrier;
    vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

    vkCmdCopyBuffer(cmd_.obj(), bufs[1].obj(), bufs[2].obj(), 1, &region);

    /* Use vkCmdSetEvent and vkCmdWaitEvents to test them.
     * This could be vkCmdPipelineBarrier.
     */
    vkCmdSetEvent(cmd_.obj(), event, VK_PIPE_EVENT_TRANSFER_COMPLETE);

    // Additional commands could go into the buffer here before the wait.

    memory_barrier = bufs[1].buffer_memory_barrier(
            VK_MEMORY_OUTPUT_COPY_BIT, VK_MEMORY_INPUT_CPU_READ_BIT, 0, 4);
    pmemory_barrier = &memory_barrier;
    VK_EVENT_WAIT_INFO wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_EVENT_WAIT_INFO;
    wait_info.eventCount = 1;
    wait_info.pEvents = &event;
    wait_info.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
    wait_info.memBarrierCount = 1;
    wait_info.ppMemBarriers = (const void **)&pmemory_barrier;
    vkCmdWaitEvents(cmd_.obj(), &wait_info);

    cmd_.end();

    submit_and_done();

    const uint32_t *data = static_cast<const uint32_t *>(bufs[2].map());
    EXPECT_EQ(0x11111111, data[0]);
    bufs[2].unmap();

    // All done with event memory, clean up
    err = vkBindObjectMemory(event, 0, VK_NULL_HANDLE, 0);
    ASSERT_VK_SUCCESS(err);

    err = vkDestroyObject(event);
    ASSERT_VK_SUCCESS(err);

    err = vkFreeMemory(event_mem);
    ASSERT_VK_SUCCESS(err);
}

class XglCmdBlitImageTest : public XglCmdBlitTest {
protected:
    void init_test_formats(VK_FLAGS features)
    {
        first_linear_format_ = VK_FMT_UNDEFINED;
        first_optimal_format_ = VK_FMT_UNDEFINED;

        for (std::vector<vk_testing::Device::Format>::const_iterator it = dev_.formats().begin();
             it != dev_.formats().end(); it++) {
            if (it->features & features) {
                test_formats_.push_back(*it);

                if (it->tiling == VK_LINEAR_TILING &&
                    first_linear_format_ == VK_FMT_UNDEFINED)
                    first_linear_format_ = it->format;
                if (it->tiling == VK_OPTIMAL_TILING &&
                    first_optimal_format_ == VK_FMT_UNDEFINED)
                    first_optimal_format_ = it->format;
            }
        }
    }

    void init_test_formats()
    {
        init_test_formats(static_cast<VK_FLAGS>(-1));
    }

    void fill_src(vk_testing::Image &img, const vk_testing::ImageChecker &checker)
    {
        if (img.transparent()) {
            checker.fill(img);
            return;
        }

        ASSERT_EQ(true, img.copyable());

        vk_testing::Buffer in_buf;
        in_buf.init(dev_, checker.buffer_size());
        checker.fill(in_buf);

        add_memory_ref(in_buf);
        add_memory_ref(img);

        // copy in and tile
        cmd_.begin();
        vkCmdCopyBufferToImage(cmd_.obj(), in_buf.obj(),
                img.obj(), VK_IMAGE_LAYOUT_TRANSFER_DESTINATION_OPTIMAL,
                checker.regions().size(), &checker.regions()[0]);
        cmd_.end();

        submit_and_done();
    }

    void check_dst(vk_testing::Image &img, const vk_testing::ImageChecker &checker)
    {
        if (img.transparent()) {
            DO(checker.check(img));
            return;
        }

        ASSERT_EQ(true, img.copyable());

        vk_testing::Buffer out_buf;
        out_buf.init(dev_, checker.buffer_size());

        add_memory_ref(img);
        add_memory_ref(out_buf);

        // copy out and linearize
        cmd_.begin();
        vkCmdCopyImageToBuffer(cmd_.obj(),
                img.obj(), VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL,
                out_buf.obj(),
                checker.regions().size(), &checker.regions()[0]);
        cmd_.end();

        submit_and_done();

        DO(checker.check(out_buf));
    }

    std::vector<vk_testing::Device::Format> test_formats_;
    VK_FORMAT first_linear_format_;
    VK_FORMAT first_optimal_format_;
};

class XglCmdCopyBufferToImageTest : public XglCmdBlitImageTest {
protected:
    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();
        init_test_formats(VK_FORMAT_IMAGE_COPY_BIT);
        ASSERT_NE(true, test_formats_.empty());
    }

    void test_copy_memory_to_image(const VK_IMAGE_CREATE_INFO &img_info, const vk_testing::ImageChecker &checker)
    {
        vk_testing::Buffer buf;
        vk_testing::Image img;

        buf.init(dev_, checker.buffer_size());
        checker.fill(buf);
        add_memory_ref(buf);

        img.init(dev_, img_info);
        add_memory_ref(img);

        cmd_.begin();
        vkCmdCopyBufferToImage(cmd_.obj(),
                buf.obj(),
                img.obj(), VK_IMAGE_LAYOUT_TRANSFER_DESTINATION_OPTIMAL,
                checker.regions().size(), &checker.regions()[0]);
        cmd_.end();

        submit_and_done();

        check_dst(img, checker);
    }

    void test_copy_memory_to_image(const VK_IMAGE_CREATE_INFO &img_info, const std::vector<VK_BUFFER_IMAGE_COPY> &regions)
    {
        vk_testing::ImageChecker checker(img_info, regions);
        test_copy_memory_to_image(img_info, checker);
    }

    void test_copy_memory_to_image(const VK_IMAGE_CREATE_INFO &img_info)
    {
        vk_testing::ImageChecker checker(img_info);
        test_copy_memory_to_image(img_info, checker);
    }
};

TEST_F(XglCmdCopyBufferToImageTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {

        // not sure what to do here
        if (it->format == VK_FMT_UNDEFINED ||
            (it->format >= VK_FMT_B8G8R8_UNORM &&
             it->format <= VK_FMT_B8G8R8_SRGB))
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;

        test_copy_memory_to_image(img_info);
    }
}

class XglCmdCopyImageToBufferTest : public XglCmdBlitImageTest {
protected:
    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();
        init_test_formats(VK_FORMAT_IMAGE_COPY_BIT);
        ASSERT_NE(true, test_formats_.empty());
    }

    void test_copy_image_to_memory(const VK_IMAGE_CREATE_INFO &img_info, const vk_testing::ImageChecker &checker)
    {
        vk_testing::Image img;
        vk_testing::Buffer buf;

        img.init(dev_, img_info);
        fill_src(img, checker);
        add_memory_ref(img);

        buf.init(dev_, checker.buffer_size());
        add_memory_ref(buf);

        cmd_.begin();
        vkCmdCopyImageToBuffer(cmd_.obj(),
                img.obj(), VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL,
                buf.obj(),
                checker.regions().size(), &checker.regions()[0]);
        cmd_.end();

        submit_and_done();

        checker.check(buf);
    }

    void test_copy_image_to_memory(const VK_IMAGE_CREATE_INFO &img_info, const std::vector<VK_BUFFER_IMAGE_COPY> &regions)
    {
        vk_testing::ImageChecker checker(img_info, regions);
        test_copy_image_to_memory(img_info, checker);
    }

    void test_copy_image_to_memory(const VK_IMAGE_CREATE_INFO &img_info)
    {
        vk_testing::ImageChecker checker(img_info);
        test_copy_image_to_memory(img_info, checker);
    }
};

TEST_F(XglCmdCopyImageToBufferTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {

        // not sure what to do here
        if (it->format == VK_FMT_UNDEFINED ||
            (it->format >= VK_FMT_B8G8R8_UNORM &&
             it->format <= VK_FMT_B8G8R8_SRGB))
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;

        test_copy_image_to_memory(img_info);
    }
}

class XglCmdCopyImageTest : public XglCmdBlitImageTest {
protected:
    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();
        init_test_formats(VK_FORMAT_IMAGE_COPY_BIT);
        ASSERT_NE(true, test_formats_.empty());
    }

    void test_copy_image(const VK_IMAGE_CREATE_INFO &src_info, const VK_IMAGE_CREATE_INFO &dst_info,
                         const std::vector<VK_IMAGE_COPY> &copies)
    {
        // convert VK_IMAGE_COPY to two sets of VK_BUFFER_IMAGE_COPY
        std::vector<VK_BUFFER_IMAGE_COPY> src_regions, dst_regions;
        VK_GPU_SIZE src_offset = 0, dst_offset = 0;
        for (std::vector<VK_IMAGE_COPY>::const_iterator it = copies.begin(); it != copies.end(); it++) {
            VK_BUFFER_IMAGE_COPY src_region = {}, dst_region = {};

            src_region.bufferOffset = src_offset;
            src_region.imageSubresource = it->srcSubresource;
            src_region.imageOffset = it->srcOffset;
            src_region.imageExtent = it->extent;
            src_regions.push_back(src_region);

            dst_region.bufferOffset = src_offset;
            dst_region.imageSubresource = it->destSubresource;
            dst_region.imageOffset = it->destOffset;
            dst_region.imageExtent = it->extent;
            dst_regions.push_back(dst_region);

            const VK_GPU_SIZE size = it->extent.width * it->extent.height * it->extent.depth;
            src_offset += vk_testing::get_format_size(src_info.format) * size;
            dst_offset += vk_testing::get_format_size(dst_info.format) * size;
        }

        vk_testing::ImageChecker src_checker(src_info, src_regions);
        vk_testing::ImageChecker dst_checker(dst_info, dst_regions);

        vk_testing::Image src;
        src.init(dev_, src_info);
        fill_src(src, src_checker);
        add_memory_ref(src);

        vk_testing::Image dst;
        dst.init(dev_, dst_info);
        add_memory_ref(dst);

        cmd_.begin();
        vkCmdCopyImage(cmd_.obj(),
                        src.obj(), VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL,
                        dst.obj(), VK_IMAGE_LAYOUT_TRANSFER_DESTINATION_OPTIMAL,
                        copies.size(), &copies[0]);
        cmd_.end();

        submit_and_done();

        check_dst(dst, dst_checker);
    }
};

TEST_F(XglCmdCopyImageTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {

        // not sure what to do here
        if (it->format == VK_FMT_UNDEFINED ||
            (it->format >= VK_FMT_B8G8R8_UNORM &&
             it->format <= VK_FMT_B8G8R8_SRGB))
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;

        VK_IMAGE_COPY copy = {};
        copy.srcSubresource = vk_testing::Image::subresource(VK_IMAGE_ASPECT_COLOR, 0, 0);
        copy.destSubresource = copy.srcSubresource;
        copy.extent = img_info.extent;

        test_copy_image(img_info, img_info, std::vector<VK_IMAGE_COPY>(&copy, &copy + 1));
    }
}

class XglCmdCloneImageDataTest : public XglCmdBlitImageTest {
protected:
    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();
        init_test_formats();
        ASSERT_NE(true, test_formats_.empty());
    }

    void test_clone_image_data(const VK_IMAGE_CREATE_INFO &img_info)
    {
        vk_testing::ImageChecker checker(img_info);
        vk_testing::Image src, dst;

        src.init(dev_, img_info);
        if (src.transparent() || src.copyable())
            fill_src(src, checker);
        add_memory_ref(src);

        dst.init(dev_, img_info);
        add_memory_ref(dst);

        const VK_IMAGE_LAYOUT layout = VK_IMAGE_LAYOUT_GENERAL;

        cmd_.begin();
        vkCmdCloneImageData(cmd_.obj(), src.obj(), layout, dst.obj(), layout);
        cmd_.end();

        submit_and_done();

        // cannot verify
        if (!dst.transparent() && !dst.copyable())
            return;

        check_dst(dst, checker);
    }
};

TEST_F(XglCmdCloneImageDataTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {
        // not sure what to do here
        if (it->format == VK_FMT_UNDEFINED ||
            (it->format >= VK_FMT_R32G32B32_UINT &&
             it->format <= VK_FMT_R32G32B32_SFLOAT) ||
            (it->format >= VK_FMT_B8G8R8_UNORM &&
             it->format <= VK_FMT_B8G8R8_SRGB) ||
            (it->format >= VK_FMT_BC1_RGB_UNORM &&
             it->format <= VK_FMT_ASTC_12x12_SRGB) ||
            (it->format >= VK_FMT_D16_UNORM &&
             it->format <= VK_FMT_D32_SFLOAT_S8_UINT) ||
            it->format == VK_FMT_R64G64B64_SFLOAT ||
            it->format == VK_FMT_R64G64B64A64_SFLOAT)
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;
        img_info.flags = VK_IMAGE_CREATE_CLONEABLE_BIT;

        const VK_IMAGE_SUBRESOURCE_RANGE range =
            vk_testing::Image::subresource_range(img_info, VK_IMAGE_ASPECT_COLOR);
        std::vector<VK_IMAGE_SUBRESOURCE_RANGE> ranges(&range, &range + 1);

        test_clone_image_data(img_info);
    }
}

class XglCmdClearColorImageTest : public XglCmdBlitImageTest {
protected:
    XglCmdClearColorImageTest() : test_raw_(false) {}
    XglCmdClearColorImageTest(bool test_raw) : test_raw_(test_raw) {}

    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();

        if (test_raw_)
            init_test_formats();
        else
            init_test_formats(VK_FORMAT_CONVERSION_BIT);

        ASSERT_NE(true, test_formats_.empty());
    }

    union Color {
        float color[4];
        uint32_t raw[4];
    };

    bool test_raw_;

    std::vector<uint8_t> color_to_raw(VK_FORMAT format, const float color[4])
    {
        std::vector<uint8_t> raw;

        // TODO support all formats
        switch (format) {
        case VK_FMT_R8G8B8A8_UNORM:
            raw.push_back(color[0] * 255.0f);
            raw.push_back(color[1] * 255.0f);
            raw.push_back(color[2] * 255.0f);
            raw.push_back(color[3] * 255.0f);
            break;
        case VK_FMT_B8G8R8A8_UNORM:
            raw.push_back(color[2] * 255.0f);
            raw.push_back(color[1] * 255.0f);
            raw.push_back(color[0] * 255.0f);
            raw.push_back(color[3] * 255.0f);
            break;
        default:
            break;
        }

        return raw;
    }

    std::vector<uint8_t> color_to_raw(VK_FORMAT format, const uint32_t color[4])
    {
        std::vector<uint8_t> raw;

        // TODO support all formats
        switch (format) {
        case VK_FMT_R8G8B8A8_UNORM:
            raw.push_back(static_cast<uint8_t>(color[0]));
            raw.push_back(static_cast<uint8_t>(color[1]));
            raw.push_back(static_cast<uint8_t>(color[2]));
            raw.push_back(static_cast<uint8_t>(color[3]));
            break;
        case VK_FMT_B8G8R8A8_UNORM:
            raw.push_back(static_cast<uint8_t>(color[2]));
            raw.push_back(static_cast<uint8_t>(color[1]));
            raw.push_back(static_cast<uint8_t>(color[0]));
            raw.push_back(static_cast<uint8_t>(color[3]));
            break;
        default:
            break;
        }

        return raw;
    }

    std::vector<uint8_t> color_to_raw(VK_FORMAT format, const VK_CLEAR_COLOR &color)
    {
        if (color.useRawValue)
            return color_to_raw(format, color.color.rawColor);
        else
            return color_to_raw(format, color.color.floatColor);
    }

    void test_clear_color_image(const VK_IMAGE_CREATE_INFO &img_info,
                                const VK_CLEAR_COLOR &clear_color,
                                const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges)
    {
        vk_testing::Image img;
        img.init(dev_, img_info);
        add_memory_ref(img);
        const VK_FLAGS all_cache_outputs =
                VK_MEMORY_OUTPUT_CPU_WRITE_BIT |
                VK_MEMORY_OUTPUT_SHADER_WRITE_BIT |
                VK_MEMORY_OUTPUT_COLOR_ATTACHMENT_BIT |
                VK_MEMORY_OUTPUT_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_MEMORY_OUTPUT_COPY_BIT;
        const VK_FLAGS all_cache_inputs =
                VK_MEMORY_INPUT_CPU_READ_BIT |
                VK_MEMORY_INPUT_INDIRECT_COMMAND_BIT |
                VK_MEMORY_INPUT_INDEX_FETCH_BIT |
                VK_MEMORY_INPUT_VERTEX_ATTRIBUTE_FETCH_BIT |
                VK_MEMORY_INPUT_UNIFORM_READ_BIT |
                VK_MEMORY_INPUT_SHADER_READ_BIT |
                VK_MEMORY_INPUT_COLOR_ATTACHMENT_BIT |
                VK_MEMORY_INPUT_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_MEMORY_INPUT_COPY_BIT;

        std::vector<VK_IMAGE_MEMORY_BARRIER> to_clear;
        std::vector<VK_IMAGE_MEMORY_BARRIER *> p_to_clear;
        std::vector<VK_IMAGE_MEMORY_BARRIER> to_xfer;
        std::vector<VK_IMAGE_MEMORY_BARRIER *> p_to_xfer;

        for (std::vector<VK_IMAGE_SUBRESOURCE_RANGE>::const_iterator it = ranges.begin();
             it != ranges.end(); it++) {
            to_clear.push_back(img.image_memory_barrier(all_cache_outputs, all_cache_inputs,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                    *it));
            p_to_clear.push_back(&to_clear.back());
            to_xfer.push_back(img.image_memory_barrier(all_cache_outputs, all_cache_inputs,
                    VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL, *it));
            p_to_xfer.push_back(&to_xfer.back());
        }

        cmd_.begin();

        VK_PIPE_EVENT set_events[] = { VK_PIPE_EVENT_GPU_COMMANDS_COMPLETE };
        VK_PIPELINE_BARRIER pipeline_barrier = {};
        pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
        pipeline_barrier.eventCount = 1;
        pipeline_barrier.pEvents = set_events;
        pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
        pipeline_barrier.memBarrierCount = to_clear.size();
        pipeline_barrier.ppMemBarriers = (const void **)&p_to_clear[0];
        vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

        vkCmdClearColorImage(cmd_.obj(),
                              img.obj(), VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                              clear_color, ranges.size(), &ranges[0]);

        pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
        pipeline_barrier.eventCount = 1;
        pipeline_barrier.pEvents = set_events;
        pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
        pipeline_barrier.memBarrierCount = to_xfer.size();
        pipeline_barrier.ppMemBarriers = (const void **)&p_to_xfer[0];
        vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

        cmd_.end();

        submit_and_done();

        // cannot verify
        if (!img.transparent() && !img.copyable())
            return;

        vk_testing::ImageChecker checker(img_info, ranges);

        const std::vector<uint8_t> solid_pattern = color_to_raw(img_info.format, clear_color);
        if (solid_pattern.empty())
            return;

        checker.set_solid_pattern(solid_pattern);
        check_dst(img, checker);
    }

    void test_clear_color_image(const VK_IMAGE_CREATE_INFO &img_info,
                                const float color[4],
                                const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges)
    {
        VK_CLEAR_COLOR c = {};
        memcpy(c.color.floatColor, color, sizeof(c.color.floatColor));
        test_clear_color_image(img_info, c, ranges);
    }
};

TEST_F(XglCmdClearColorImageTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {
        const float color[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;

        const VK_IMAGE_SUBRESOURCE_RANGE range =
            vk_testing::Image::subresource_range(img_info, VK_IMAGE_ASPECT_COLOR);
        std::vector<VK_IMAGE_SUBRESOURCE_RANGE> ranges(&range, &range + 1);

        test_clear_color_image(img_info, color, ranges);
    }
}

class XglCmdClearColorImageRawTest : public XglCmdClearColorImageTest {
protected:
    XglCmdClearColorImageRawTest() : XglCmdClearColorImageTest(true) {}

    void test_clear_color_image_raw(const VK_IMAGE_CREATE_INFO &img_info,
                                    const uint32_t color[4],
                                    const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges)
    {
        VK_CLEAR_COLOR c = {};
        c.useRawValue = true;
        memcpy(c.color.rawColor, color, sizeof(c.color.rawColor));
        test_clear_color_image(img_info, c, ranges);
    }
};

TEST_F(XglCmdClearColorImageRawTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {
        const uint32_t color[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };

        // not sure what to do here
        if (it->format == VK_FMT_UNDEFINED ||
            (it->format >= VK_FMT_R8G8B8_UNORM &&
             it->format <= VK_FMT_R8G8B8_SRGB) ||
            (it->format >= VK_FMT_B8G8R8_UNORM &&
             it->format <= VK_FMT_B8G8R8_SRGB) ||
            (it->format >= VK_FMT_R16G16B16_UNORM &&
             it->format <= VK_FMT_R16G16B16_SFLOAT) ||
            (it->format >= VK_FMT_R32G32B32_UINT &&
             it->format <= VK_FMT_R32G32B32_SFLOAT) ||
            it->format == VK_FMT_R64G64B64_SFLOAT ||
            it->format == VK_FMT_R64G64B64A64_SFLOAT ||
            (it->format >= VK_FMT_D16_UNORM &&
             it->format <= VK_FMT_D32_SFLOAT_S8_UINT))
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;

        const VK_IMAGE_SUBRESOURCE_RANGE range =
            vk_testing::Image::subresource_range(img_info, VK_IMAGE_ASPECT_COLOR);
        std::vector<VK_IMAGE_SUBRESOURCE_RANGE> ranges(&range, &range + 1);

        test_clear_color_image_raw(img_info, color, ranges);
    }
}

class XglCmdClearDepthStencilTest : public XglCmdBlitImageTest {
protected:
    virtual void SetUp()
    {
        XglCmdBlitTest::SetUp();
        init_test_formats(VK_FORMAT_DEPTH_ATTACHMENT_BIT |
                          VK_FORMAT_STENCIL_ATTACHMENT_BIT);
        ASSERT_NE(true, test_formats_.empty());
    }

    std::vector<uint8_t> ds_to_raw(VK_FORMAT format, float depth, uint32_t stencil)
    {
        std::vector<uint8_t> raw;

        // depth
        switch (format) {
        case VK_FMT_D16_UNORM:
        case VK_FMT_D16_UNORM_S8_UINT:
            {
                const uint16_t unorm = depth * 65535.0f;
                raw.push_back(unorm & 0xff);
                raw.push_back(unorm >> 8);
            }
            break;
        case VK_FMT_D32_SFLOAT:
        case VK_FMT_D32_SFLOAT_S8_UINT:
            {
                const union {
                    float depth;
                    uint32_t u32;
                } u = { depth };

                raw.push_back((u.u32      ) & 0xff);
                raw.push_back((u.u32 >>  8) & 0xff);
                raw.push_back((u.u32 >> 16) & 0xff);
                raw.push_back((u.u32 >> 24) & 0xff);
            }
            break;
        default:
            break;
        }

        // stencil
        switch (format) {
        case VK_FMT_S8_UINT:
            raw.push_back(stencil);
            break;
        case VK_FMT_D16_UNORM_S8_UINT:
            raw.push_back(stencil);
            raw.push_back(0);
            break;
        case VK_FMT_D32_SFLOAT_S8_UINT:
            raw.push_back(stencil);
            raw.push_back(0);
            raw.push_back(0);
            raw.push_back(0);
            break;
        default:
            break;
        }

        return raw;
    }

    void test_clear_depth_stencil(const VK_IMAGE_CREATE_INFO &img_info,
                                  float depth, uint32_t stencil,
                                  const std::vector<VK_IMAGE_SUBRESOURCE_RANGE> &ranges)
    {
        vk_testing::Image img;
        img.init(dev_, img_info);
        add_memory_ref(img);
        const VK_FLAGS all_cache_outputs =
                VK_MEMORY_OUTPUT_CPU_WRITE_BIT |
                VK_MEMORY_OUTPUT_SHADER_WRITE_BIT |
                VK_MEMORY_OUTPUT_COLOR_ATTACHMENT_BIT |
                VK_MEMORY_OUTPUT_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_MEMORY_OUTPUT_COPY_BIT;
        const VK_FLAGS all_cache_inputs =
                VK_MEMORY_INPUT_CPU_READ_BIT |
                VK_MEMORY_INPUT_INDIRECT_COMMAND_BIT |
                VK_MEMORY_INPUT_INDEX_FETCH_BIT |
                VK_MEMORY_INPUT_VERTEX_ATTRIBUTE_FETCH_BIT |
                VK_MEMORY_INPUT_UNIFORM_READ_BIT |
                VK_MEMORY_INPUT_SHADER_READ_BIT |
                VK_MEMORY_INPUT_COLOR_ATTACHMENT_BIT |
                VK_MEMORY_INPUT_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_MEMORY_INPUT_COPY_BIT;

        std::vector<VK_IMAGE_MEMORY_BARRIER> to_clear;
        std::vector<VK_IMAGE_MEMORY_BARRIER *> p_to_clear;
        std::vector<VK_IMAGE_MEMORY_BARRIER> to_xfer;
        std::vector<VK_IMAGE_MEMORY_BARRIER *> p_to_xfer;

        for (std::vector<VK_IMAGE_SUBRESOURCE_RANGE>::const_iterator it = ranges.begin();
             it != ranges.end(); it++) {
            to_clear.push_back(img.image_memory_barrier(all_cache_outputs, all_cache_inputs,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                    *it));
            p_to_clear.push_back(&to_clear.back());
            to_xfer.push_back(img.image_memory_barrier(all_cache_outputs, all_cache_inputs,
                    VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL, *it));
            p_to_xfer.push_back(&to_xfer.back());
        }

        cmd_.begin();

        VK_PIPE_EVENT set_events[] = { VK_PIPE_EVENT_GPU_COMMANDS_COMPLETE };
        VK_PIPELINE_BARRIER pipeline_barrier = {};
        pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
        pipeline_barrier.eventCount = 1;
        pipeline_barrier.pEvents = set_events;
        pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
        pipeline_barrier.memBarrierCount = to_clear.size();
        pipeline_barrier.ppMemBarriers = (const void **)&p_to_clear[0];
        vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

        vkCmdClearDepthStencil(cmd_.obj(),
                                img.obj(), VK_IMAGE_LAYOUT_CLEAR_OPTIMAL,
                                depth, stencil,
                                ranges.size(), &ranges[0]);

        pipeline_barrier.sType = VK_STRUCTURE_TYPE_PIPELINE_BARRIER;
        pipeline_barrier.eventCount = 1;
        pipeline_barrier.pEvents = set_events;
        pipeline_barrier.waitEvent = VK_WAIT_EVENT_TOP_OF_PIPE;
        pipeline_barrier.memBarrierCount = to_xfer.size();
        pipeline_barrier.ppMemBarriers = (const void **)&p_to_xfer[0];
        vkCmdPipelineBarrier(cmd_.obj(), &pipeline_barrier);

        cmd_.end();

        submit_and_done();

        // cannot verify
        if (!img.transparent() && !img.copyable())
            return;

        vk_testing::ImageChecker checker(img_info, ranges);

        checker.set_solid_pattern(ds_to_raw(img_info.format, depth, stencil));
        check_dst(img, checker);
    }
};

TEST_F(XglCmdClearDepthStencilTest, Basic)
{
    for (std::vector<vk_testing::Device::Format>::const_iterator it = test_formats_.begin();
         it != test_formats_.end(); it++) {
        // known driver issues
        if (it->format == VK_FMT_S8_UINT ||
            it->format == VK_FMT_D24_UNORM ||
            it->format == VK_FMT_D16_UNORM_S8_UINT ||
            it->format == VK_FMT_D24_UNORM_S8_UINT)
            continue;

        VK_IMAGE_CREATE_INFO img_info = vk_testing::Image::create_info();
        img_info.imageType = VK_IMAGE_2D;
        img_info.format = it->format;
        img_info.extent.width = 64;
        img_info.extent.height = 64;
        img_info.tiling = it->tiling;
        img_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_BIT;

        const VK_IMAGE_SUBRESOURCE_RANGE range =
            vk_testing::Image::subresource_range(img_info, VK_IMAGE_ASPECT_DEPTH);
        std::vector<VK_IMAGE_SUBRESOURCE_RANGE> ranges(&range, &range + 1);

        test_clear_depth_stencil(img_info, 0.25f, 63, ranges);
    }
}

}; // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    vk_testing::set_error_callback(test_error_callback);

    environment = new vk_testing::Environment();

    if (!environment->parse_args(argc, argv))
        return -1;

    ::testing::AddGlobalTestEnvironment(environment);

    return RUN_ALL_TESTS();
}
