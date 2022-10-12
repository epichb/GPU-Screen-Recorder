/*
    Copyright (C) 2020 dec05eba

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <map>
#include <signal.h>
#include <sys/stat.h>

#include <unistd.h>
#include <fcntl.h>

#include "../include/sound.hpp"
#include "../include/NvFBCLibrary.hpp"
#include "../include/CudaLibrary.hpp"
#include "../include/GlLibrary.hpp"

#include <X11/extensions/Xcomposite.h>
//#include <X11/Xatom.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <deque>
#include <future>

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int VIDEO_STREAM_INDEX = 0;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

static Cuda cuda;
static GlLibrary gl;

static char* av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}

struct ScopedGLXFBConfig {
    ~ScopedGLXFBConfig() {
        if (configs)
            XFree(configs);
    }

    GLXFBConfig *configs = nullptr;
};

struct WindowPixmap {
    Pixmap pixmap = None;
    GLXPixmap glx_pixmap = None;
    unsigned int texture_id = 0;
    unsigned int target_texture_id = 0;

    int texture_width = 0;
    int texture_height = 0;

    int texture_real_width = 0;
    int texture_real_height = 0;

    Window composite_window = None;
};

enum class VideoQuality {
    MEDIUM,
    HIGH,
    VERY_HIGH,
    ULTRA
};

enum class VideoCodec {
    H264,
    H265
};

static double clock_get_monotonic_seconds() {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static bool x11_supports_composite_named_window_pixmap(Display *dpy) {
    int extension_major;
    int extension_minor;
    if (!XCompositeQueryExtension(dpy, &extension_major, &extension_minor))
        return false;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(dpy, &major_version, &minor_version) &&
           (major_version > 0 || minor_version >= 2);
}

static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
#if 0
    char type_str[128];
    XGetErrorText(dpy, ev->type, type_str, sizeof(type_str));

    char major_opcode_str[128];
    XGetErrorText(dpy, ev->type, major_opcode_str, sizeof(major_opcode_str));

    char minor_opcode_str[128];
    XGetErrorText(dpy, ev->type, minor_opcode_str, sizeof(minor_opcode_str));

    fprintf(stderr,
        "X Error of failed request:  %s\n"
        "Major opcode of failed request:  %d (%s)\n"
        "Minor opcode of failed request:  %d (%s)\n"
        "Serial number of failed request:  %d\n",
            type_str,
            ev->request_code, major_opcode_str,
            ev->minor_code, minor_opcode_str);
#endif
    return 0;
}

static int x11_io_error_handler(Display *dpy) {
    return 0;
}

static Window get_compositor_window(Display *display) {
    Window overlay_window = XCompositeGetOverlayWindow(display, DefaultRootWindow(display));
    XCompositeReleaseOverlayWindow(display, DefaultRootWindow(display));

    /*
    Atom xdnd_proxy = XInternAtom(display, "XdndProxy", False);
    if(!xdnd_proxy)
        return None;

    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = nullptr;
    if(XGetWindowProperty(display, overlay_window, xdnd_proxy, 0, 1, False, XA_WINDOW, &type, &format, &nitems, &after, &data) != Success)
        return None;

    fprintf(stderr, "type: %ld, format: %d, num items: %lu\n", type, format, nitems);
    if(type == XA_WINDOW && format == 32 && nitems == 1)
        fprintf(stderr, "Proxy window: %ld\n", *(Window*)data);

    if(data)
        XFree(data);
    */

    Window root_window, parent_window;
    Window *children = nullptr;
    unsigned int num_children = 0;
    if(XQueryTree(display, overlay_window, &root_window, &parent_window, &children, &num_children) == 0)
        return None;

    Window compositor_window = None;
    if(num_children == 1) {
        compositor_window = children[0];
        const int screen_width = XWidthOfScreen(DefaultScreenOfDisplay(display));
        const int screen_height = XHeightOfScreen(DefaultScreenOfDisplay(display));

        XWindowAttributes attr;
        if(!XGetWindowAttributes(display, compositor_window, &attr) || attr.width != screen_width || attr.height != screen_height)
            compositor_window = None;
    }

    if(children)
        XFree(children);

    return compositor_window;
}

static void cleanup_window_pixmap(Display *dpy, WindowPixmap &pixmap) {
    if (pixmap.target_texture_id) {
        gl.glDeleteTextures(1, &pixmap.target_texture_id);
        pixmap.target_texture_id = 0;
    }

    if (pixmap.texture_id) {
        gl.glDeleteTextures(1, &pixmap.texture_id);
        pixmap.texture_id = 0;
        pixmap.texture_width = 0;
        pixmap.texture_height = 0;
        pixmap.texture_real_width = 0;
        pixmap.texture_real_height = 0;
    }

    if (pixmap.glx_pixmap) {
        gl.glXDestroyPixmap(dpy, pixmap.glx_pixmap);
        gl.glXReleaseTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT);
        pixmap.glx_pixmap = None;
    }

    if (pixmap.pixmap) {
        XFreePixmap(dpy, pixmap.pixmap);
        pixmap.pixmap = None;
    }

    if(pixmap.composite_window) {
        XCompositeUnredirectWindow(dpy, pixmap.composite_window, CompositeRedirectAutomatic);
        pixmap.composite_window = None;
    }
}

static bool recreate_window_pixmap(Display *dpy, Window window_id,
                                   WindowPixmap &pixmap, bool fallback_composite_window = true) {
    cleanup_window_pixmap(dpy, pixmap);

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, window_id, &attr)) {
        fprintf(stderr, "Failed to get window attributes\n");
        return false;
    }

    const int pixmap_config[] = {
        GLX_BIND_TO_TEXTURE_RGB_EXT, True,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT | GLX_WINDOW_BIT,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_BUFFER_SIZE, 24,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 0,
        // GLX_Y_INVERTED_EXT, (int)GLX_DONT_CARE,
        None};

    const int pixmap_attribs[] = {GLX_TEXTURE_TARGET_EXT,
                                  GLX_TEXTURE_2D_EXT,
                                  GLX_TEXTURE_FORMAT_EXT,
                                  GLX_TEXTURE_FORMAT_RGB_EXT,
                                  None};

    int c;
    GLXFBConfig *configs = gl.glXChooseFBConfig(dpy, 0, pixmap_config, &c);
    if (!configs) {
        fprintf(stderr, "Failed too choose fb config\n");
        return false;
    }
    ScopedGLXFBConfig scoped_configs;
    scoped_configs.configs = configs;

    bool found = false;
    GLXFBConfig config;
    for (int i = 0; i < c; i++) {
        config = configs[i];
        XVisualInfo *visual = gl.glXGetVisualFromFBConfig(dpy, config);
        if (!visual)
            continue;

        if (attr.depth != visual->depth) {
            XFree(visual);
            continue;
        }
        XFree(visual);
        found = true;
        break;
    }

    if(!found) {
        fprintf(stderr, "No matching fb config found\n");
        return false;
    }

    Pixmap new_window_pixmap = XCompositeNameWindowPixmap(dpy, window_id);
    if (!new_window_pixmap) {
        fprintf(stderr, "Failed to get pixmap for window %ld\n", window_id);
        return false;
    }

    GLXPixmap glx_pixmap = gl.glXCreatePixmap(dpy, config, new_window_pixmap, pixmap_attribs);
    if (!glx_pixmap) {
        fprintf(stderr, "Failed to create glx pixmap\n");
        XFreePixmap(dpy, new_window_pixmap);
        return false;
    }

    pixmap.pixmap = new_window_pixmap;
    pixmap.glx_pixmap = glx_pixmap;

    //glEnable(GL_TEXTURE_2D);
    gl.glGenTextures(1, &pixmap.texture_id);
    gl.glBindTexture(GL_TEXTURE_2D, pixmap.texture_id);

    // glEnable(GL_BLEND);
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl.glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    //glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &pixmap.texture_width);
    gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &pixmap.texture_height);

    pixmap.texture_real_width = pixmap.texture_width;
    pixmap.texture_real_height = pixmap.texture_height;

    if(pixmap.texture_width == 0 || pixmap.texture_height == 0) {
        gl.glBindTexture(GL_TEXTURE_2D, 0);        
        pixmap.texture_width = attr.width;
        pixmap.texture_height = attr.height;

        pixmap.texture_real_width = pixmap.texture_width;
        pixmap.texture_real_height = pixmap.texture_height;

        if(fallback_composite_window) {
            Window compositor_window = get_compositor_window(dpy);
            if(!compositor_window) {
                fprintf(stderr, "Warning: failed to get texture size. You are probably running an unsupported compositor and recording the selected window doesn't work at the moment. This could also happen if you are trying to record a window with client-side decorations. A black window will be displayed instead. A workaround is to record the whole monitor (which uses NvFBC).\n");
                return false;
            }

            fprintf(stderr, "Warning: failed to get texture size. You are probably trying to record a window with client-side decorations (using GNOME?). Trying to fallback to recording the compositor proxy window\n");
            XCompositeRedirectWindow(dpy, compositor_window, CompositeRedirectAutomatic);

            // TODO: Target texture should be the same size as the target window, not the size of the composite window
            if(recreate_window_pixmap(dpy, compositor_window, pixmap, false)) {
                pixmap.composite_window = compositor_window;
                pixmap.texture_width = attr.width;
                pixmap.texture_height = attr.height;
                return true;
            }

            pixmap.texture_width = attr.width;
            pixmap.texture_height = attr.height;

            return false;
        } else {
            fprintf(stderr, "Warning: failed to get texture size. You are probably running an unsupported compositor and recording the selected window doesn't work at the moment. This could also happen if you are trying to record a window with client-side decorations. A black window will be displayed instead. A workaround is to record the whole monitor (which uses NvFBC).\n");
        }
    }

    fprintf(stderr, "texture width: %d, height: %d\n", pixmap.texture_width,
           pixmap.texture_height);

    // Generating this second texture is needed because
    // cuGraphicsGLRegisterImage cant be used with the texture that is mapped
    // directly to the pixmap.
    // TODO: Investigate if it's somehow possible to use the pixmap texture
    // directly, this should improve performance since only less image copy is
    // then needed every frame.
    gl.glGenTextures(1, &pixmap.target_texture_id);
    gl.glBindTexture(GL_TEXTURE_2D, pixmap.target_texture_id);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixmap.texture_width,
                 pixmap.texture_height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    unsigned int err2 = gl.glGetError();
    //fprintf(stderr, "error: %d\n", err2);
    // glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    // glGenerateTextureMipmapEXT(glxpixmap, GL_TEXTURE_2D);

    // glGenerateMipmap(GL_TEXTURE_2D);

    // gl.glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    // gl.glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    //glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl.glBindTexture(GL_TEXTURE_2D, 0);

    return pixmap.texture_id != 0 && pixmap.target_texture_id != 0;
}

