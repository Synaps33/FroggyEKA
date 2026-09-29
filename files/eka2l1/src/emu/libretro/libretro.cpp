#include "libretro.h"
#include "libretro_graphics_driver.h"

#include <system/epoc.h>
#include <config/config.h>
#include <config/app_settings.h>
#include <kernel/kernel.h>
#include <kernel/thread.h>
#include <kernel/timing.h>
#include <mem/model/multiple/chunk.h>
#include <services/window/window.h>
#include <services/window/screen.h>
#include <services/applist/applist.h>
#include <package/manager.h>
#include <utils/apacmd.h>
#include <common/fileutils.h>
#include <common/path.h>
#include <common/log.h>
#include <common/cvt.h>
#include <common/algorithm.h>
#include <common/applauncher.h>
#include <drivers/audio/audio.h>
#include <drivers/audio/stream.h>
#include <drivers/itc.h>
#include <drivers/ui/input_dialog.h>

#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <thread>
#include <utility>
#include <cstdarg>
#include <cstdio>

#if defined(SF2000) || defined(GB300V2)
extern "C" void xlog(const char *fmt, ...);
#else
extern "C" inline void xlog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}
#endif

#if defined(SF2000) || defined(GB300V2)
// Debug helper for the baremetal port: report big C++ allocations together with
// the address that requested them, so a runaway size can be traced back to a
// symbol in core.elf.map without a debugger.
#include <cstdlib>
#include <new>

static constexpr std::size_t BIG_ALLOC_TRACE_THRESHOLD = 16 * 1024 * 1024;

__attribute__((noinline)) void *operator new(std::size_t size) {
    if (size >= BIG_ALLOC_TRACE_THRESHOLD) {
        printf("[EKA2L1-TRACE] operator new size=0x%X ra=%p\n", static_cast<unsigned>(size), __builtin_return_address(0));
    }

    void *ptr = std::malloc(size);

    if (!ptr) {
        printf("[EKA2L1-TRACE] operator new FAIL size=0x%X ra=%p\n", static_cast<unsigned>(size), __builtin_return_address(0));
#if defined(SF2000) || defined(GB300V2)
        eka2l1::mem::multiple_mem_model_chunk::check_all_guards("operator new fail");
#endif
        throw std::bad_alloc();
    }

    return ptr;
}

__attribute__((noinline)) void *operator new[](std::size_t size) {
    if (size >= BIG_ALLOC_TRACE_THRESHOLD) {
        printf("[EKA2L1-TRACE] operator new[] size=0x%X ra=%p\n", static_cast<unsigned>(size), __builtin_return_address(0));
    }

    void *ptr = std::malloc(size);

    if (!ptr) {
        printf("[EKA2L1-TRACE] operator new[] FAIL size=0x%X ra=%p\n", static_cast<unsigned>(size), __builtin_return_address(0));
#if defined(SF2000) || defined(GB300V2)
        eka2l1::mem::multiple_mem_model_chunk::check_all_guards("operator new[] fail");
#endif
        throw std::bad_alloc();
    }

    return ptr;
}
#endif

namespace {
    class libretro_audio_driver;

    class libretro_audio_output_stream final : public eka2l1::drivers::audio_output_stream {
    private:
        eka2l1::drivers::data_callback callback_;
        std::atomic<bool> playing_{false};
        std::atomic<bool> paused_{false};
        float volume_{1.0f};
        std::atomic<std::uint64_t> frame_position_{0};

    public:
        libretro_audio_output_stream(eka2l1::drivers::audio_driver *driver, const std::uint32_t sample_rate,
            const std::uint8_t channels, eka2l1::drivers::data_callback callback)
            : audio_output_stream(driver, sample_rate, channels)
            , callback_(std::move(callback)) {
        }

        ~libretro_audio_output_stream() override;

        bool start() override {
            playing_ = true;
            paused_ = false;
            return true;
        }

        bool stop() override {
            playing_ = false;
            paused_ = false;
            return true;
        }

        void pause() override {
            paused_ = true;
        }

        bool is_playing() override {
            return playing_ && !paused_;
        }

        bool is_pausing() override {
            return paused_;
        }

