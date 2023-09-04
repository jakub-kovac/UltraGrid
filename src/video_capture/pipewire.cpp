/**
 * @file   video_capture/pipewire.cpp
 * @author Matej Hrica       <492778@mail.muni.cz>
 * @author Martin Piatka     <piatka@cesnet.cz>
 */
/*
 * Copyright (c) 2023 CESNET z.s.p.o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <fstream>
#include <algorithm>
#include <vector>
#include <mutex>
#include <unistd.h>
#include <pipewire/pipewire.h>
#include <pipewire/version.h>
#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>

#include "tv.h"
#include "debug.h"
#include "lib_common.h"
#include "utils/color_out.h"
#include "utils/misc.h"
#include "utils/dbus_portal.hpp"
#include "utils/synchronized_queue.h"
#include "utils/profile_timer.hpp"
#include "video.h"
#include "video_capture.h"
#include "pipewire_common.hpp"
#include "pixfmt_conv.h"

#define MOD_NAME "[screen_pw] "

static constexpr int DEFAULT_BUFFERS_PW = 2;
static constexpr int MIN_BUFFERS_PW = 2;
static constexpr int MAX_BUFFERS_PW = 10;
static constexpr int QUEUE_SIZE = 3;
static constexpr int DEFAULT_EXPECTING_FPS = 30;

struct frame_deleter{ void operator()(video_frame *f){ vf_free(f); } };
using unique_frame = std::unique_ptr<video_frame, frame_deleter>;

struct screen_cast_session { 
        // used exlusively by ultragrid thread
        unique_frame in_flight_frame;

        std::mutex mut;
        std::vector<unique_frame> blank_frames;
        synchronized_queue<unique_frame, QUEUE_SIZE> sending_frames;

        video_desc desc = {};


        ScreenCastPortal portal;

        struct {
                bool show_cursor = false;
                std::string restore_file = "";
                uint32_t fps = 0;
                bool crop = true;
        } user_options;

        struct pw_{
                pw_() {
                        pw_init(&uv_argc, &uv_argv);
                }
                
                int fd = -1;
                uint32_t node = -1;
                
                struct pw_thread_loop *loop = nullptr;
                struct pw_context *context = nullptr;

                struct pw_stream *stream = nullptr;
                struct spa_hook stream_listener = {};

                struct spa_video_info format = {};

                spa_video_format video_format() {
                        return format.info.raw.format;
                }

                ~pw_() {
                        if(loop != nullptr){
                                pw_thread_loop_stop(loop);
                                if(stream != nullptr)
                                        pw_stream_destroy(stream);
                                if(context != nullptr)
                                        pw_context_destroy(context);
                                if(loop != nullptr)
                                        pw_thread_loop_destroy(loop);
                        }
                        if( fd > 0)
                                close(fd);
                }
        }pw;
};

static void on_stream_state_changed(void *session_ptr, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
        (void) session_ptr;
        LOG(LOG_LEVEL_INFO) << MOD_NAME "stream state changed \"" << pw_stream_state_as_string(old) 
                                                << "\" -> \""<<pw_stream_state_as_string(state)<<"\"\n";
        
        if (error != nullptr) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME "stream error: '"<< error << "'\n";
        }
}


static void on_stream_param_changed(void *session_ptr, uint32_t id, const struct spa_pod *param) {
        auto &session = *static_cast<screen_cast_session*>(session_ptr);
        LOG(LOG_LEVEL_VERBOSE) << MOD_NAME "param changed:\n";

        if (param == nullptr || id != SPA_PARAM_Format)
                return;

        int parse_format_ret = spa_format_parse(param, &session.pw.format.media_type, &session.pw.format.media_subtype);
        assert(parse_format_ret > 0);

        if(session.pw.format.media_type != SPA_MEDIA_TYPE_video
                        || session.pw.format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Format not video/raw!\n");
                return;
        }

        auto& raw_format = session.pw.format.info.raw;
        spa_format_video_raw_parse(param, &raw_format);

        log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Got format: %s\n", spa_debug_type_find_name(spa_type_video_format, raw_format.format));

        session.desc.width = raw_format.size.width;
        session.desc.height = raw_format.size.height;
        if(raw_format.framerate.num != 0){
                log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Got framerate: %d / %d\n", raw_format.framerate.num, raw_format.framerate.denom);
                session.desc.fps = static_cast<double>(raw_format.framerate.num) / raw_format.framerate.denom;
        } else {
                //Variable framerate
                log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Got variable framerate: %d / %d\n", raw_format.max_framerate.num, raw_format.max_framerate.denom);
                if(raw_format.max_framerate.num != 0){
                        session.desc.fps = static_cast<double>(raw_format.max_framerate.num) / raw_format.max_framerate.denom;
                } else {
                        session.desc.fps = 60;
                        log_msg(LOG_LEVEL_WARNING, MOD_NAME "Invalid max framerate, using %f instead\n", session.desc.fps);
                }
        }
        session.desc.color_spec = uv_codec_from_pw_fmt(raw_format.format);
        session.desc.interlacing = PROGRESSIVE;
        session.desc.tile_count = 1;

        log_msg(LOG_LEVEL_VERBOSE, MOD_NAME "size: %dx%d\n", session.desc.width, session.desc.height);

        int linesize = vc_get_linesize(session.desc.width, session.desc.color_spec);
        int32_t size = linesize * session.desc.height;

        uint8_t params_buffer[1024];

        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
        const struct spa_pod *params[3] = {};
        int n_params = 0;

        params[n_params++] = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers,
                SPA_POD_CHOICE_RANGE_Int(DEFAULT_BUFFERS_PW, MIN_BUFFERS_PW, MAX_BUFFERS_PW),
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
                SPA_PARAM_BUFFERS_stride, SPA_POD_Int(linesize),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr)))
        );
        
        if(session.user_options.crop) {
                params[n_params++] = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
                        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                        SPA_PARAM_META_size,
                        SPA_POD_Int(sizeof(struct spa_meta_region)))
                );
        }
        
        pw_stream_update_params(session.pw.stream, params, n_params);
}

static void pw_frame_to_uv_frame_memcpy(video_frame *dst, spa_buffer *src, spa_video_format fmt, spa_rectangle size, const spa_region *crop){
        auto offset = src->datas[0].chunk->offset;
        auto chunk_size = src->datas[0].chunk->size;
        auto stride = src->datas[0].chunk->stride;

        auto width = size.width;
        auto height = size.height;

        unsigned start_x = 0;
        unsigned start_y = 0;
        if(crop){
                width = crop->size.width;
                height = crop->size.height;
                start_x = crop->position.x;
                start_y = crop->position.y;
        }

        if(stride == 0)
                stride = chunk_size / size.height;

        auto linesize = vc_get_linesize(width, dst->color_spec);
        auto skip = vc_get_linesize(start_x, dst->color_spec);
        bool swap_red_blue = fmt == SPA_VIDEO_FORMAT_BGRA || fmt == SPA_VIDEO_FORMAT_BGRx; //TODO
        for(unsigned i = 0; i < height; i++){
                auto src_p = static_cast<unsigned char *>(src->datas[0].data) + offset + skip + stride * (i + start_y);
                auto dst_p = reinterpret_cast<unsigned char *>(dst->tiles[0].data) + linesize * i;
                /* It would probably be better to have separate functions for
                 * pipweire to uv frame conversions like lavd has. For now,
                 * let's handle BGRA to RGBA like this.
                 */
                if(swap_red_blue)
                        vc_copylineRGBA(dst_p, src_p, linesize, 16, 8, 0);
                else
                        memcpy(dst_p, src_p, linesize);
        }

        dst->tiles[0].width = width;
        dst->tiles[0].height = height;
        dst->tiles[0].data_len = linesize * height;
}