static Window create_opengl_window(Display *display) {
    const int attr[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 0,
        None
    };

    XVisualInfo *visual_info = NULL;
    GLXFBConfig fbconfig = NULL;

    int numfbconfigs = 0;
    GLXFBConfig *fbconfigs = gl.glXChooseFBConfig(display, DefaultScreen(display), attr, &numfbconfigs);
    for(int i = 0; i < numfbconfigs; i++) {
        visual_info = gl.glXGetVisualFromFBConfig(display, fbconfigs[i]);
        if(!visual_info)
            continue;

        fbconfig = fbconfigs[i];
        break;
    }

    if(!visual_info) {
        fprintf(stderr, "mgl error: no appropriate visual found\n");
        return -1;
    }

    // TODO: Core profile? GLX_CONTEXT_CORE_PROFILE_BIT_ARB.
    // TODO: Remove need for 4.2 when copy texture function has been removed
    int context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 2,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        None
    };

    GLXContext gl_context = gl.glXCreateContextAttribsARB(display, fbconfig, nullptr, True, context_attribs);
    if(!gl_context) {
        fprintf(stderr, "Error: failed to create gl context\n");
        return None;
    }

    Colormap colormap = XCreateColormap(display, DefaultRootWindow(display), visual_info->visual, AllocNone);
    if(!colormap) {
        fprintf(stderr, "Error: failed to create x11 colormap\n");
        gl.glXDestroyContext(display, gl_context);
    }

    XSetWindowAttributes window_attr;
    window_attr.colormap = colormap;

    // TODO: Is there a way to remove the need to create a window?
    Window window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, visual_info->depth, InputOutput, visual_info->visual, CWColormap, &window_attr);

    if(!window) {
        fprintf(stderr, "Error: failed to create gl window\n");
        goto fail;
    }

    if(!gl.glXMakeContextCurrent(display, window, window, gl_context)) {
        fprintf(stderr, "Error: failed to make gl context current\n");
        goto fail;
    }

    return window;

    fail:
    XFreeColormap(display, colormap);
    gl.glXDestroyContext(display, gl_context);
    return None;
}

/* TODO: check for glx swap control extension string (GLX_EXT_swap_control, etc) */
static void set_vertical_sync_enabled(Display *display, Window window, bool enabled) {     
    int result = 0;

    if(gl.glXSwapIntervalEXT) {
        gl.glXSwapIntervalEXT(display, window, enabled ? 1 : 0);
    } else if(gl.glXSwapIntervalMESA) {
        result = gl.glXSwapIntervalMESA(enabled ? 1 : 0);
    } else if(gl.glXSwapIntervalSGI) {
        result = gl.glXSwapIntervalSGI(enabled ? 1 : 0);
    } else {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "Warning: setting vertical sync not supported\n");
        }
    }

    if(result != 0)
        fprintf(stderr, "Warning: setting vertical sync failed\n");
}

// |stream| is only required for non-replay mode
static void receive_frames(AVCodecContext *av_codec_context, int stream_index, AVStream *stream, AVFrame *frame,
                           AVFormatContext *av_format_context,
                           double replay_start_time,
                           std::deque<AVPacket> &frame_data_queue,
                           int replay_buffer_size_secs,
                           bool &frames_erased,
						   std::mutex &write_output_mutex) {
    for (;;) {
        // TODO: Use av_packet_alloc instead because sizeof(av_packet) might not be future proof(?)
        AVPacket av_packet;
        memset(&av_packet, 0, sizeof(av_packet));
        av_packet.data = NULL;
        av_packet.size = 0;
        int res = avcodec_receive_packet(av_codec_context, &av_packet);
        if (res == 0) { // we have a packet, send the packet to the muxer
            av_packet.stream_index = stream_index;
            av_packet.pts = av_packet.dts = frame->pts;

			std::lock_guard<std::mutex> lock(write_output_mutex);
            if(replay_buffer_size_secs != -1) {
                double time_now = clock_get_monotonic_seconds();
                double replay_time_elapsed = time_now - replay_start_time;

                AVPacket new_pack;
                av_packet_move_ref(&new_pack, &av_packet);
                frame_data_queue.push_back(std::move(new_pack));
                if(replay_time_elapsed >= replay_buffer_size_secs) {
                    av_packet_unref(&frame_data_queue.front());
                    frame_data_queue.pop_front();
                    frames_erased = true;
                }
            } else {
                av_packet_rescale_ts(&av_packet, av_codec_context->time_base, stream->time_base);
                av_packet.stream_index = stream->index;
                int ret = av_write_frame(av_format_context, &av_packet);
                if(ret < 0) {
                    fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet.stream_index, av_error_to_string(ret), ret);
                }
            }
            av_packet_unref(&av_packet);
        } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                             // fprintf(stderr, "No packet!\n");
            av_packet_unref(&av_packet);
            break;
        } else if (res == AVERROR_EOF) { // this is the end of the stream
            fprintf(stderr, "End of stream!\n");
            av_packet_unref(&av_packet);
            break;
        } else {
            fprintf(stderr, "Unexpected error: %d\n", res);
            av_packet_unref(&av_packet);
            break;
        }
    }
}

static AVCodecContext* create_audio_codec_context(AVFormatContext *av_format_context, int fps) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(
            stderr,
            "Error: Could not find aac encoder\n");
        exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    assert(codec->type == AVMEDIA_TYPE_AUDIO);
    /*
    codec_context->sample_fmt = (*codec)->sample_fmts
                                    ? (*codec)->sample_fmts[0]
                                    : AV_SAMPLE_FMT_FLTP;
    */
	codec_context->codec_id = AV_CODEC_ID_AAC;
    codec_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    //codec_context->bit_rate = 64000;
    codec_context->sample_rate = 48000;
    codec_context->profile = FF_PROFILE_AAC_LOW;