        bool set_volume(const float volume) override {
            volume_ = volume;
            return true;
        }

        float get_volume() const override {
            return volume_;
        }

        bool current_frame_position(std::uint64_t *pos) override {
            if (pos) {
                *pos = frame_position_.load(std::memory_order_relaxed);
            }
            return true;
        }

        void pump(std::size_t requested_frames);
    };

    class libretro_audio_input_stream final : public eka2l1::drivers::audio_input_stream {
    public:
        libretro_audio_input_stream(eka2l1::drivers::audio_driver *driver, const std::uint32_t sample_rate,
            const std::uint8_t channels)
            : audio_input_stream(driver, sample_rate, channels) {
        }

        bool start() override {
            return false;
        }

        bool stop() override {
            return true;
        }

        bool is_recording() override {
            return false;
        }

        bool current_frame_position(std::uint64_t *pos) override {
            if (pos) {
                *pos = 0;
            }
            return true;
        }
    };

    class libretro_audio_driver final : public eka2l1::drivers::audio_driver {
    private:
        std::vector<libretro_audio_output_stream *> streams_;

    public:
        libretro_audio_driver(const std::uint32_t volume, const eka2l1::drivers::player_type player)
            : audio_driver(volume, player) {
        }

        std::unique_ptr<eka2l1::drivers::audio_output_stream> new_output_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, eka2l1::drivers::data_callback callback) override {
            auto s = std::make_unique<libretro_audio_output_stream>(this, sample_rate, channels, std::move(callback));
            streams_.push_back(s.get());
            return s;
        }

        void unregister_stream(libretro_audio_output_stream *s) {
            auto it = std::find(streams_.begin(), streams_.end(), s);
            if (it != streams_.end()) {
                streams_.erase(it);
            }
        }

        std::unique_ptr<eka2l1::drivers::audio_input_stream> new_input_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, eka2l1::drivers::data_callback callback) override {
            (void)callback;
            return std::make_unique<libretro_audio_input_stream>(this, sample_rate, channels);
        }

        std::uint32_t native_sample_rate() override {
            return 44100;
        }

        void pump_all(std::size_t frames) {
            for (auto *stream : streams_) {
                if (stream) {
                    stream->pump(frames);
                }
            }
        }
    };

    libretro_audio_output_stream::~libretro_audio_output_stream() {
        stop();
        if (driver_) {
            static_cast<libretro_audio_driver *>(driver_)->unregister_stream(this);
        }
    }
}

static retro_video_refresh_t s_video_cb = nullptr;
static retro_audio_sample_t s_audio_cb = nullptr;
static retro_audio_sample_batch_t s_audio_batch_cb = nullptr;
static retro_input_poll_t s_input_poll_cb = nullptr;
static retro_input_state_t s_input_state_cb = nullptr;
static retro_environment_t s_environ_cb = nullptr;

namespace {
    void libretro_audio_output_stream::pump(std::size_t requested_frames) {
        if (!playing_ || paused_ || !callback_ || !s_audio_batch_cb) return;

        int16_t buffer[1024 * 2];
        std::size_t to_render = std::min<std::size_t>(requested_frames, 1024);

        if (channels == 2) {
            std::size_t frames = callback_(buffer, to_render);
            if (frames > 0) {
                frame_position_.fetch_add(frames, std::memory_order_relaxed);
                s_audio_batch_cb(buffer, frames);
            }
        } else {
            int16_t mono_buf[1024];
            std::size_t frames = callback_(mono_buf, to_render);
            if (frames > 0) {
                frame_position_.fetch_add(frames, std::memory_order_relaxed);
                for (std::size_t i = 0; i < frames; ++i) {
                    buffer[i * 2] = mono_buf[i];
                    buffer[i * 2 + 1] = mono_buf[i];
                }
                s_audio_batch_cb(buffer, frames);
            }
        }
    }
}

static eka2l1::config::state s_conf;
static std::unique_ptr<eka2l1::config::app_settings> s_app_settings;
static std::unique_ptr<eka2l1::system> s_symsys;
static std::unique_ptr<eka2l1::drivers::audio_driver> s_audio_driver;
static std::unique_ptr<eka2l1::drivers::libretro_graphics_driver> s_graphics_driver;
static eka2l1::window_server *s_winserv = nullptr;