static void on_process(void *session_ptr) {
        using namespace std::chrono_literals;
        PROFILE_FUNC;

        screen_cast_session &session = *static_cast<screen_cast_session*>(session_ptr);
        pw_buffer *buffer;
        [[maybe_unused]] int n_buffers_from_pw = 0;
        while((buffer = pw_stream_dequeue_buffer(session.pw.stream)) != nullptr){    
                ++n_buffers_from_pw;

                unique_frame next_frame;
                
                assert(buffer->buffer != nullptr);
                assert(buffer->buffer->datas != nullptr);
                assert(buffer->buffer->n_datas == 1);
                assert(buffer->buffer->datas[0].data != nullptr);

                if(buffer->buffer->datas[0].chunk == nullptr || buffer->buffer->datas[0].chunk->size == 0) {
                        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "dropping - empty pw frame " << "\n";
                        pw_stream_queue_buffer(session.pw.stream, buffer);
                        continue;
                }

                {
                        std::lock_guard<std::mutex> lock(session.mut);
                        if(!session.blank_frames.empty()){
                                next_frame = std::move(session.blank_frames.back());
                                session.blank_frames.pop_back();
                        }
                }

                if(!next_frame) {
                        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "dropping frame (no blank frames)\n";
                        pw_stream_queue_buffer(session.pw.stream, buffer);
                        continue;
                }

                spa_region *crop_region = nullptr;
                if (session.user_options.crop) {
                        spa_meta_region *meta_crop_region = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_VideoCrop, sizeof(*meta_crop_region)));
                        if (meta_crop_region != nullptr && spa_meta_region_is_valid(meta_crop_region))
                           crop_region = &meta_crop_region->region;
                }

                if(crop_region){
                        //Update desc so that we don't reallocate on each frame
                        //TODO: Figure what to do when we can't actually crop (MJPEG)
                        session.desc.width = crop_region->size.width;
                        session.desc.height = crop_region->size.height;
                }

                if(!next_frame || !video_desc_eq(video_desc_from_frame(next_frame.get()), session.desc)){
                        log_msg(LOG_LEVEL_VERBOSE, MOD_NAME "Desc changed, allocating new video_frame\n");
                        next_frame.reset(vf_alloc_desc_data(session.desc));
                }


                auto& raw_format = session.pw.format.info.raw;

                //copy_frame(session.pw.video_format(), buffer->buffer, next_frame.get(), session.desc.width, session.desc.height, crop_region);
                pw_frame_to_uv_frame_memcpy(next_frame.get(), buffer->buffer, raw_format.format, raw_format.size, crop_region);

                session.sending_frames.push(std::move(next_frame));
                pw_stream_queue_buffer(session.pw.stream, buffer);
        }
        
        //LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: from pw: "<< n_buffers_from_pw << "\t sending: "<<session.sending_frames.size_approx() << "\t blank: " << session.blank_frames.size_approx() << "\n";
        
}