#if LIBAVCODEC_VERSION_MAJOR < 60
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;
#else
    av_channel_layout_default(&codec_context->ch_layout, 2);
#endif

    codec_context->time_base.num = 1;
    codec_context->time_base.den = codec_context->sample_rate;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;

    av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static const AVCodec* find_h264_encoder() {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if(!codec)
        codec = avcodec_find_encoder_by_name("nvenc_h264");
    return codec;
}

static const AVCodec* find_h265_encoder() {
    const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if(!codec)
        codec = avcodec_find_encoder_by_name("nvenc_hevc");
    return codec;
}

static AVCodecContext *create_video_codec_context(AVFormatContext *av_format_context, 
                            VideoQuality video_quality,
                            int record_width, int record_height,
                            int fps, const AVCodec *codec, bool is_livestream) {

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    //double fps_ratio = (double)fps / 30.0;

    assert(codec->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = codec->id;
    codec_context->width = record_width & ~1;
    codec_context->height = record_height & ~1;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = fps;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;
    codec_context->sample_aspect_ratio.num = 0;
    codec_context->sample_aspect_ratio.den = 0;
    // High values reeduce file size but increases time it takes to seek
    if(is_livestream) {
        codec_context->flags |= (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
        codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
        //codec_context->gop_size = std::numeric_limits<int>::max();
        //codec_context->keyint_min = std::numeric_limits<int>::max();
        codec_context->gop_size = fps * 2;
    } else {
        codec_context->gop_size = fps * 2;
    }
    codec_context->max_b_frames = 0;
    codec_context->pix_fmt = AV_PIX_FMT_CUDA;
    codec_context->color_range = AVCOL_RANGE_JPEG;
    if(codec->id == AV_CODEC_ID_HEVC)
        codec_context->codec_tag = MKTAG('h', 'v', 'c', '1');
    switch(video_quality) {
        case VideoQuality::MEDIUM:
            //codec_context->qmin = 35;
            //codec_context->qmax = 35;
            codec_context->bit_rate = 100000;//4500000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::HIGH:
            //codec_context->qmin = 34;
            //codec_context->qmax = 34;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::VERY_HIGH:
            //codec_context->qmin = 28;
            //codec_context->qmax = 28;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::ULTRA:
            //codec_context->qmin = 22;
            //codec_context->qmax = 22;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
    }
    //codec_context->profile = FF_PROFILE_H264_MAIN;
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    // stream->time_base = codec_context->time_base;
    // codec_context->ticks_per_frame = 30;
    //av_opt_set(codec_context->priv_data, "tune", "hq", 0);
    // TODO: Do this for better file size? also allows setting qmin, qmax per frame? which can then be used to dynamically set bitrate to reduce quality
    // if live streaming is slow or if the users harddrive is cant handle writing megabytes of data per second.
    #if 0
    char qmin_str[32];
    snprintf(qmin_str, sizeof(qmin_str), "%d", codec_context->qmin);

    char qmax_str[32];
    snprintf(qmax_str, sizeof(qmax_str), "%d", codec_context->qmax);

    av_opt_set(codec_context->priv_data, "cq", qmax_str, 0);
    av_opt_set(codec_context->priv_data, "rc", "vbr", 0);
    av_opt_set(codec_context->priv_data, "qmin", qmin_str, 0);
    av_opt_set(codec_context->priv_data, "qmax", qmax_str, 0);
    codec_context->bit_rate = 0;
    #endif

    //codec_context->rc_max_rate = codec_context->bit_rate;
    //codec_context->rc_min_rate = codec_context->bit_rate;
    //codec_context->rc_buffer_size = codec_context->bit_rate / 10;

    av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static AVFrame* open_audio(AVCodecContext *audio_codec_context) {
    int ret;
    ret = avcodec_open2(audio_codec_context, audio_codec_context->codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        exit(1);
    }

    frame->nb_samples = audio_codec_context->frame_size;
    frame->format = audio_codec_context->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR < 60
    frame->channels = audio_codec_context->channels;
    frame->channel_layout = audio_codec_context->channel_layout;
#else
    av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout);
#endif

    ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    return frame;
}

#if LIBAVUTIL_VERSION_MAJOR < 57
static AVBufferRef* dummy_hw_frame_init(int size) {
    return av_buffer_alloc(size);
}
#else
static AVBufferRef* dummy_hw_frame_init(size_t size) {
    return av_buffer_alloc(size);
}
#endif

static void open_video(AVCodecContext *codec_context,
                       WindowPixmap &window_pixmap, AVBufferRef **device_ctx,
                       CUgraphicsResource *cuda_graphics_resource, CUcontext cuda_context, bool use_nvfbc, VideoQuality video_quality, bool is_livestream, bool very_old_gpu) {
    int ret;

    *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!*device_ctx) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        exit(1);
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext *)(*device_ctx)->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext *)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cuda_context;
    if(av_hwdevice_ctx_init(*device_ctx) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        exit(1);
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(*device_ctx);
    if (!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        exit(1);
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = codec_context->width;
    hw_frame_context->height = codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_0RGB32;
    hw_frame_context->format = codec_context->pix_fmt;
    hw_frame_context->device_ref = *device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext *)(*device_ctx)->data;

    if(use_nvfbc) {
        hw_frame_context->pool = av_buffer_pool_init(1, dummy_hw_frame_init);
        hw_frame_context->initial_pool_size = 1;
    }

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0\n");
        exit(1);
    }

    codec_context->hw_device_ctx = *device_ctx;
    codec_context->hw_frames_ctx = frame_context;

    bool supports_p4 = false;
    bool supports_p7 = false;

    const AVOption *opt = nullptr;
    while((opt = av_opt_next(codec_context->priv_data, opt))) {
        if(opt->type == AV_OPT_TYPE_CONST) {
            if(strcmp(opt->name, "p4") == 0)
                supports_p4 = true;
            else if(strcmp(opt->name, "p7") == 0)
                supports_p7 = true;
        }
    }

    AVDictionary *options = nullptr;
    if(very_old_gpu) {
        switch(video_quality) {
            case VideoQuality::MEDIUM:
                av_dict_set_int(&options, "qp", 37, 0);
                break;
            case VideoQuality::HIGH:
                av_dict_set_int(&options, "qp", 32, 0);
                break;
            case VideoQuality::VERY_HIGH:
                av_dict_set_int(&options, "qp", 27, 0);
                break;
            case VideoQuality::ULTRA:
                av_dict_set_int(&options, "qp", 21, 0);
                break;
        }
    } else {
        switch(video_quality) {
            case VideoQuality::MEDIUM:
                av_dict_set_int(&options, "qp", 40, 0);
                break;
            case VideoQuality::HIGH:
                av_dict_set_int(&options, "qp", 35, 0);
                break;
            case VideoQuality::VERY_HIGH:
                av_dict_set_int(&options, "qp", 30, 0);
                break;
            case VideoQuality::ULTRA:
                av_dict_set_int(&options, "qp", 24, 0);
                break;
        }
    }

    if(!supports_p4 && !supports_p7) {
        fprintf(stderr, "Info: your ffmpeg version is outdated. It's recommended that you use the flatpak version of gpu-screen-recorder version instead, which you can find at https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder\n");
    }

    //if(is_livestream) {
    //    av_dict_set_int(&options, "zerolatency", 1, 0);
    //    //av_dict_set(&options, "preset", "llhq", 0);
    //}

    // Fuck nvidia and ffmpeg, I want to use a good preset for the gpu but all gpus prefer different
    // presets. Nvidia and ffmpeg used to support "hq" preset that chose the best preset for the gpu
    // with pretty good performance but you now have to choose p1-p7, which are gpu agnostic and on
    // older gpus p5-p7 slow the gpu down to a crawl...
    // "hq" is now just an alias for p7 in ffmpeg :(
    if(very_old_gpu)
        av_dict_set(&options, "preset", supports_p4 ? "p4" : "medium", 0);
    else
        av_dict_set(&options, "preset", supports_p7 ? "p7" : "slow", 0);

    av_dict_set(&options, "tune", "hq", 0);
    av_dict_set(&options, "rc", "constqp", 0);

    ret = avcodec_open2(codec_context, codec_context->codec, &options);
    if (ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n",
                "blabla"); // av_err2str(ret));
        exit(1);
    }

    if(window_pixmap.target_texture_id != 0) {
        CUresult res;
        CUcontext old_ctx;
        res = cuda.cuCtxPopCurrent_v2(&old_ctx);
        res = cuda.cuCtxPushCurrent_v2(cuda_context);
        res = cuda.cuGraphicsGLRegisterImage(
            cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D,
            CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
        // cuda.cuGraphicsUnregisterResource(*cuda_graphics_resource);
        if (res != CUDA_SUCCESS) {
            const char *err_str;
            cuda.cuGetErrorString(res, &err_str);
            fprintf(stderr,
                    "Error: cuda.cuGraphicsGLRegisterImage failed, error %s, texture "
                    "id: %u\n",
                    err_str, window_pixmap.target_texture_id);
            exit(1);
        }
        res = cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static void close_video(AVStream *video_stream, AVFrame *frame) {
    // avcodec_close(video_stream->codec);
    // av_frame_free(&frame);
}

static void usage() {
    fprintf(stderr, "usage: gpu-screen-recorder -w <window_id> -c <container_format> -f <fps> [-a <audio_input>...] [-q <quality>] [-r <replay_buffer_size_sec>] [-o <output_file>]\n");
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -w    Window to record or a display, \"screen\" or \"screen-direct\". The display is the display name in xrandr and if \"screen\" or \"screen-direct\" is selected then all displays are recorded and they are recorded in h265 (aka hevc)."
        "\"screen-direct\" skips one texture copy for fullscreen applications so it may lead to better performance and it works with VRR monitors when recording fullscreen application but may break some applications, such as mpv in fullscreen mode. Recording a display requires a gpu with NvFBC support.\n");
    fprintf(stderr, "  -s    The size (area) to record at in the format WxH, for example 1920x1080. Usually you want to set this to the size of the window. Optional, by default the size of the window (which is passed to -w). This option is only supported when recording a window, not a screen/monitor.\n");
    fprintf(stderr, "  -c    Container format for output file, for example mp4, or flv.\n");
    fprintf(stderr, "  -f    Framerate to record at.\n");
    fprintf(stderr, "  -a    Audio device to record from (pulse audio device). Can be specified multiple times. Each time this is specified a new audio track is added for the specified audio device. A name can be given to the audio input device by prefixing the audio input with <name>/, for example \"dummy/alsa_output.pci-0000_00_1b.0.analog-stereo.monitor\". Optional, no audio track is added by default.\n");
    fprintf(stderr, "  -q    Video quality. Should be either 'medium', 'high', 'very_high' or 'ultra'. 'high' is the recommended option when live streaming or when you have a slower harddrive. Optional, set to 'very_high' be default.\n");
    fprintf(stderr, "  -r    Replay buffer size in seconds. If this is set, then only the last seconds as set by this option will be stored"
        " and the video will only be saved when the gpu-screen-recorder is closed. This feature is similar to Nvidia's instant replay feature."
        " This option has be between 5 and 1200. Note that the replay buffer size will not always be precise, because of keyframes. Optional, disabled by default.\n");
    fprintf(stderr, "  -k    Codec to use. Should be either 'auto', 'h264' or 'h265'. Defaults to 'auto' which defaults to 'h265' unless recording at a higher resolution than 60. Forcefully set to 'h264' if -c is 'flv'.\n");
    fprintf(stderr, "  -o    The output file path. If omitted then the encoded data is sent to stdout. Required in replay mode (when using -r). In replay mode this has to be an existing directory instead of a file.\n");
    fprintf(stderr, "NOTES:\n");
    fprintf(stderr, "  Send signal SIGINT (Ctrl+C) to gpu-screen-recorder to stop and save the recording (when not using replay mode).\n");
    fprintf(stderr, "  Send signal SIGUSR1 (killall -SIGUSR1 gpu-screen-recorder) to gpu-screen-recorder to save a replay.\n");
    exit(1);
}

static sig_atomic_t started = 0;
static sig_atomic_t running = 1;
static sig_atomic_t save_replay = 0;
static const char *pid_file = "/tmp/gpu-screen-recorder";

static void term_handler(int) {
    if(started)
        unlink(pid_file);
    exit(0);
}

static void int_handler(int) {
    running = 0;
}

static void save_replay_handler(int) {
    save_replay = 1;
}

struct Arg {
    std::vector<const char*> values;
    bool optional = false;
    bool list = false;

    const char* value() const {
        if(values.empty())
            return nullptr;
        return values.front();
    }
};

static bool is_hex_num(char c) {
    return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
    size_t len = strlen(str);
    if(len >= 2 && memcmp(str, "0x", 2) == 0) {
        str += 2;
        len -= 2;
    }

    for(size_t i = 0; i < len; ++i) {
        char c = str[i];
        if(c == '\0')
            return false;
        if(!is_hex_num(c))
            return true;
    }
    return false;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str; 
}

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = codec_context->framerate;
    return stream;
}