static uint16_t s_fb[320 * 240];
static bool s_frame_drawn = false;
static bool s_game_loaded = false;

struct key_binding {
    unsigned retro_id;
    uint32_t epoc_code;
    bool prev_state;
};

static key_binding s_key_bindings[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,     eka2l1::epoc::std_key_up_arrow,    false },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   eka2l1::epoc::std_key_down_arrow,  false },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   eka2l1::epoc::std_key_left_arrow,  false },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  eka2l1::epoc::std_key_right_arrow, false },
    { RETRO_DEVICE_ID_JOYPAD_A,      '5',                                false }, // N-Gage Key 5 (Fire)
    { RETRO_DEVICE_ID_JOYPAD_B,      '7',                                false }, // N-Gage Key 7 (Action)
    { RETRO_DEVICE_ID_JOYPAD_X,      eka2l1::epoc::std_key_device_0,    false }, // Left Softkey
    { RETRO_DEVICE_ID_JOYPAD_Y,      eka2l1::epoc::std_key_device_1,    false }, // Right Softkey
    { RETRO_DEVICE_ID_JOYPAD_START,  eka2l1::epoc::std_key_enter,       false }, // Enter / OK
    { RETRO_DEVICE_ID_JOYPAD_SELECT, eka2l1::epoc::std_key_device_3,    false }, // D-Pad Center
    { RETRO_DEVICE_ID_JOYPAD_L,      '1',                                false },
    { RETRO_DEVICE_ID_JOYPAD_R,      '3',                                false },
};

static void on_screen_redraw(void *userdata, eka2l1::epoc::screen *scr, const bool is_dsa) {
    (void)userdata;
    (void)is_dsa;
    if (!scr) return;
    static int s_redraw_count = 0;
    s_redraw_count++;
    // xlog("[LIBRETRO] on_screen_redraw #%d (is_dsa=%d)\n", s_redraw_count, (int)is_dsa);
    uint8_t *src = scr->screen_buffer_ptr();
    if (!src) return;

    const auto &mode = scr->current_mode();
    int w = mode.size.x;
    int h = mode.size.y;
    if (w <= 0 || h <= 0) return;

    uint32_t bpp = eka2l1::epoc::get_bpp_from_display_mode(scr->dsa_disp_mode);
    uint32_t pitch = scr->screen_buffer_byte_width(scr->dsa_disp_mode);

    int ox = (320 > w) ? (320 - w) / 2 : 0;
    int oy = (240 > h) ? (240 - h) / 2 : 0;
    int copy_w = std::min(w, 320);
    int copy_h = std::min(h, 240);

    if (bpp == 12) {
        for (int y = 0; y < copy_h; ++y) {
            const std::uint16_t *src_row = reinterpret_cast<const std::uint16_t *>(src + y * pitch);
            std::uint16_t *dst_row = s_fb + (oy + y) * 320 + ox;
            for (int x = 0; x < copy_w; ++x) {
                const std::uint16_t pixel = src_row[x];
                const std::uint16_t r = (pixel >> 8) & 0xF;
                const std::uint16_t g = (pixel >> 4) & 0xF;
                const std::uint16_t b = pixel & 0xF;
                dst_row[x] = static_cast<std::uint16_t>(((r * 31 / 15) << 11)
                    | ((g * 63 / 15) << 5) | (b * 31 / 15));
            }
        }
    } else if (bpp == 16) {
        for (int y = 0; y < copy_h; ++y) {
            const uint16_t *src_row = reinterpret_cast<const uint16_t *>(src + y * pitch);
            uint16_t *dst_row = s_fb + (oy + y) * 320 + ox;
            std::memcpy(dst_row, src_row, copy_w * sizeof(uint16_t));
        }
    } else if (bpp == 24) {
        for (int y = 0; y < copy_h; ++y) {
            const std::uint8_t *src_row = src + y * pitch;
            std::uint16_t *dst_row = s_fb + (oy + y) * 320 + ox;
            for (int x = 0; x < copy_w; ++x) {
                const std::uint8_t *pixel = src_row + x * 3;
                dst_row[x] = static_cast<std::uint16_t>(((pixel[2] >> 3) << 11)
                    | ((pixel[1] >> 2) << 5) | (pixel[0] >> 3));
            }
        }
    } else if (bpp == 32) {
        for (int y = 0; y < copy_h; ++y) {
            const uint32_t *src_row = reinterpret_cast<const uint32_t *>(src + y * pitch);
            uint16_t *dst_row = s_fb + (oy + y) * 320 + ox;
            for (int x = 0; x < copy_w; ++x) {
                uint32_t p = src_row[x];
                uint16_t r = (p >> 19) & 0x1F;
                uint16_t g = (p >> 10) & 0x3F;
                uint16_t b = (p >> 3) & 0x1F;
                dst_row[x] = (r << 11) | (g << 5) | b;
            }
        }
    }

    s_frame_drawn = true;
}