static void on_drained(void*)
{
        LOG(LOG_LEVEL_VERBOSE) << MOD_NAME "pipewire: drained\n";
}

static void on_add_buffer(void *session_ptr, struct pw_buffer *)
{
        (void) session_ptr;

        LOG(LOG_LEVEL_VERBOSE) << MOD_NAME "pipewire: add_buffer\n";
}

static void on_remove_buffer(void *session_ptr, struct pw_buffer *)
{
        (void) session_ptr;
        LOG(LOG_LEVEL_VERBOSE) << MOD_NAME "pipewire: remove_buffer\n";
}

static const struct pw_stream_events stream_events = {
                .version = PW_VERSION_STREAM_EVENTS,
                .destroy = nullptr,
                .state_changed = on_stream_state_changed,
                .control_info = nullptr,
                .io_changed = nullptr,
                .param_changed = on_stream_param_changed,
                .add_buffer = on_add_buffer,
                .remove_buffer = on_remove_buffer,
                .process = on_process,
                .drained = on_drained,
#if PW_MAJOR > 0 || PW_MINOR > 3 || (PW_MINOR == 3 && PW_MICRO > 39)
                .command = nullptr,
                .trigger_done = nullptr,
#endif
};

static int start_pipewire(screen_cast_session &session)
{    
        const struct spa_pod *params[2] = {};
        uint8_t params_buffer[1024];
        struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

        session.pw.loop = pw_thread_loop_new("pipewire_thread_loop", nullptr);
        assert(session.pw.loop != nullptr);
        pw_thread_loop_lock(session.pw.loop);
        session.pw.context = pw_context_new(pw_thread_loop_get_loop(session.pw.loop), nullptr, 0);
        assert(session.pw.context != nullptr);

        if (pw_thread_loop_start(session.pw.loop) != 0) {
                assert(false && "error starting pipewire thread loop");
        }

        pw_core *core = nullptr;

        if(session.pw.fd != -1)
                core = pw_context_connect_fd(session.pw.context, session.pw.fd, nullptr, 0); 
        else
                core = pw_context_connect(session.pw.context, nullptr, 0); 

        assert(core != nullptr);

        session.pw.stream = pw_stream_new(core, "my_screencast", pw_properties_new(
                        PW_KEY_MEDIA_TYPE, "Video",
                        PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Screen",
                        nullptr));
        assert(session.pw.stream != nullptr);
        pw_stream_add_listener(session.pw.stream, &session.pw.stream_listener, &stream_events, &session);

        auto size_rect_def = SPA_RECTANGLE(1920, 1080);
        auto size_rect_min = SPA_RECTANGLE(1, 1);
        auto size_rect_max = SPA_RECTANGLE(3840, 2160);

        auto framerate_def = SPA_FRACTION(session.user_options.fps > 0 ? session.user_options.fps : DEFAULT_EXPECTING_FPS, 1);
        auto framerate_min = SPA_FRACTION(0, 1);
        auto framerate_max = SPA_FRACTION(600, 1);

        const int n_params = 1;
        params[0] = static_cast<spa_pod *> (spa_pod_builder_add_object(
                        &pod_builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(7, 
                                SPA_VIDEO_FORMAT_UYVY,
                                SPA_VIDEO_FORMAT_RGB,
                                SPA_VIDEO_FORMAT_RGBA,
                                SPA_VIDEO_FORMAT_RGBx,
                                SPA_VIDEO_FORMAT_YUY2,
                                SPA_VIDEO_FORMAT_BGRA,
                                SPA_VIDEO_FORMAT_BGRx
                                ),
                        SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(
                                        &size_rect_def,
                                        &size_rect_min,
                                        &size_rect_max),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(
                                        &framerate_def,
                                        &framerate_min,
                                        &framerate_max)
        ));

        int res = pw_stream_connect(session.pw.stream,
                                        PW_DIRECTION_INPUT,
                                        session.pw.node,
                                        static_cast<pw_stream_flags>(
                                                PW_STREAM_FLAG_AUTOCONNECT |
                                                PW_STREAM_FLAG_MAP_BUFFERS |
                                                PW_STREAM_FLAG_DONT_RECONNECT
                                        ),
                                        params, n_params);
        if (res < 0) {
                fprintf(stderr, MOD_NAME "can't connect: %s\n", spa_strerror(res));
                return -1;
        }

        pw_stream_set_active(session.pw.stream, true);
        pw_thread_loop_unlock(session.pw.loop);
        return 0;
}

