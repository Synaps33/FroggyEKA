#pragma once

#include <drivers/graphics/graphics.h>
#include <drivers/itc.h>
#include <common/vecx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace eka2l1::drivers {

struct sw_bitmap {
    eka2l1::vec2 size{0, 0};
    std::uint32_t bpp{32};
    std::vector<std::uint8_t> data;
};

class libretro_graphics_driver final : public graphics_driver {
private:
    struct rgba8 {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
        std::uint8_t a;
    };

    std::atomic<bool> running_{false};
    std::uint32_t next_handle_{1};
    std::unordered_map<drivers::handle, sw_bitmap> bitmaps_;
    drivers::handle bound_bitmap_{0};
    rgba8 brush_color_{255, 255, 255, 255};
    bool blending_{false};
    std::mutex bitmap_mutex_;

    static std::size_t bytes_per_pixel(const std::uint32_t bpp) {
        return (bpp <= 8) ? 1 : (bpp + 7) / 8;
    }

    static std::uint8_t expand_nibble(const std::uint16_t value) {
        return static_cast<std::uint8_t>((value << 4) | value);
    }

    static std::uint8_t clamp_byte(const std::uint32_t value) {
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(value, 255));
    }

    static rgba8 read_raw_pixel(const std::uint8_t *pixel, const std::uint32_t bpp) {
        switch (bpp) {
        case 8:
            return { pixel[0], pixel[0], pixel[0], pixel[0] };

        case 12: {
            const std::uint16_t value = static_cast<std::uint16_t>(pixel[0])
                | (static_cast<std::uint16_t>(pixel[1]) << 8);
            return { expand_nibble((value >> 8) & 0xF), expand_nibble((value >> 4) & 0xF),
                expand_nibble(value & 0xF), 255 };
        }

        case 16: {
            const std::uint16_t value = static_cast<std::uint16_t>(pixel[0])
                | (static_cast<std::uint16_t>(pixel[1]) << 8);
            const std::uint8_t r = static_cast<std::uint8_t>((value >> 11) & 0x1F);
            const std::uint8_t g = static_cast<std::uint8_t>((value >> 5) & 0x3F);
            const std::uint8_t b = static_cast<std::uint8_t>(value & 0x1F);
            return { static_cast<std::uint8_t>((r << 3) | (r >> 2)),
                static_cast<std::uint8_t>((g << 2) | (g >> 4)),
                static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255 };
        }

        case 24:
            // Symbian color16m bitmaps are stored as BGR bytes.
            return { pixel[2], pixel[1], pixel[0], 255 };

        case 32:
            // Symbian color16mu/color16ma bitmaps are stored as BGRA bytes.
            return { pixel[2], pixel[1], pixel[0], pixel[3] };

        default:
            return { 0, 0, 0, 0 };
        }
    }

    static void write_raw_pixel(std::uint8_t *pixel, const std::uint32_t bpp, const rgba8 color) {
        switch (bpp) {
        case 8:
            pixel[0] = color.r;
            break;

        case 12: {
            const std::uint16_t value = static_cast<std::uint16_t>((color.r >> 4) << 8)
                | static_cast<std::uint16_t>((color.g >> 4) << 4) | (color.b >> 4);
            pixel[0] = static_cast<std::uint8_t>(value);
            pixel[1] = static_cast<std::uint8_t>(value >> 8);
            break;
        }

        case 16: {
            const std::uint16_t value = static_cast<std::uint16_t>((color.r >> 3) << 11)
                | static_cast<std::uint16_t>((color.g >> 2) << 5) | (color.b >> 3);
            pixel[0] = static_cast<std::uint8_t>(value);
            pixel[1] = static_cast<std::uint8_t>(value >> 8);
            break;
        }

        case 24:
            pixel[0] = color.b;
            pixel[1] = color.g;
            pixel[2] = color.r;
            break;

        case 32:
            pixel[0] = color.b;
            pixel[1] = color.g;
            pixel[2] = color.r;
            pixel[3] = color.a;
            break;

        default:
            break;
        }
    }

    static rgba8 read_pixel(const sw_bitmap &bitmap, const int x, const int y) {
        if (x < 0 || y < 0 || x >= bitmap.size.x || y >= bitmap.size.y) {
            return { 0, 0, 0, 0 };
        }

        const std::size_t offset = (static_cast<std::size_t>(y) * bitmap.size.x + x)
            * bytes_per_pixel(bitmap.bpp);
        return read_raw_pixel(bitmap.data.data() + offset, bitmap.bpp);
    }

    static void write_pixel(sw_bitmap &bitmap, const int x, const int y, const rgba8 color) {
        if (x < 0 || y < 0 || x >= bitmap.size.x || y >= bitmap.size.y) {
            return;
        }

        const std::size_t offset = (static_cast<std::size_t>(y) * bitmap.size.x + x)
            * bytes_per_pixel(bitmap.bpp);
        write_raw_pixel(bitmap.data.data() + offset, bitmap.bpp, color);
    }

    static rgba8 blend_pixel(const rgba8 source, const rgba8 destination) {
        if (source.a == 255) {
            return source;
        }

        if (source.a == 0) {
            return destination;
        }

        const std::uint32_t inverse = 255 - source.a;
        return {
            clamp_byte((static_cast<std::uint32_t>(source.r) * source.a
                + static_cast<std::uint32_t>(destination.r) * inverse + 127) / 255),
            clamp_byte((static_cast<std::uint32_t>(source.g) * source.a
                + static_cast<std::uint32_t>(destination.g) * inverse + 127) / 255),
            clamp_byte((static_cast<std::uint32_t>(source.b) * source.a
                + static_cast<std::uint32_t>(destination.b) * inverse + 127) / 255),
            clamp_byte(source.a + (static_cast<std::uint32_t>(destination.a) * inverse + 127) / 255)
        };
    }

    void copy_bitmap_data(const drivers::handle handle, const std::size_t size,
        const eka2l1::vec2 &offset, const eka2l1::vec2 &dim, const void *data,
        const std::size_t pixels_per_line) {
        auto bitmap_it = bitmaps_.find(handle);
        if (bitmap_it == bitmaps_.end() || !data || size == 0) {
            return;
        }

        sw_bitmap &bitmap = bitmap_it->second;
        const std::size_t source_bpp = bytes_per_pixel(bitmap.bpp);
        const std::size_t source_pixels_per_line = pixels_per_line ? pixels_per_line : dim.x;
        const std::size_t source_pitch = (source_pixels_per_line * source_bpp + 3) & ~std::size_t(3);
        const auto *source = reinterpret_cast<const std::uint8_t *>(data);

        for (int y = 0; y < dim.y; ++y) {
            for (int x = 0; x < dim.x; ++x) {
                const std::size_t source_offset = static_cast<std::size_t>(y) * source_pitch
                    + static_cast<std::size_t>(x) * source_bpp;
                if (source_offset + source_bpp > size) {
                    break;
                }

                const int destination_x = offset.x + x;
                const int destination_y = offset.y + y;
                if (destination_x >= 0 && destination_y >= 0
                    && destination_x < bitmap.size.x && destination_y < bitmap.size.y) {
                    std::memcpy(bitmap.data.data()
                        + (static_cast<std::size_t>(destination_y) * bitmap.size.x + destination_x) * source_bpp,
                        source + source_offset, source_bpp);
                }
            }
        }
    }

    void draw_bitmap(const command &cmd) {
        const drivers::handle source_handle = static_cast<drivers::handle>(cmd.data_[0]);
        const drivers::handle mask_handle = static_cast<drivers::handle>(cmd.data_[1]);
        auto source_it = bitmaps_.find(source_handle);
        auto destination_it = bitmaps_.find(bound_bitmap_);
        if (source_it == bitmaps_.end() || destination_it == bitmaps_.end()) {
            return;
        }

        sw_bitmap source = source_it->second;
        const sw_bitmap *mask = nullptr;
        auto mask_it = bitmaps_.find(mask_handle);
        if (mask_it != bitmaps_.end()) {
            mask = &mask_it->second;
        }

        std::int32_t destination_x = 0;
        std::int32_t destination_y = 0;
        std::int32_t destination_width = 0;
        std::int32_t destination_height = 0;
        std::int32_t source_x = 0;
        std::int32_t source_y = 0;
        std::int32_t source_width = 0;
        std::int32_t source_height = 0;
        unpack_u64_to_2u32(cmd.data_[2], destination_x, destination_y);
        unpack_u64_to_2u32(cmd.data_[3], destination_width, destination_height);
        unpack_u64_to_2u32(cmd.data_[4], source_x, source_y);
        unpack_u64_to_2u32(cmd.data_[5], source_width, source_height);

        if (source_width == 0) {
            source_width = source.size.x;
        }
        if (source_height == 0) {
            source_height = source.size.y;
        }
        if (destination_width == 0) {
            destination_width = source_width;
        }
        if (destination_height == 0) {
            destination_height = source_height;
        }
        if (source_width <= 0 || source_height <= 0 || destination_width <= 0 || destination_height <= 0) {
            return;
        }

        std::uint32_t rotation_bits = static_cast<std::uint32_t>(cmd.data_[7]);
        float rotation = 0.0f;
        std::memcpy(&rotation, &rotation_bits, sizeof(rotation));
        int rotation_quarters = static_cast<int>(std::round(rotation / 90.0f)) % 4;
        if (rotation_quarters < 0) {
            rotation_quarters += 4;
        }
        const std::uint32_t flags = static_cast<std::uint32_t>(cmd.data_[7] >> 32);

        // Drawing a bitmap onto itself is used by a few window-server paths. Read
        // from a snapshot so the destination writes cannot alter later source pixels.
        const sw_bitmap *source_bitmap = &source;
        const sw_bitmap *mask_bitmap = mask;
        sw_bitmap mask_copy;
        if (mask && mask == &source_it->second && source_handle == bound_bitmap_) {
            mask_copy = *mask;
            mask_bitmap = &mask_copy;
        }

        for (int y = 0; y < destination_height; ++y) {
            for (int x = 0; x < destination_width; ++x) {
                int sample_x = source_x + (x * source_width) / destination_width;
                int sample_y = source_y + (y * source_height) / destination_height;

                if (flags & bitmap_draw_flag_flip) {
                    sample_y = source_y + source_height - 1 - ((y * source_height) / destination_height);
                }

                switch (rotation_quarters) {
                case 1:
                    sample_x = source_x + source_width - 1 - ((y * source_width) / destination_height);
                    sample_y = source_y + ((x * source_height) / destination_width);
                    break;
                case 2:
                    sample_x = source_x + source_width - 1 - ((x * source_width) / destination_width);
                    sample_y = source_y + source_height - 1 - ((y * source_height) / destination_height);
                    break;
                case 3:
                    sample_x = source_x + ((y * source_width) / destination_height);
                    sample_y = source_y + source_height - 1 - ((x * source_height) / destination_width);
                    break;
                default:
                    break;
                }

                rgba8 color = read_pixel(*source_bitmap, sample_x, sample_y);
                if (flags & bitmap_draw_flag_use_brush) {
                    color.r = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.r) * brush_color_.r) / 255);
                    color.g = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.g) * brush_color_.g) / 255);
                    color.b = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.b) * brush_color_.b) / 255);
                    color.a = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.a) * brush_color_.a) / 255);
                }

                if (mask_bitmap) {
                    rgba8 mask_color = read_pixel(*mask_bitmap, sample_x, sample_y);
                    std::uint8_t mask_alpha = mask_color.a;
                    if (flags & bitmap_draw_flag_invert_mask) {
                        mask_alpha = 255 - mask_alpha;
                    }
                    color.a = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.a) * mask_alpha) / 255);
                }

                const int target_x = destination_x + x;
                const int target_y = destination_y + y;
                if (target_x < 0 || target_y < 0 || target_x >= destination_it->second.size.x
                    || target_y >= destination_it->second.size.y) {
                    continue;
                }

                if (blending_) {
                    color = blend_pixel(color, read_pixel(destination_it->second, target_x, target_y));
                }
                write_pixel(destination_it->second, target_x, target_y, color);
            }
        }
    }

    void clear_bitmap(const command &cmd) {
        auto bitmap_it = bitmaps_.find(bound_bitmap_);
        if (bitmap_it == bitmaps_.end() || !(cmd.data_[3] & draw_buffer_bit_color_buffer)) {
            return;
        }

        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float alpha = 0.0f;
        std::uint32_t low = static_cast<std::uint32_t>(cmd.data_[0]);
        std::uint32_t high = static_cast<std::uint32_t>(cmd.data_[0] >> 32);
        std::memcpy(&red, &low, sizeof(red));
        std::memcpy(&green, &high, sizeof(green));
        low = static_cast<std::uint32_t>(cmd.data_[1]);
        high = static_cast<std::uint32_t>(cmd.data_[1] >> 32);
        std::memcpy(&blue, &low, sizeof(blue));
        std::memcpy(&alpha, &high, sizeof(alpha));

        const rgba8 color = {
            clamp_byte(static_cast<std::uint32_t>(std::max(0.0f, std::min(1.0f, red)) * 255.0f)),
            clamp_byte(static_cast<std::uint32_t>(std::max(0.0f, std::min(1.0f, green)) * 255.0f)),
            clamp_byte(static_cast<std::uint32_t>(std::max(0.0f, std::min(1.0f, blue)) * 255.0f)),
            clamp_byte(static_cast<std::uint32_t>(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f))
        };

        for (int y = 0; y < bitmap_it->second.size.y; ++y) {
            for (int x = 0; x < bitmap_it->second.size.x; ++x) {
                write_pixel(bitmap_it->second, x, y, color);
            }
        }
    }

    void read_bitmap(const command &cmd) {
        auto bitmap_it = bitmaps_.find(static_cast<drivers::handle>(cmd.data_[0]));
        auto *destination = reinterpret_cast<std::uint8_t *>(cmd.data_[4]);
        if (bitmap_it == bitmaps_.end() || !destination) {
            return;
        }

        std::int32_t source_x = 0;
        std::int32_t source_y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        unpack_u64_to_2u32(cmd.data_[1], source_x, source_y);
        unpack_u64_to_2u32(cmd.data_[2], width, height);
        const std::uint32_t output_bpp = static_cast<std::uint32_t>(cmd.data_[3]);
        const std::size_t output_bpp_bytes = bytes_per_pixel(output_bpp);
        const std::size_t output_pitch = ((static_cast<std::size_t>(width) * output_bpp_bytes) + 3) & ~std::size_t(3);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const rgba8 color = read_pixel(bitmap_it->second, source_x + x, source_y + y);
                write_raw_pixel(destination + static_cast<std::size_t>(y) * output_pitch
                    + static_cast<std::size_t>(x) * output_bpp_bytes, output_bpp, color);
            }
        }
    }