static void register_screen_callbacks() {
    if (!s_winserv) return;
    eka2l1::epoc::screen *screens = s_winserv->get_screens();
    while (screens) {
        screens->add_screen_redraw_callback(nullptr, on_screen_redraw);
        screens = screens->next;
    }
}

static void poll_and_send_input() {
    if (!s_winserv || !s_input_poll_cb || !s_input_state_cb) return;
    s_input_poll_cb();

    for (auto &binding : s_key_bindings) {
        bool pressed = s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, binding.retro_id) != 0;
        if (pressed != binding.prev_state) {
            binding.prev_state = pressed;
            // xlog("[LIBRETRO] Key retro_id=%u epoc=0x%x pressed=%d\n", binding.retro_id, binding.epoc_code, (int)pressed);
            eka2l1::drivers::input_event evt;
            evt.type_ = eka2l1::drivers::input_event_type::key_raw;
            evt.key_.state_ = pressed ? eka2l1::drivers::key_state::pressed : eka2l1::drivers::key_state::released;
            evt.key_.code_ = binding.epoc_code;
            s_winserv->queue_input_from_driver(evt);
        }
    }
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    s_environ_cb = cb;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    s_video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    s_audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    s_audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    s_input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    s_input_state_cb = cb;
}

RETRO_API void retro_init(void) {
    xlog("[EKA2L1] retro_init called\n");
    std::memset(s_fb, 0, sizeof(s_fb));
}

RETRO_API void retro_deinit(void) {
    s_symsys.reset();
    s_graphics_driver.reset();
    s_audio_driver.reset();
    s_app_settings.reset();
    s_winserv = nullptr;
    s_game_loaded = false;
}

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "EKA2L1";
    info->library_version = "0.0.1";
    info->valid_extensions = "app|sis|sisx|ngage";
    info->need_fullpath = true;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    std::memset(info, 0, sizeof(*info));
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
    info->geometry.base_width = 320;
    info->geometry.base_height = 240;
    info->geometry.max_width = 320;
    info->geometry.max_height = 240;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

RETRO_API void retro_reset(void) {
}