static void vidcap_screen_pw_probe(struct device_info **available_devices, int *count, void (**deleter)(void *))
{
        *deleter = free;
        *count = 1;
        *available_devices = (struct device_info *) calloc(*count, sizeof(struct device_info));
        // (*available_devices)[0].dev can be "" since screen cap. doesn't require parameters
        snprintf((*available_devices)[0].name, sizeof (*available_devices)[0].name, "Screen capture PipeWire");
}

static void show_help() {
        auto param = [](const char* name) -> std::ostream& {
                col() << "  " << SBOLD(name) << " - ";
                return std::cout;
        };

        std::cout << "Screen capture using PipeWire and ScreenCast freedesktop portal API\n";
        std::cout << "Usage: -t screen_pw[:cursor|:nocrop|:fps=<fps>|:restore=<token_file>]]\n";
        param("cursor") << "make the cursor visible (default hidden)\n";
        param("nocrop") << "when capturing a window do not crop out the empty background\n";
        param("<fps>") << "prefered FPS passed to PipeWire (PipeWire may ignore it)\n";
        param("<token_file>") << "restore the selected window/display from a file.\n\t\tIf not possible, display the selection dialog and save the token to the file specified.\n";
}


static int parse_params(struct vidcap_params *params, screen_cast_session &session) {
        if(const char *fmt = vidcap_params_get_fmt(params)) {        
                std::istringstream params_stream(fmt);
                
                std::string param;
                while (std::getline(params_stream, param, ':')) {
                        if (param == "help") {
                                        show_help();//session.init_error
                                        return VIDCAP_INIT_NOERR;
                        } else if (param == "cursor") {
                                session.user_options.show_cursor = true;
                        } else if (param == "nocrop") {
                                session.user_options.crop = false;
                        } else {
                                auto split_index = param.find('=');
                                if(split_index != std::string::npos && split_index != 0){
                                        std::string name = param.substr(0, split_index);
                                        std::string value = param.substr(split_index + 1);

                                        if (name == "fps" || name == "FPS"){
                                                std::istringstream is(value);
                                                is >> session.user_options.fps;
                                                continue;
                                        }else if(name == "restore"){
                                                session.user_options.restore_file = value;
                                                continue;
                                        }
                                }

                                LOG(LOG_LEVEL_ERROR) << MOD_NAME "invalid option: \"" << param << "\"\n";
                                return VIDCAP_INIT_FAIL;
                        }
                }
        }
        return VIDCAP_INIT_OK;
}