public:
    libretro_graphics_driver()
        : graphics_driver(graphic_api::opengl) {
    }

    ~libretro_graphics_driver() override {
        abort();
    }

    void run() override {
        running_ = true;
    }

    void abort() override {
        running_ = false;
        cond_.notify_all();
    }

    bool aborted() const override {
        return !running_;
    }

    std::uint32_t max_texture_size() const override {
        return 2048;
    }

    void update_bitmap(drivers::handle h, const std::size_t size, const eka2l1::vec2 &offset,
        const eka2l1::vec2 &dim, const void *data, const std::size_t pixels_per_line = 0) override {
        std::lock_guard<std::mutex> guard(bitmap_mutex_);
        copy_bitmap_data(h, size, offset, dim, data, pixels_per_line);
    }

    void set_viewport(const eka2l1::rect &viewport) override {
        (void)viewport;
    }

    void update_surface(void *surface) override {
        (void)surface;
    }

    void update_surface_size(const eka2l1::vec2 &size) override {
        (void)size;
    }

    void set_upscale_shader(const std::string &name) override {
        (void)name;
    }

    std::string get_active_upscale_shader() const override {
        return {};
    }

    bool support_extension(const graphics_driver_extension ext) override {
        (void)ext;
        return false;
    }

    bool query_extension_value(const graphics_driver_extension_query query, void *data_ptr) override {
        (void)query;
        (void)data_ptr;
        return false;
    }

    void submit_command_list(command_list &cmd_list) override {
        std::lock_guard<std::mutex> guard(bitmap_mutex_);

        for (std::size_t i = 0; i < cmd_list.size_; ++i) {
            command &cmd = cmd_list.base_[i];
            switch (cmd.opcode_) {
            case graphics_driver_create_bitmap: {
                drivers::handle *out_handle = reinterpret_cast<drivers::handle *>(cmd.data_[2]);
                const std::uint32_t width = static_cast<std::uint32_t>(cmd.data_[0]);
                const std::uint32_t height = static_cast<std::uint32_t>(cmd.data_[0] >> 32);
                const std::uint32_t bpp = static_cast<std::uint32_t>(cmd.data_[1]);
                const drivers::handle handle = next_handle_++;
                if (out_handle) {
                    *out_handle = handle;
                }

                sw_bitmap &bitmap = bitmaps_[handle];
                bitmap.size = eka2l1::vec2(width, height);
                bitmap.bpp = bpp;
                bitmap.data.assign(static_cast<std::size_t>(width) * height * bytes_per_pixel(bpp), 0);
                break;
            }

            case graphics_driver_bind_bitmap:
                bound_bitmap_ = static_cast<drivers::handle>(cmd.data_[0]);
                break;

            case graphics_driver_update_bitmap:
                copy_bitmap_data(static_cast<drivers::handle>(cmd.data_[0]),
                    static_cast<std::size_t>(cmd.data_[2]),
                    eka2l1::vec2(static_cast<std::int32_t>(cmd.data_[3]), static_cast<std::int32_t>(cmd.data_[3] >> 32)),
                    eka2l1::vec2(static_cast<std::int32_t>(cmd.data_[4]), static_cast<std::int32_t>(cmd.data_[4] >> 32)),
                    reinterpret_cast<const void *>(cmd.data_[1]), static_cast<std::size_t>(cmd.data_[5]));
                if (cmd.data_[6]) {
                    delete[] reinterpret_cast<const std::uint8_t *>(cmd.data_[1]);
                }
                break;

            case graphics_driver_resize_bitmap: {
                auto bitmap_it = bitmaps_.find(static_cast<drivers::handle>(cmd.data_[0]));
                if (bitmap_it != bitmaps_.end()) {
                    const std::uint32_t width = static_cast<std::uint32_t>(cmd.data_[1]);
                    const std::uint32_t height = static_cast<std::uint32_t>(cmd.data_[1] >> 32);
                    bitmap_it->second.size = eka2l1::vec2(width, height);
                    bitmap_it->second.data.assign(static_cast<std::size_t>(width) * height
                        * bytes_per_pixel(bitmap_it->second.bpp), 0);
                }
                break;
            }

            case graphics_driver_read_bitmap:
                read_bitmap(cmd);
                break;

            case graphics_driver_destroy_bitmap:
                bitmaps_.erase(static_cast<drivers::handle>(cmd.data_[0]));
                if (bound_bitmap_ == cmd.data_[0]) {
                    bound_bitmap_ = 0;
                }
                break;

            case graphics_driver_draw_bitmap:
                draw_bitmap(cmd);
                break;

            case graphics_driver_clear:
                clear_bitmap(cmd);
                break;

            case graphics_driver_set_brush_color: {
                std::uint32_t red = static_cast<std::uint32_t>(cmd.data_[0]);
                std::uint32_t green = static_cast<std::uint32_t>(cmd.data_[0] >> 32);
                std::uint32_t blue = static_cast<std::uint32_t>(cmd.data_[1]);
                std::uint32_t alpha = static_cast<std::uint32_t>(cmd.data_[1] >> 32);
                brush_color_ = { clamp_byte(red), clamp_byte(green), clamp_byte(blue), clamp_byte(alpha) };
                break;
            }

            case graphics_driver_set_feature:
                if (static_cast<graphics_feature>(cmd.data_[0]) == graphics_feature::blend) {
                    blending_ = cmd.data_[0] >> 32;
                }
                break;

            case graphics_driver_clip_region:
                delete[] reinterpret_cast<const std::uint8_t *>(cmd.data_[1]);
                break;

            default:
                break;
            }

            if (cmd.status_) {
                *cmd.status_ = 0;
            }
        }

        cond_.notify_all();

        delete[] cmd_list.base_;
        cmd_list.base_ = nullptr;
        cmd_list.size_ = 0;
    }
};

} // namespace eka2l1::drivers