struct AudioTrack {
    AVCodecContext *codec_context = nullptr;
    AVFrame *frame = nullptr;
    AVStream *stream = nullptr;

    SoundDevice sound_device;
    std::thread thread; // TODO: Instead of having a thread for each track, have one thread for all threads and read the data with non-blocking read

    int stream_index = 0;
    AudioInput audio_input;
};

static std::future<void> save_replay_thread;
static std::vector<AVPacket> save_replay_packets;
static std::string save_replay_output_filepath;

static void save_replay_async(AVCodecContext *video_codec_context, int video_stream_index, std::vector<AudioTrack> &audio_tracks, const std::deque<AVPacket> &frame_data_queue, bool frames_erased, std::string output_dir, std::string container_format, std::mutex &write_output_mutex) {
    if(save_replay_thread.valid())
        return;
    
    size_t start_index = (size_t)-1;
    int64_t video_pts_offset = 0;
    int64_t audio_pts_offset = 0;

    {
        std::lock_guard<std::mutex> lock(write_output_mutex);
        start_index = (size_t)-1;
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            const AVPacket &av_packet = frame_data_queue[i];
            if((av_packet.flags & AV_PKT_FLAG_KEY) && av_packet.stream_index == video_stream_index) {
                start_index = i;
                break;
            }
        }

        if(start_index == (size_t)-1)
            return;

        if(frames_erased) {
            video_pts_offset = frame_data_queue[start_index].pts;
            
            // Find the next audio packet to use as audio pts offset
            for(size_t i = start_index; i < frame_data_queue.size(); ++i) {
                const AVPacket &av_packet = frame_data_queue[i];
                if(av_packet.stream_index != video_stream_index) {
                    audio_pts_offset = av_packet.pts;
                    break;
                }
            }
        } else {
            start_index = 0;
        }

        save_replay_packets.resize(frame_data_queue.size());
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            av_packet_ref(&save_replay_packets[i], &frame_data_queue[i]);
        }
    }

    save_replay_output_filepath = output_dir + "/Replay_" + get_date_str() + "." + container_format;
    save_replay_thread = std::async(std::launch::async, [video_stream_index, container_format, start_index, video_pts_offset, audio_pts_offset, video_codec_context, &audio_tracks]() mutable {
        AVFormatContext *av_format_context;
        // The output format is automatically guessed from the file extension
        avformat_alloc_output_context2(&av_format_context, nullptr, container_format.c_str(), nullptr);

        av_format_context->flags |= AVFMT_FLAG_GENPTS;
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVStream *video_stream = create_stream(av_format_context, video_codec_context);
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

        std::unordered_map<int, AudioTrack*> stream_index_to_audio_track_map;
        for(AudioTrack &audio_track : audio_tracks) {
            stream_index_to_audio_track_map[audio_track.stream_index] = &audio_track;
            AVStream *audio_stream = create_stream(av_format_context, audio_track.codec_context);
            avcodec_parameters_from_context(audio_stream->codecpar, audio_track.codec_context);
            audio_track.stream = audio_stream;
        }

        int ret = avio_open(&av_format_context->pb, save_replay_output_filepath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s. Make sure %s is an existing directory with write access\n", save_replay_output_filepath.c_str(), av_error_to_string(ret), save_replay_output_filepath.c_str());
            return;
        }

        ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return;
        }

        for(size_t i = start_index; i < save_replay_packets.size(); ++i) {
            AVPacket &av_packet = save_replay_packets[i];

            AVStream *stream = video_stream;
            AVCodecContext *codec_context = video_codec_context;

            if(av_packet.stream_index == video_stream_index) {
                av_packet.pts -= video_pts_offset;
                av_packet.dts -= video_pts_offset;
            } else {
                AudioTrack *audio_track = stream_index_to_audio_track_map[av_packet.stream_index];
                stream = audio_track->stream;
                codec_context = audio_track->codec_context;

                av_packet.pts -= audio_pts_offset;
                av_packet.dts -= audio_pts_offset;
            }

            av_packet.stream_index = stream->index;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            int ret = av_write_frame(av_format_context, &av_packet);
            if(ret < 0)
                fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", stream->index, av_error_to_string(ret), ret);
        }

        if (av_write_trailer(av_format_context) != 0)
            fprintf(stderr, "Failed to write trailer\n");

        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);

        for(AudioTrack &audio_track : audio_tracks) {
            audio_track.stream = nullptr;
        }
    });
}