RETRO_API void retro_run(void) {
    static std::uint64_t s_run_count = 0;

    poll_and_send_input();

#if defined(SF2000) || defined(GB300V2)
    // std::thread is stubbed on this port, so ntimer has no thread of its own
    // and nothing would wake guest threads sleeping on timer events. Pump the
    // timers once per frame here: doing it inside every emulation loop
    // iteration instead multiplies an expensive baremetal clock read by the
    // thousands of iterations an idle frame can run.
    if (s_symsys) {
        if (eka2l1::kernel_system *kern = s_symsys->get_kernel_system()) {
            if (eka2l1::ntimer *timer = kern->get_ntimer()) {
                timer->advance();
            }
        }
    }
#endif

    s_frame_drawn = false;
    int loops = 0;

    while (!s_frame_drawn && loops < 5000) {
        if (!s_symsys || s_symsys->loop() == 0) {
            break;
        }

        loops++;

        // Nothing runnable: stop spinning until the next frame. The timer was
        // already advanced above, so sleepers wake up on the next retro_run
        // instead of burning thousands of idle iterations per frame.
        if (!s_frame_drawn && !s_symsys->get_kernel_system()->crr_thread()) {
            break;
        }
    }

    s_run_count++;

    // Verify the chunk host-buffer canaries while memory is still healthy: the
    // first slot that is broken identifies the code that wrote past its buffer.
#if defined(SF2000) || defined(GB300V2)
    if (s_run_count <= 10 || (s_run_count % 300) == 0) {
        eka2l1::mem::multiple_mem_model_chunk::check_all_guards("heartbeat");
    }
#endif

    // Heartbeat: tells whether the guest is still executing (loops) and whether
    // anything is actually being drawn (nonzero pixels / content hash), which a
    // black screen alone cannot distinguish.
    if (s_run_count <= 5 || (s_run_count % 300) == 0) {
        std::uint32_t hash = 2166136261u;
        std::uint32_t nonzero = 0;

        for (std::size_t i = 0; i < 320 * 240; i++) {
            hash = (hash ^ s_fb[i]) * 16777619u;
            if (s_fb[i]) {
                nonzero++;
            }
        }

        xlog("[EKA2L1] heartbeat frame=%u loops=%d drawn=%d nonzero=%u hash=0x%08X thread=%s\n",
            static_cast<unsigned>(s_run_count), loops, static_cast<int>(s_frame_drawn),
            static_cast<unsigned>(nonzero), hash,
            (s_symsys && s_symsys->get_kernel_system()->crr_thread())
                ? s_symsys->get_kernel_system()->crr_thread()->name().c_str()
                : "?");
    }

    if (s_video_cb) {
        s_video_cb(s_fb, 320, 240, 320 * sizeof(uint16_t));
    }

    if (s_audio_driver) {
        static_cast<libretro_audio_driver *>(s_audio_driver.get())->pump_all(735);
    }
}