static int vidcap_screen_pw_init(struct vidcap_params *params, void **state)
{
        if (vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_ANY) {
                return VIDCAP_INIT_AUDIO_NOT_SUPPORTED;
        }

        screen_cast_session &session = *new screen_cast_session();
        *state = &session;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "init\n";
        
        int params_ok = parse_params(params, session);
        if(params_ok != VIDCAP_INIT_OK)
                return params_ok;

        for(int i = 0; i < QUEUE_SIZE; i++)
                session.blank_frames.emplace_back(vf_alloc(1));

        auto portalResult = session.portal.run(session.user_options.restore_file, session.user_options.show_cursor);
        
        if (portalResult.pipewire_fd == -1) {
                return VIDCAP_INIT_FAIL;
        }

        session.pw.fd = portalResult.pipewire_fd;
        session.pw.node = portalResult.pipewire_node;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "init ok\n";
        start_pipewire(session);
        return VIDCAP_INIT_OK;
}

static int vidcap_pw_init(struct vidcap_params *params, void **state)
{
        if (vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_ANY) {
                return VIDCAP_INIT_AUDIO_NOT_SUPPORTED;
        }

        screen_cast_session &session = *new screen_cast_session();
        *state = &session;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "init\n";
        
        int params_ok = parse_params(params, session);
        if(params_ok != VIDCAP_INIT_OK)
                return params_ok;

        for(int i = 0; i < QUEUE_SIZE; i++)
                session.blank_frames.emplace_back(vf_alloc(1));

        session.pw.fd = -1;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "init ok\n";
        start_pipewire(session);
        return VIDCAP_INIT_OK;
}

static void vidcap_screen_pw_done(void *session_ptr)
{
        LOG(LOG_LEVEL_DEBUG) << MOD_NAME "done\n";   
        delete static_cast<screen_cast_session*>(session_ptr);
}

static struct video_frame *vidcap_screen_pw_grab(void *session_ptr, struct audio_frame **audio)
{    
        PROFILE_FUNC;

        assert(session_ptr != nullptr);
        auto &session = *static_cast<screen_cast_session*>(session_ptr);
        *audio = nullptr;
   
        if(session.in_flight_frame.get() != nullptr){
                session.blank_frames.push_back(std::move(session.in_flight_frame));
        }

        using namespace std::chrono_literals;
        session.sending_frames.timed_pop(session.in_flight_frame, 500ms);
        return session.in_flight_frame.get();
}

static const struct video_capture_info vidcap_screen_pw_info = {
        vidcap_screen_pw_probe,
        vidcap_screen_pw_init,
        vidcap_screen_pw_done,
        vidcap_screen_pw_grab,
        MOD_NAME,
};

REGISTER_MODULE(screen_pw, &vidcap_screen_pw_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);

static const struct video_capture_info vidcap_pw_info = {
        vidcap_screen_pw_probe,
        vidcap_pw_init,
        vidcap_screen_pw_done,
        vidcap_screen_pw_grab,
        MOD_NAME,
};

REGISTER_MODULE(pipewire, &vidcap_pw_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);

/* vim: set expandtab sw=8: */