static AudioInput parse_audio_input_arg(const char *str) {
    AudioInput audio_input;
    audio_input.name = str;
    const size_t index = audio_input.name.find('/');
    if(index != std::string::npos) {
        audio_input.description = audio_input.name.substr(0, index);
        audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + index + 1);
    }
    return audio_input;
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
    const int len = strlen(str);
    if((len >= 7 && memcmp(str, "http://", 7) == 0) || (len >= 8 && memcmp(str, "https://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtmp://", 7) == 0) || (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
        return true;
    else
        return false;
}

int main(int argc, char **argv) {
    signal(SIGTERM, term_handler);
    signal(SIGINT, int_handler);
    signal(SIGUSR1, save_replay_handler);

    std::map<std::string, Arg> args = {
        { "-w", Arg { {}, false, false } },
        //{ "-s", Arg { nullptr, true } },
        { "-c", Arg { {}, false, false } },
        { "-f", Arg { {}, false, false } },
        { "-s", Arg { {}, true, false } },
        { "-a", Arg { {}, true, true } },
        { "-q", Arg { {}, true, false } },
        { "-o", Arg { {}, true, false } },
        { "-r", Arg { {}, true, false } },
        { "-k", Arg { {}, true, false } }
    };

    for(int i = 1; i < argc - 1; i += 2) {
        auto it = args.find(argv[i]);
        if(it == args.end()) {
            fprintf(stderr, "Invalid argument '%s'\n", argv[i]);
            usage();
        }

        if(!it->second.values.empty() && !it->second.list) {
            fprintf(stderr, "Expected argument '%s' to only be specified once\n", argv[i]);
            usage();
        }

        it->second.values.push_back(argv[i + 1]);
    }

    for(auto &it : args) {
        if(!it.second.optional && !it.second.value()) {
            fprintf(stderr, "Missing argument '%s'\n", it.first.c_str());
            usage();
        }
    }

    VideoCodec video_codec;
    const char *codec_to_use = args["-k"].value();
    if(!codec_to_use)
        codec_to_use = "auto";

    if(strcmp(codec_to_use, "h264") == 0) {
        video_codec = VideoCodec::H264;
    } else if(strcmp(codec_to_use, "h265") == 0) {
        video_codec = VideoCodec::H265;
    } else if(strcmp(codec_to_use, "auto") != 0) {
        fprintf(stderr, "Error: -k should either be either 'auto', 'h264' or 'h265', got: '%s'\n", codec_to_use);
        usage();
    }

    const Arg &audio_input_arg = args["-a"];
    const std::vector<AudioInput> audio_inputs = get_pulseaudio_inputs();
    std::vector<AudioInput> requested_audio_inputs;

    // Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
    // Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
    // OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
    for(const char *audio_input : audio_input_arg.values) {
        requested_audio_inputs.push_back(parse_audio_input_arg(audio_input));
        AudioInput &request_audio_input = requested_audio_inputs.back();

        bool match = false;
        for(const auto &existing_audio_input : audio_inputs) {
            if(strcmp(request_audio_input.name.c_str(), existing_audio_input.name.c_str()) == 0) {
                if(request_audio_input.description.empty())
                    request_audio_input.description = "gsr-" + existing_audio_input.description;

                match = true;
                break;
            }
        }

        if(!match) {
            fprintf(stderr, "Error: Audio input device '%s' is not a valid audio device. Expected one of:\n", request_audio_input.name.c_str());
            for(const auto &existing_audio_input : audio_inputs) {
                fprintf(stderr, "    %s\n", existing_audio_input.name.c_str());
            }
            exit(2);
        }
    }

    uint32_t region_x = 0;
    uint32_t region_y = 0;
    uint32_t region_width = 0;
    uint32_t region_height = 0;

    /*
    TODO: Fix this. Doesn't work for some reason
    const char *screen_region = args["-s"].value();
    if(screen_region) {
        if(sscanf(screen_region, "%ux%u+%u+%u", &region_x, &region_y, &region_width, &region_height) != 4) {
            fprintf(stderr, "Invalid value for -s '%s', expected a value in format WxH+X+Y\n", screen_region);
            return 1;
        }
    }
    */

    const char *container_format = args["-c"].value();
    int fps = atoi(args["-f"].value());
    if(fps == 0) {
        fprintf(stderr, "Invalid fps argument: %s\n", args["-f"].value());
        return 1;
    }
    if(fps < 1)
        fps = 1;

    const char *quality_str = args["-q"].value();
    if(!quality_str)
        quality_str = "very_high";

    VideoQuality quality;
    if(strcmp(quality_str, "medium") == 0) {
        quality = VideoQuality::MEDIUM;
    } else if(strcmp(quality_str, "high") == 0) {
        quality = VideoQuality::HIGH;
    } else if(strcmp(quality_str, "very_high") == 0) {
        quality = VideoQuality::VERY_HIGH;
    } else if(strcmp(quality_str, "ultra") == 0) {
        quality = VideoQuality::ULTRA;
    } else {
        fprintf(stderr, "Error: -q should either be either 'medium', 'high', 'very_high' or 'ultra', got: '%s'\n", quality_str);
        usage();
    }

    int replay_buffer_size_secs = -1;
    const char *replay_buffer_size_secs_str = args["-r"].value();
    if(replay_buffer_size_secs_str) {
        replay_buffer_size_secs = atoi(replay_buffer_size_secs_str);
        if(replay_buffer_size_secs < 5 || replay_buffer_size_secs > 1200) {
            fprintf(stderr, "Error: option -r has to be between 5 and 1200, was: %s\n", replay_buffer_size_secs_str);
            return 1;
        }
        replay_buffer_size_secs += 5; // Add a few seconds to account of lost packets because of non-keyframe packets skipped
    }

    if(!cuda.load()) {
        fprintf(stderr, "Error: failed to load cuda\n");
        return 2;
    }

    CUresult res;

    res = cuda.cuInit(0);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: cuInit failed, error %s (result: %d)\n", err_str, res);
        return 1;
    }

    int nGpu = 0;
    cuda.cuDeviceGetCount(&nGpu);
    if (nGpu <= 0) {
        fprintf(stderr, "Error: no cuda supported devices found\n");
        return 1;
    }

    CUdevice cu_dev;
    res = cuda.cuDeviceGet(&cu_dev, 0);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: unable to get CUDA device, error: %s (result: %d)\n", err_str, res);
        return 1;
    }

    CUcontext cu_ctx;
    res = cuda.cuCtxCreate_v2(&cu_ctx, CU_CTX_SCHED_AUTO, cu_dev);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: unable to create CUDA context, error: %s (result: %d)\n", err_str, res);
        return 1;
    }

    const char *record_area = args["-s"].value();

    uint32_t window_width = 0;
    uint32_t window_height = 0;
    int window_x = 0;
    int window_y = 0;

    NvFBCLibrary nv_fbc_library;

    const char *window_str = args["-w"].value();
    Window src_window_id = None;
    if(contains_non_hex_number(window_str)) {
        if(record_area) {
            fprintf(stderr, "Option -s is not supported when recording a monitor/screen\n");
            usage();
        }

        if(!nv_fbc_library.load())
            return 1;

        const char *capture_target = window_str;
        bool direct_capture = strcmp(window_str, "screen-direct") == 0;
        if(direct_capture) {
            capture_target = "screen";
            // TODO: Temporary disable direct capture because push model causes stuttering when it's direct capturing. This might be a nvfbc bug. This does not happen when using a compositor.
            direct_capture = false;
            fprintf(stderr, "Warning: screen-direct has temporary been disabled as it causes stuttering. This is likely a NvFBC bug. Falling back to \"screen\".\n");
        }

        if(!nv_fbc_library.create(capture_target, fps, &window_width, &window_height, region_x, region_y, region_width, region_height, direct_capture))
            return 1;
    } else {
        errno = 0;
        src_window_id = strtol(window_str, nullptr, 0);
        if(src_window_id == None || errno == EINVAL) {
            fprintf(stderr, "Invalid window number %s\n", window_str);
            usage();
        }
    }

    int record_width = window_width;
    int record_height = window_height;
    if(record_area) {
        if(sscanf(record_area, "%dx%d", &record_width, &record_height) != 2) {
            fprintf(stderr, "Invalid value for -s '%s', expected a value in format WxH\n", record_area);
            return 1;
        }
    }

    const char *filename = args["-o"].value();
    if(filename) {
        if(replay_buffer_size_secs != -1) {
            struct stat buf;
            if(stat(filename, &buf) == -1 || !S_ISDIR(buf.st_mode)) {
                fprintf(stderr, "%s does not exist or is not a directory\n", filename);
                usage();
            }
        }
    } else {
        if(replay_buffer_size_secs == -1) {
            filename = "/dev/stdout";
        } else {
            fprintf(stderr, "Option -o is required when using option -r\n");
            usage();
        }
    }

    const double target_fps = 1.0 / (double)fps;

    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "Error: Failed to open display\n");
        return 1;
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    WindowPixmap window_pixmap;
    Window window = None;
    if(src_window_id) {
        bool has_name_pixmap = x11_supports_composite_named_window_pixmap(dpy);
        if (!has_name_pixmap) {
            fprintf(stderr, "Error: XCompositeNameWindowPixmap is not supported by "
                            "your X11 server\n");
            return 1;
        }

        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, src_window_id, &attr)) {
            fprintf(stderr, "Error: Invalid window id: %lu\n", src_window_id);
            return 1;
        }

        window_width = std::max(0, attr.width);
        window_height = std::max(0, attr.height);
        window_x = attr.x;
        window_y = attr.y;
        Window c;    
        XTranslateCoordinates(dpy, src_window_id, DefaultRootWindow(dpy), 0, 0, &window_x, &window_y, &c);

        XCompositeRedirectWindow(dpy, src_window_id, CompositeRedirectAutomatic);

        if(!gl.load()) {
            fprintf(stderr, "Error: Failed to load opengl\n");
            return 1;
        }

        window = create_opengl_window(dpy);
        if(!window)
            return 1;

        set_vertical_sync_enabled(dpy, window, false);
        recreate_window_pixmap(dpy, src_window_id, window_pixmap);

        if(!record_area) {
            record_width = window_pixmap.texture_width;
            record_height = window_pixmap.texture_height;
            fprintf(stderr, "Record size: %dx%d\n", record_width, record_height);
        }
    } else {
        window_pixmap.texture_id = 0;
        window_pixmap.target_texture_id = 0;
        window_pixmap.texture_width = window_width;
        window_pixmap.texture_height = window_height;
    }

    bool very_old_gpu = false;
    bool gl_loaded = window;
    if(!gl_loaded) {
        if(!gl.load()) {
            fprintf(stderr, "Error: Failed to load opengl\n");
            return 1;
        }
    }

    const unsigned char *gl_renderer = gl.glGetString(GL_RENDERER);
    if(gl_renderer) {
        int gpu_num = 1000;
        sscanf((const char*)gl_renderer, "%*s %*s %*s %d", &gpu_num);
        if(gpu_num < 900) {
            fprintf(stderr, "Info: your gpu appears to be very old (older than maxwell architecture). Switching to lower preset\n");
            very_old_gpu = true;
        }
    }

    if(!gl_loaded)
        gl.unload();

    if(strcmp(codec_to_use, "auto") == 0) {
        const AVCodec *h265_codec = find_h265_encoder();

        // h265 generally allows recording at a higher resolution than h264 on nvidia cards. On a gtx 1080 4k is the max resolution for h264 but for h265 it's 8k.
        // Another important info is that when recording at a higher fps than.. 60? h265 has very bad performance. For example when recording at 144 fps the fps drops to 1
        // while with h264 the fps doesn't drop.
        if(!h265_codec) {
            fprintf(stderr, "Info: using h264 encoder because a codec was not specified and your gpu does not support h265\n");
        } else if(fps > 60) {
            fprintf(stderr, "Info: using h264 encoder because a codec was not specified and fps is more than 60\n");
            codec_to_use = "h264";
            video_codec = VideoCodec::H264;
        } else {
            fprintf(stderr, "Info: using h265 encoder because a codec was not specified\n");
            codec_to_use = "h265";
            video_codec = VideoCodec::H265;
        }
    }

    const AVCodec *video_codec_f = nullptr;
    switch(video_codec) {
        case VideoCodec::H264:
            video_codec_f = find_h264_encoder();
            break;
        case VideoCodec::H265:
            video_codec_f = find_h265_encoder();
            break;
    }

    if(!video_codec_f) {
        fprintf(stderr, "Error: your gpu does not support '%s' video codec\n", video_codec == VideoCodec::H264 ? "h264" : "h265");
        exit(2);
    }

    // Video start
    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, container_format,
                                   nullptr);
    if (!av_format_context) {
        fprintf(
            stderr,
            "Error: Failed to deduce output format from file extension\n");
        return 1;
    }

    av_format_context->flags |= AVFMT_FLAG_GENPTS;
    const AVOutputFormat *output_format = av_format_context->oformat;

    const bool is_livestream = is_livestream_path(filename);
    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(is_livestream && requested_audio_inputs.empty()) {
        fprintf(stderr, "Info: live streaming but no audio track was added. Adding a silent audio track\n");
        requested_audio_inputs.push_back({ "", "gsr-silent" });
    }

    //bool use_hevc = strcmp(window_str, "screen") == 0 || strcmp(window_str, "screen-direct") == 0;
    if(video_codec != VideoCodec::H264 && strcmp(container_format, "flv") == 0) {
        video_codec = VideoCodec::H264;
        fprintf(stderr, "Warning: h265 is not compatible with flv, falling back to h264 instead.\n");
    }

    AVStream *video_stream = nullptr;
    std::vector<AudioTrack> audio_tracks;

    AVCodecContext *video_codec_context = create_video_codec_context(av_format_context, quality, record_width, record_height, fps, video_codec_f, is_livestream);
    if(replay_buffer_size_secs == -1)
        video_stream = create_stream(av_format_context, video_codec_context);

    AVBufferRef *device_ctx;
    CUgraphicsResource cuda_graphics_resource;
    open_video(video_codec_context, window_pixmap, &device_ctx, &cuda_graphics_resource, cu_ctx, !src_window_id, quality, is_livestream, very_old_gpu);
    if(video_stream)
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(const AudioInput &audio_input : requested_audio_inputs) {
        AVCodecContext *audio_codec_context = create_audio_codec_context(av_format_context, fps);

        AVStream *audio_stream = nullptr;
        if(replay_buffer_size_secs == -1)
            audio_stream = create_stream(av_format_context, audio_codec_context);

        AVFrame *audio_frame = open_audio(audio_codec_context);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        audio_tracks.push_back({ audio_codec_context, audio_frame, audio_stream, {}, {}, audio_stream_index, audio_input });
        ++audio_stream_index;
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, av_error_to_string(ret));
            return 1;
        }
    }

    //video_stream->duration = AV_TIME_BASE * 15;
    //audio_stream->duration = AV_TIME_BASE * 15;
    //av_format_context->duration = AV_TIME_BASE * 15;
    if(replay_buffer_size_secs == -1) {
        int ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return 1;
        }
    }

    // av_frame_free(&rgb_frame);
    // avcodec_close(av_codec_context);

    if(src_window_id)
        XSelectInput(dpy, src_window_id, StructureNotifyMask | ExposureMask);

    /*
    int damage_event;
    int damage_error;
    if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "Error: XDamage is not supported by your X11 server\n");
        return 1;
    }

    Damage damage = XDamageCreate(dpy, src_window_id, XDamageReportNonEmpty);
    XDamageSubtract(dpy, damage,None,None);
    */

    const double start_time_pts = clock_get_monotonic_seconds();

    CUcontext old_ctx;
    CUarray mapped_array;
    if(src_window_id) {
        res = cuda.cuCtxPopCurrent_v2(&old_ctx);
        res = cuda.cuCtxPushCurrent_v2(cu_ctx);

        // Get texture
        res = cuda.cuGraphicsResourceSetMapFlags(
            cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
        res = cuda.cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

        // Map texture to cuda array
        res = cuda.cuGraphicsSubResourceGetMappedArray(&mapped_array,
                                                cuda_graphics_resource, 0, 0);
    }

    // Release texture
    // res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    double start_time = clock_get_monotonic_seconds();
    double frame_timer_start = start_time;
    double window_resize_timer = start_time;
    bool window_resized = false;
    int fps_counter = 0;
    int current_fps = 30;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    frame->format = video_codec_context->pix_fmt;
    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(src_window_id) {
        if (av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0) < 0) {
            fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
            exit(1);
        }
    } else {
        frame->hw_frames_ctx = av_buffer_ref(video_codec_context->hw_frames_ctx);
        frame->buf[0] = av_buffer_pool_get(((AVHWFramesContext*)video_codec_context->hw_frames_ctx->data)->pool);
        frame->extended_data = frame->data;
    }

    if(window_pixmap.texture_width < record_width)
        frame->width = window_pixmap.texture_width & ~1;
    else
        frame->width = record_width & ~1;

    if(window_pixmap.texture_height < record_height)
        frame->height = window_pixmap.texture_height & ~1;
    else
        frame->height = record_height & ~1;

    std::mutex write_output_mutex;

    const double record_start_time = clock_get_monotonic_seconds();
    std::deque<AVPacket> frame_data_queue;
    bool frames_erased = false;

    const size_t audio_buffer_size = 1024 * 2 * 2; // 2 bytes/sample, 2 channels
    uint8_t *empty_audio = (uint8_t*)malloc(audio_buffer_size);
    if(!empty_audio) {
        fprintf(stderr, "Error: failed to create empty audio\n");
        exit(1);
    }
    memset(empty_audio, 0, audio_buffer_size);

    for(AudioTrack &audio_track : audio_tracks) {
        audio_track.thread = std::thread([record_start_time, replay_buffer_size_secs, &frame_data_queue, &frames_erased, &audio_track, empty_audio](AVFormatContext *av_format_context, std::mutex *write_output_mutex) mutable {
            #if LIBAVCODEC_VERSION_MAJOR < 60
            const int num_channels = audio_track.codec_context->channels;
            #else
            const int num_channels = audio_track.codec_context->ch_layout.nb_channels;
            #endif

            if(audio_track.audio_input.name.empty()) {
                audio_track.sound_device.handle = NULL;
                audio_track.sound_device.frames = 0;
            } else {
                if(sound_device_get_by_name(&audio_track.sound_device, audio_track.audio_input.name.c_str(), audio_track.audio_input.description.c_str(), num_channels, audio_track.codec_context->frame_size) != 0) {
                    fprintf(stderr, "failed to get 'pulse' sound device\n");
                    exit(1);
                }
            }

            SwrContext *swr = swr_alloc();
            if(!swr) {
                fprintf(stderr, "Failed to create SwrContext\n");
                exit(1);
            }
            av_opt_set_int(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(swr, "in_sample_rate", audio_track.codec_context->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", audio_track.codec_context->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
            swr_init(swr);

            int64_t pts = 0;
            const double target_audio_hz = 1.0 / (double)audio_track.codec_context->sample_rate;
            double received_audio_time = clock_get_monotonic_seconds();
            const int64_t timeout_ms = std::round((1000.0 / (double)audio_track.codec_context->sample_rate) * 1000.0);

            while(running) {
                void *sound_buffer;
                int sound_buffer_size = -1;
                if(audio_track.sound_device.handle)
                    sound_buffer_size = sound_device_read_next_chunk(&audio_track.sound_device, &sound_buffer);
                const bool got_audio_data = sound_buffer_size >= 0;

                const double this_audio_frame_time = clock_get_monotonic_seconds();
                if(got_audio_data)
                    received_audio_time = this_audio_frame_time;

                int ret = av_frame_make_writable(audio_track.frame);
                if (ret < 0) {
                    fprintf(stderr, "Failed to make audio frame writable\n");
                    break;
                }

                const int64_t num_missing_frames = std::round((this_audio_frame_time - received_audio_time) / target_audio_hz / (int64_t)audio_track.frame->nb_samples);
                // Jesus is there a better way to do this? I JUST WANT TO KEEP VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW.
                // THIS PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY.
                // BUT WE CANT USE DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG DELAYED!!!
                if(num_missing_frames >= 5 || (num_missing_frames > 0 && got_audio_data)) {
                    // TODO:
                    //audio_track.frame->data[0] = empty_audio;
                    received_audio_time = this_audio_frame_time;
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&empty_audio, audio_track.sound_device.frames);
                    // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                    for(int i = 0; i < num_missing_frames; ++i) {
                        audio_track.frame->pts = pts;
                        pts += audio_track.frame->nb_samples;
                        ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                        if(ret >= 0){
                            receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                        } else {
                            fprintf(stderr, "Failed to encode audio!\n");
                        }
                    }
                }

                if(!audio_track.sound_device.handle) {
                    // TODO:
                    //audio_track.frame->data[0] = empty_audio;
                    received_audio_time = this_audio_frame_time;
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&empty_audio, audio_track.codec_context->frame_size);
                    audio_track.frame->pts = pts;
                    pts += audio_track.frame->nb_samples;
                    ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                    if(ret >= 0){
                        receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                    } else {
                        fprintf(stderr, "Failed to encode audio!\n");
                    }

                    usleep(timeout_ms * 1000);
                }

                if(got_audio_data) {
                    // TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&sound_buffer, audio_track.sound_device.frames);

                    audio_track.frame->pts = pts;
                    pts += audio_track.frame->nb_samples;

                    ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                    if(ret >= 0){
                        receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                    } else {
                        fprintf(stderr, "Failed to encode audio!\n");
                    }
                }
            }

            sound_device_close(&audio_track.sound_device);
            swr_free(&swr);
        }, av_format_context, &write_output_mutex);
    }

    started = 1;

    // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio desync or too fast/slow video.
    const double update_fps = fps + 190;
    int64_t video_pts_counter = 0;

    bool redraw = true;
    XEvent e;
    while (running) {
        double frame_start = clock_get_monotonic_seconds();
        if(window)
            gl.glClear(GL_COLOR_BUFFER_BIT);

        redraw = true;

        if(src_window_id) {
            if (XCheckTypedWindowEvent(dpy, src_window_id, DestroyNotify, &e)) {
                running = 0;
            }

            if (XCheckTypedWindowEvent(dpy, src_window_id, Expose, &e) && e.xexpose.count == 0) {
                window_resize_timer = clock_get_monotonic_seconds();
                window_resized = true;
            }

            if (XCheckTypedWindowEvent(dpy, src_window_id, ConfigureNotify, &e) && e.xconfigure.window == src_window_id) {
                while(XCheckTypedWindowEvent(dpy, src_window_id, ConfigureNotify, &e)) {}
                window_x = e.xconfigure.x;
                window_y = e.xconfigure.y;
                Window c;
                XTranslateCoordinates(dpy, src_window_id, DefaultRootWindow(dpy), 0, 0, &window_x, &window_y, &c);
                // Window resize
                if(e.xconfigure.width != (int)window_width || e.xconfigure.height != (int)window_height) {
                    window_width = std::max(0, e.xconfigure.width);
                    window_height = std::max(0, e.xconfigure.height);
                    window_resize_timer = clock_get_monotonic_seconds();
                    window_resized = true;
                }
            }

            const double window_resize_timeout = 1.0; // 1 second
            if(window_resized && clock_get_monotonic_seconds() - window_resize_timer >= window_resize_timeout) {
                window_resized = false;
                fprintf(stderr, "Resize window!\n");
                recreate_window_pixmap(dpy, src_window_id, window_pixmap);
                // Resolution must be a multiple of two
                //video_stream->codec->width = window_pixmap.texture_width & ~1;
                //video_stream->codec->height = window_pixmap.texture_height & ~1;

                cuda.cuGraphicsUnregisterResource(cuda_graphics_resource);
                res = cuda.cuGraphicsGLRegisterImage(
                    &cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D,
                    CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
                if (res != CUDA_SUCCESS) {
                    const char *err_str;
                    cuda.cuGetErrorString(res, &err_str);
                    fprintf(stderr,
                            "Error: cuda.cuGraphicsGLRegisterImage failed, error %s, texture "
                            "id: %u\n",
                            err_str, window_pixmap.target_texture_id);
                    running = false;
                    break;
                }

                res = cuda.cuGraphicsResourceSetMapFlags(
                    cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
                res = cuda.cuGraphicsMapResources(1, &cuda_graphics_resource, 0);
                res = cuda.cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

                av_frame_free(&frame);
                frame = av_frame_alloc();
                if (!frame) {
                    fprintf(stderr, "Error: Failed to allocate frame\n");
                    running = false;
                    break;
                }
                frame->format = video_codec_context->pix_fmt;
                frame->width = video_codec_context->width;
                frame->height = video_codec_context->height;

                if (av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0) < 0) {
                    fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
                    running = false;
                    break;
                }

                if(window_pixmap.texture_width < record_width)
                    frame->width = window_pixmap.texture_width & ~1;
                else
                    frame->width = record_width & ~1;

                if(window_pixmap.texture_height < record_height)
                    frame->height = window_pixmap.texture_height & ~1;
                else
                    frame->height = record_height & ~1;

                // Make the new completely black to clear unused parts
                // TODO: cuMemsetD32?
                cuda.cuMemsetD8_v2((CUdeviceptr)frame->data[0], 0, record_width * record_height * 4);
            }
        }

        ++fps_counter;

        double time_now = clock_get_monotonic_seconds();
        double frame_timer_elapsed = time_now - frame_timer_start;
        double elapsed = time_now - start_time;
        if (elapsed >= 1.0) {
            fprintf(stderr, "update fps: %d\n", fps_counter);
            start_time = time_now;
            current_fps = fps_counter;
            fps_counter = 0;
        }

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if (frame_time_overflow >= 0.0) {
            frame_timer_start = time_now - frame_time_overflow;

            bool frame_captured = true;
            if(redraw) {
                redraw = false;
                if(src_window_id) {
                    // TODO: Use a framebuffer instead. glCopyImageSubData requires
                    // opengl 4.2
                    int source_x = 0;
                    int source_y = 0;

                    int source_width = window_pixmap.texture_width;
                    int source_height = window_pixmap.texture_height;

                    bool clamped = false;

                    if(window_pixmap.composite_window) {
                        source_x = window_x;
                        source_y = window_y;

                        int underflow_x = 0;
                        int underflow_y = 0;

                        if(source_x < 0) {
                            underflow_x = -source_x;
                            source_x = 0;
                            source_width += source_x;
                        }

                        if(source_y < 0) {
                            underflow_y = -source_y;
                            source_y = 0;
                            source_height += source_y;
                        }

                        const int clamped_source_width = std::max(0, window_pixmap.texture_real_width - source_x - underflow_x);
                        const int clamped_source_height = std::max(0, window_pixmap.texture_real_height - source_y - underflow_y);

                        if(clamped_source_width < source_width) {
                            source_width = clamped_source_width;
                            clamped = true;
                        }

                        if(clamped_source_height < source_height) {
                            source_height = clamped_source_height;
                            clamped = true;
                        }
                    }

                    if(clamped) {
                        // Requires opengl 4.4... TODO: Replace with earlier opengl if opengl < 4.2
                        if(gl.glClearTexImage)
                            gl.glClearTexImage(window_pixmap.target_texture_id, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                    }

                    // Requires opengl 4.2... TODO: Replace with earlier opengl if opengl < 4.2
                    gl.glCopyImageSubData(
                        window_pixmap.texture_id, GL_TEXTURE_2D, 0, source_x, source_y, 0,
                        window_pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                        source_width, source_height, 1);
                    unsigned int err = gl.glGetError();
                    if(err != 0) {
                        static bool error_shown = false;
                        if(!error_shown) {
                            error_shown = true;
                            fprintf(stderr, "Error: glCopyImageSubData failed, gl error: %d\n", err);
                        }
                    }
                    gl.glXSwapBuffers(dpy, window);
                    // int err = gl.glGetError();
                    // fprintf(stderr, "error: %d\n", err);

                    // TODO: Remove this copy, which is only possible by using nvenc directly and encoding window_pixmap.target_texture_id

                    frame->linesize[0] = frame->width * 4;

                    CUDA_MEMCPY2D memcpy_struct;
                    memcpy_struct.srcXInBytes = 0;
                    memcpy_struct.srcY = 0;
                    memcpy_struct.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;

                    memcpy_struct.dstXInBytes = 0;
                    memcpy_struct.dstY = 0;
                    memcpy_struct.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

                    memcpy_struct.srcArray = mapped_array;
                    memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
                    memcpy_struct.dstPitch = frame->linesize[0];
                    memcpy_struct.WidthInBytes = frame->width * 4;
                    memcpy_struct.Height = frame->height;
                    cuda.cuMemcpy2D_v2(&memcpy_struct);

                    frame_captured = true;
                } else {
                    // TODO: Check when src_cu_device_ptr changes and re-register resource
                    frame->linesize[0] = frame->width * 4;

                    uint32_t byte_size = 0;
                    CUdeviceptr src_cu_device_ptr = 0;
                    frame_captured = nv_fbc_library.capture(&src_cu_device_ptr, &byte_size);
                    frame->data[0] = (uint8_t*)src_cu_device_ptr;
                }
                // res = cuda.cuCtxPopCurrent_v2(&old_ctx);
            }

            const double this_video_frame_time = clock_get_monotonic_seconds();
            const int64_t expected_frames = std::round((this_video_frame_time - start_time_pts) / target_fps);

            const int num_frames = std::max(0L, expected_frames - video_pts_counter);
            // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
            for(int i = 0; i < num_frames; ++i) {
                frame->pts = video_pts_counter + i;
                if (avcodec_send_frame(video_codec_context, frame) >= 0) {
                    receive_frames(video_codec_context, VIDEO_STREAM_INDEX, video_stream, frame, av_format_context,
                                record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex);
                } else {
                    fprintf(stderr, "Error: avcodec_send_frame failed\n");
                }
            }
            video_pts_counter += num_frames;
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            save_replay_thread.get();
            puts(save_replay_output_filepath.c_str());
            for(size_t i = 0; i < save_replay_packets.size(); ++i) {
                av_packet_unref(&save_replay_packets[i]);
            }
            save_replay_packets.clear();
        }

        if(save_replay == 1 && !save_replay_thread.valid() && replay_buffer_size_secs != -1) {
            save_replay = 0;
            save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks, frame_data_queue, frames_erased, filename, container_format, write_output_mutex);
        }

        // av_frame_free(&frame);
        double frame_end = clock_get_monotonic_seconds();
        double frame_sleep_fps = 1.0 / update_fps;
        double sleep_time = frame_sleep_fps - (frame_end - frame_start);
        if(sleep_time > 0.0)
            usleep(sleep_time * 1000.0 * 1000.0);
    }

	running = 0;

    if(save_replay_thread.valid())
        save_replay_thread.get();

    for(AudioTrack &audio_track : audio_tracks) {
        audio_track.thread.join();
    }

    if (replay_buffer_size_secs == -1 && av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    if(replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);

    if(dpy)
        XCloseDisplay(dpy);

    unlink(pid_file);
    free(empty_audio);
}