RETRO_API size_t retro_serialize_size(void) {
    return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

RETRO_API void retro_cheat_reset(void) {
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    xlog("[EKA2L1] retro_load_game: path=%s\n", (game && game->path) ? game->path : "NULL");
    std::memset(s_fb, 0, sizeof(s_fb));
    s_frame_drawn = false;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (s_environ_cb) {
        s_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    }

    std::string root_dir;
    if (game && game->path && std::strlen(game->path) > 0) {
        std::string p = game->path;
        auto pos = p.find("/data/drives/");
        if (pos != std::string::npos) {
            root_dir = p.substr(0, pos);
        }
    }

    if (root_dir.empty()) {
        const char *sys_dir = nullptr;
        if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
            std::string candidate = std::string(sys_dir) + "/eka2l1";
            if (eka2l1::common::exists(candidate + "/config.yml") || eka2l1::common::exists(candidate + "/data")) {
                root_dir = candidate;
            }
        }
    }

    if (root_dir.empty()) {
        const char *candidates[] = {
            "/mnt/sda1/eka2l1",
            "/mnt/sda1/ROMS/eka2l1",
            "sdcard/eka2l1",
            "/home/Sajnaps/gb300/eka2l1"
        };
        for (const char *path : candidates) {
            if (eka2l1::common::exists(std::string(path) + "/config.yml") || eka2l1::common::exists(std::string(path) + "/data")) {
                root_dir = path;
                break;
            }
        }
    }

    if (root_dir.empty()) {
        root_dir = "/mnt/sda1/eka2l1";
    }

    eka2l1::set_runtime_resource_root(root_dir);
    xlog("[EKA2L1] root_dir=%s, storage=%s\n", root_dir.c_str(), (root_dir + "/data").c_str());

    std::string config_path = root_dir + "/config.yml";
    if (eka2l1::common::exists(config_path)) {
        s_conf.deserialize();
    }

    if (!eka2l1::already_setup) {
        eka2l1::log::setup_log(nullptr);
    }
    if (eka2l1::log::filterings) {
        eka2l1::log::filterings->parse_filter_string(s_conf.log_filter);
    }

    s_conf.storage = root_dir + "/data";
    s_conf.device = 0;
    s_conf.cpu_load_save = false;
    s_conf.screen_buffer_sync = eka2l1::config::screen_buffer_sync_option_on;
    s_conf.nearest_neighbor_filtering = true;
    s_conf.cpu_backend = "dyncom";
    s_conf.midi_backend = eka2l1::config::MIDI_BACKEND_TSF;
    s_conf.sf2_bank_path = "resources/defaultbank.sf2";
    s_conf.hsb_bank_path = "resources/defaultbank.hsb";

    s_app_settings = std::make_unique<eka2l1::config::app_settings>(&s_conf);

    eka2l1::system_create_components comp;
    s_audio_driver = std::make_unique<libretro_audio_driver>(s_conf.audio_master_volume,
        eka2l1::drivers::player_type_tsf);
    comp.audio_ = s_audio_driver.get();
    comp.graphics_ = nullptr;
    comp.conf_ = &s_conf;
    comp.settings_ = s_app_settings.get();

    xlog("[EKA2L1] creating system...\n");
    s_symsys = std::make_unique<eka2l1::system>(comp);
    xlog("[EKA2L1] creating graphics driver...\n");
    s_graphics_driver = std::make_unique<eka2l1::drivers::libretro_graphics_driver>();
    s_graphics_driver->run();
    s_symsys->set_graphics_driver(s_graphics_driver.get());

    xlog("[EKA2L1] starting up symsys...\n");
    s_symsys->startup();
    xlog("[EKA2L1] symsys startup done, setting device %d\n", s_conf.device);
    if (!s_symsys->set_device(s_conf.device)) {
        xlog("[EKA2L1] ERROR: set_device %d failed!\n", s_conf.device);
        return false;
    }
    xlog("[EKA2L1] set_device ok, mounting drives\n");

    s_symsys->mount(drive_c, drive_media::physical, eka2l1::add_path(s_conf.storage, "/drives/c/"), io_attrib_internal);
    s_symsys->mount(drive_d, drive_media::physical, eka2l1::add_path(s_conf.storage, "/drives/d/"), io_attrib_internal);
    s_symsys->mount(drive_e, drive_media::physical, eka2l1::add_path(s_conf.storage, "/drives/e/"), io_attrib_removeable);
    s_symsys->mount(drive_z, drive_media::rom,      eka2l1::add_path(s_conf.storage, "/drives/z/"), io_attrib_internal | io_attrib_write_protected);

    xlog("[EKA2L1] drives mounted, initializing user parties\n");
    s_symsys->initialize_user_parties();

    // Boot services are started asynchronously by initialize_user_parties(). Give
    // them a few scheduler ticks before launching a game; otherwise applications
    // that open DBMS/shared-data sessions during startup race the boot servers.
    for (int i = 0; i < 10000 && s_symsys->loop() != 0; ++i) {
    }

    s_winserv = reinterpret_cast<eka2l1::window_server *>(
        s_symsys->get_kernel_system()->get_by_name<eka2l1::service::server>(
            eka2l1::get_winserv_name_by_epocver(s_symsys->get_symbian_version_use())));

    register_screen_callbacks();

    s_symsys->add_system_reset_callback([](eka2l1::system *the_sys) {
        s_winserv = reinterpret_cast<eka2l1::window_server *>(
            the_sys->get_kernel_system()->get_by_name<eka2l1::service::server>(
                eka2l1::get_winserv_name_by_epocver(s_symsys->get_symbian_version_use())));
        register_screen_callbacks();
    });

    eka2l1::manager::packages *pkgmngr = s_symsys->get_packages();
    if (pkgmngr) {
        pkgmngr->load_registries();
        pkgmngr->migrate_legacy_registries();
    }

    std::u16string app_to_load = u"E:\\system\\apps\\6r48\\6r48.app";
    if (game && game->path && std::strlen(game->path) > 0) {
        std::string gpath = game->path;
        const std::string lower_path = eka2l1::common::lowercase_string(gpath);
        auto pos = lower_path.find("/system/apps/");
        if (pos != std::string::npos) {
            char drv = 'E';
            if (lower_path.find("/drives/c/") != std::string::npos) drv = 'C';
            else if (lower_path.find("/drives/z/") != std::string::npos) drv = 'Z';
            std::string rel = gpath.substr(pos + 1);
            std::string sym_path;
            sym_path += drv;
            sym_path += ":\\";
            for (char c : rel) {
                if (c == '/') sym_path += '\\';
                else sym_path += c;
            }
            app_to_load = eka2l1::common::utf8_to_ucs2(sym_path);
        } else if (lower_path.rfind(".app") != std::string::npos) {
            std::string fname = eka2l1::filename(gpath);
            std::string name_no_ext = fname;
            auto dot = name_no_ext.rfind('.');
            if (dot != std::string::npos) {
                name_no_ext = name_no_ext.substr(0, dot);
            }
            if (lower_path.find("call") != std::string::npos || lower_path.find("duty") != std::string::npos || lower_path.find("cod") != std::string::npos || lower_path.find("6r48") != std::string::npos) {
                app_to_load = u"E:\\system\\apps\\6r48\\6r48.app";
            } else {
                std::string sym_path = "E:\\system\\apps\\" + name_no_ext + "\\" + fname;
                app_to_load = eka2l1::common::utf8_to_ucs2(sym_path);
            }
        }
    }

    xlog("[EKA2L1] app_to_load=%s\n", eka2l1::common::ucs2_to_utf8(app_to_load).c_str());

    bool launched = false;
    eka2l1::kernel_system *kern = s_symsys->get_kernel_system();
    kern->register_thread_kill_callback([](eka2l1::kernel::thread *target, const std::string &category, const std::int32_t reason) {
        xlog("[LIBRETRO] Thread killed: %s cat=%s reason=%d\n", target ? target->name().c_str() : "null", category.c_str(), reason);
    });
    eka2l1::applist_server *app_svr = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

    xlog("[EKA2L1-TRACE] applist server found=%d\n", app_svr != nullptr);
    if (app_svr) {
        xlog("[EKA2L1-TRACE] get_registerations...\n");
        std::vector<eka2l1::apa_app_registry> &regs = app_svr->get_registerations();
        xlog("[EKA2L1-TRACE] registry loop begin count=%u\n", static_cast<unsigned>(regs.size()));

        for (std::size_t ri = 0; ri < regs.size(); ri++) {
            eka2l1::apa_app_registry &reg = regs[ri];
            std::u16string reg_path = reg.mandatory_info.app_path.to_std_string(nullptr);
            xlog("[EKA2L1-TRACE] reg[%u] uid=0x%x path=%s\n", static_cast<unsigned>(ri),
                static_cast<unsigned>(reg.mandatory_info.uid), eka2l1::common::ucs2_to_utf8(reg_path).c_str());

            if (eka2l1::common::compare_ignore_case(reg_path, app_to_load) == 0) {
                eka2l1::epoc::apa::command_line cmdline;
                cmdline.launch_cmd_ = eka2l1::epoc::apa::command_open;
                xlog("[EKA2L1-TRACE] registry matched uid=0x%x, calling launch_app\n",
                    static_cast<unsigned>(reg.mandatory_info.uid));
                launched = app_svr->launch_app(reg, cmdline, nullptr, nullptr);
                xlog("[EKA2L1-TRACE] launch_app returned %d\n", static_cast<int>(launched));
                break;
            }
        }

        xlog("[EKA2L1-TRACE] registry loop done\n");
    }

    if (!launched) {
        xlog("[EKA2L1] fallback: loading app directly via symsys->load\n");
        launched = s_symsys->load(app_to_load, u"");
        xlog("[EKA2L1-TRACE] fallback load returned %d\n", static_cast<int>(launched));
    }

    xlog("[EKA2L1] app launch result: %d\n", (int)launched);

    if (!launched) {
        return false;
    }

    s_game_loaded = true;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void) {
    s_symsys.reset();
    s_graphics_driver.reset();
    s_audio_driver.reset();
    s_app_settings.reset();
    s_winserv = nullptr;
    s_game_loaded = false;
}

RETRO_API unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id) {
    (void)id;
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}

namespace eka2l1::common {
    bool launch_browser(const std::string &) {
        return false;
    }
}

namespace eka2l1::drivers::ui {
    bool open_input_view(const std::u16string &, const int, input_dialog_complete_callback) {
        return false;
    }

    void close_input_view() {
    }

    void show_yes_no_dialog(const std::u16string &, const std::u16string &, const std::u16string &,
        yes_no_dialog_complete_callback complete_callback) {
        if (complete_callback) {
            complete_callback(1);
        }
    }
}
