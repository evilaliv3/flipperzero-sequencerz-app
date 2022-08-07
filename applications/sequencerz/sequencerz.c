#include <furi.h>

#include <gui/gui.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#include <lib/toolbox/path.h>
#include <assets_icons.h>

#include <flipper_format/flipper_format_i.h>
#include <applications/subghz/subghz_i.h>

#include "flipper_format_stream.h"
#include "flipper_format_stream_i.h"

#include <lib/subghz/transmitter.h>
#include <lib/subghz/protocols/raw.h>

#include "sequencerz_file.h"
#include "canvas_helper.h"

#define PLAYLIST_FOLDER "/ext/sequencerz"
#define PLAYLIST_EXT ".txt"
#define TAG "Playlist"

#define STATE_NONE 0
#define STATE_OVERVIEW 1
#define STATE_SENDING 2

#define WIDTH 128
#define HEIGHT 64

typedef struct {
    int current_count; // number of processed files
    int total_count; // number of items in the sequencerz

    // last 3 files
    string_t prev_0_path; // current file
    string_t prev_1_path; // previous file
    string_t prev_2_path; // previous previous file
    string_t prev_3_path; // you get the idea

    int state; // current state

    ViewPort* view_port;
} DisplayMeta;

typedef struct {
    FuriThread* thread;
    Storage* storage;
    FlipperFormat* format;

    DisplayMeta* meta;

    string_t file_path; // path to the sequencerz file

    bool ctl_request_exit; // can be set to true if the worker should exit
    bool ctl_pause; // can be set to true if the worker should pause

    bool is_running; // indicates if the worker is running
} PlaylistWorker;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;

    DisplayMeta* meta;
    PlaylistWorker* worker;

    string_t file_path; // Path to the sequencerz file
} Playlist;

////////////////////////////////////////////////////////////////////////////////

void meta_set_state(DisplayMeta* meta, int state) {
    meta->state = state;
    view_port_update(meta->view_port);
}

static FuriHalSubGhzPreset str_to_preset(string_t preset) {
    if(string_cmp_str(preset, "FuriHalSubGhzPresetOok270Async") == 0) {
        return FuriHalSubGhzPresetOok270Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetOok650Async") == 0) {
        return FuriHalSubGhzPresetOok650Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev238Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev238Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev476Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev476Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetMSK99_97KbAsync") == 0) {
        return FuriHalSubGhzPresetMSK99_97KbAsync;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetMSK99_97KbAsync") == 0) {
        return FuriHalSubGhzPresetMSK99_97KbAsync;
    }
    return FuriHalSubGhzPresetCustom;
}

// -4: missing protocol
// -3: missing preset
// -2: transmit error
// -1: error
// 0: ok
// 1: resend
// 2: exited
static int sequencerz_worker_process(
    PlaylistWorker* worker,
    FlipperFormat* fff_file,
    FlipperFormat* fff_data,
    const char* path,
    string_t preset,
    string_t protocol) {
    // actual sending of .sub file

    if(!flipper_format_file_open_existing(fff_file, path)) {
        return -1;
    }

    // read frequency or default to 433.92MHz
    uint32_t frequency = 0;
    if(!flipper_format_read_uint32(fff_file, "Frequency", &frequency, 1)) {
        frequency = 433920000;
    }
    if(!furi_hal_subghz_is_tx_allowed(frequency)) {
        return -2;
    }

    // check if preset is present
    if(!flipper_format_read_string(fff_file, "Preset", preset)) {
        return -3;
    }

    // check if protocol is present
    if(!flipper_format_read_string(fff_file, "Protocol", protocol)) {
        return -4;
    }

    if(!string_cmp_str(protocol, "RAW")) {
        subghz_protocol_raw_gen_fff_data(fff_data, path);
    } else {
        stream_copy_full(
            flipper_format_get_raw_stream(fff_file), flipper_format_get_raw_stream(fff_data));
    }
    flipper_format_free(fff_file);

    // (try to) send file
    SubGhzEnvironment* environment = subghz_environment_alloc();
    SubGhzTransmitter* transmitter =
        subghz_transmitter_alloc_init(environment, string_get_cstr(protocol));

    subghz_transmitter_deserialize(transmitter, fff_data);

    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(str_to_preset(preset));

    frequency = furi_hal_subghz_set_frequency_and_path(frequency);

    furi_hal_power_suppress_charge_enter();

    int status = 0;

    furi_hal_subghz_start_async_tx(subghz_transmitter_yield, transmitter);
    while(!furi_hal_subghz_is_async_tx_complete()) {
        if(worker->ctl_request_exit) {
            status = 2;
            break;
        }
        if(worker->ctl_pause) {
            status = 1;
            break;
        }
        furi_delay_ms(50);
    }

    furi_hal_subghz_stop_async_tx();
    furi_hal_subghz_sleep();

    furi_hal_power_suppress_charge_exit();

    subghz_transmitter_free(transmitter);

    return status;
}

// true - the worker can continue
// false - the worker should exit
static bool sequencerz_worker_wait_pause(PlaylistWorker* worker) {
    // wait if paused
    while(worker->ctl_pause && !worker->ctl_request_exit) {
        furi_delay_ms(50);
    }
    // exit loop if requested to stop
    if(worker->ctl_request_exit) {
        return false;
    }
    return true;
}

static int32_t sequencerz_worker_thread(void* ctx) {
    PlaylistWorker* worker = ctx;

    string_t data, preset, protocol;
    string_init(data);
    string_init(preset);
    string_init(protocol);

    while(sequencerz_worker_wait_pause(worker)) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FlipperFormat* fff_head = flipper_format_file_alloc(storage);
        FlipperFormat* fff_data = flipper_format_string_alloc();

        if(!flipper_format_file_open_existing(fff_head, string_get_cstr(worker->file_path))) {
            worker->is_running = false;

            furi_record_close(RECORD_STORAGE);
            flipper_format_free(fff_head);
            return 0;
        }

        while(flipper_format_read_string(fff_head, "sub", data)) {
            if(!sequencerz_worker_wait_pause(worker)) {
                break;
            }

            // update state to sending
            meta_set_state(worker->meta, STATE_SENDING);

            ++worker->meta->current_count;

            const char* str = string_get_cstr(data);

            // it's not fancy, but it works for now :)
            string_reset(worker->meta->prev_3_path);
            string_set_str(worker->meta->prev_3_path, string_get_cstr(worker->meta->prev_2_path));
            string_reset(worker->meta->prev_2_path);
            string_set_str(worker->meta->prev_2_path, string_get_cstr(worker->meta->prev_1_path));
            string_reset(worker->meta->prev_1_path);
            string_set_str(worker->meta->prev_1_path, string_get_cstr(worker->meta->prev_0_path));
            string_reset(worker->meta->prev_0_path);
            string_set_str(worker->meta->prev_0_path, str);
            view_port_update(worker->meta->view_port);

            if(!sequencerz_worker_wait_pause(worker)) {
                break;
            }

            view_port_update(worker->meta->view_port);

            FlipperFormat* fff_file = flipper_format_file_alloc(storage);

            int status =
                sequencerz_worker_process(worker, fff_file, fff_data, str, preset, protocol);

            // if there was an error, fff_file is not already freed
            if(status < 0) {
                flipper_format_free(fff_file);
            }

            // re-send file is paused mid-send
            if(status == 1) {
              // errored, skip to next file
            } else if(status < 0) {
                break;
                // exited, exit loop
            } else if(status == 2) {
                break;
            }
        }

        worker->meta->current_count = 0;

        sequencerz_meta_free(worker->meta);

        string_clear(data);
        string_clear(preset);
        string_clear(protocol);

        furi_record_close(RECORD_STORAGE);
        flipper_format_free(fff_head);
        flipper_format_free(fff_data);
    }

    worker->is_running = false;

    // update state to overview
    meta_set_state(worker->meta, STATE_OVERVIEW);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

void sequencerz_meta_reset(DisplayMeta* instance) {
    instance->current_count = 0;

    string_reset(instance->prev_0_path);
    string_reset(instance->prev_1_path);
    string_reset(instance->prev_2_path);
    string_reset(instance->prev_3_path);
}

DisplayMeta* sequencerz_meta_alloc() {
    DisplayMeta* instance = malloc(sizeof(DisplayMeta));
    string_init(instance->prev_0_path);
    string_init(instance->prev_1_path);
    string_init(instance->prev_2_path);
    string_init(instance->prev_3_path);
    sequencerz_meta_reset(instance);
    instance->state = STATE_NONE;
    return instance;
}

void sequencerz_meta_free(DisplayMeta* instance) {
    string_clear(instance->prev_0_path);
    string_clear(instance->prev_1_path);
    string_clear(instance->prev_2_path);
    string_clear(instance->prev_3_path);
    free(instance);
}

////////////////////////////////////////////////////////////////////////////////

PlaylistWorker* sequencerz_worker_alloc(DisplayMeta* meta) {
    PlaylistWorker* instance = malloc(sizeof(PlaylistWorker));

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "PlaylistWorker");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);
    furi_thread_set_callback(instance->thread, sequencerz_worker_thread);

    instance->meta = meta;
    instance->ctl_pause = true; // require the user to manually start the worker

    string_init(instance->file_path);

    return instance;
}

void sequencerz_worker_free(PlaylistWorker* instance) {
    furi_assert(instance);
    furi_thread_free(instance->thread);
    string_clear(instance->file_path);
    free(instance);
}

void sequencerz_worker_stop(PlaylistWorker* worker) {
    furi_assert(worker);
    furi_assert(worker->is_running);

    worker->ctl_request_exit = true;
    furi_thread_join(worker->thread);
}

bool sequencerz_worker_running(PlaylistWorker* worker) {
    furi_assert(worker);
    return worker->is_running;
}

void sequencerz_worker_start(PlaylistWorker* instance, const char* file_path) {
    furi_assert(instance);
    furi_assert(!instance->is_running);

    string_set_str(instance->file_path, file_path);
    instance->is_running = true;

    // reset meta (current/total)
    sequencerz_meta_reset(instance->meta);

    furi_thread_start(instance->thread);
}

////////////////////////////////////////////////////////////////////////////////

static void render_callback(Canvas* canvas, void* ctx) {
    Playlist* app = ctx;
    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    switch(app->meta->state) {
    case STATE_NONE:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(
            canvas, WIDTH / 2, HEIGHT / 2, AlignCenter, AlignCenter, "No sequencerz loaded");
        break;

    case STATE_OVERVIEW:
        // draw file name
        {
            string_t sequencerz_name;
            string_init(sequencerz_name);
            path_extract_filename(app->file_path, sequencerz_name, true);

            canvas_set_font(canvas, FontPrimary);
            draw_centered_boxed_str(canvas, 1, 1, 15, 6, string_get_cstr(sequencerz_name));

            string_clear(sequencerz_name);
        }

        canvas_set_font(canvas, FontSecondary);

        // draw loaded count
        {
            string_t str;
            string_init_printf(str, "%d Items in sequencerz", app->meta->total_count);
            canvas_draw_str_aligned(canvas, 1, 19, AlignLeft, AlignTop, string_get_cstr(str));
            string_clear(str);
        }

        // draw buttons
        draw_corner_aligned(canvas, 40, 15, AlignCenter, AlignBottom);

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, WIDTH / 2 - 7, HEIGHT - 11, AlignLeft, AlignTop, "Start");
        canvas_draw_disc(canvas, WIDTH / 2 - 14, HEIGHT - 8, 3);

        canvas_set_color(canvas, ColorBlack);

        break;
    case STATE_SENDING:
        // draw status bar
        {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rframe(canvas, 1, HEIGHT - 12, WIDTH - 2, 11, 2);
            canvas_draw_rbox(canvas, 1, HEIGHT - 12, WIDTH - 2, 11, 2);

            // draw progress text
            string_t progress_text;
            string_init(progress_text);
            string_printf(
                progress_text, "%d/%d", app->meta->current_count, app->meta->total_count);

            canvas_set_color(canvas, ColorWhite);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(
                canvas,
                WIDTH / 2,
                HEIGHT - 3,
                AlignCenter,
                AlignBottom,
                string_get_cstr(progress_text));

            string_clear(progress_text);
        }

        // draw last and current file
        {
            canvas_set_color(canvas, ColorBlack);

            string_t path;
            string_init(path);

            canvas_set_font(canvas, FontSecondary);

            // current
            if(!string_empty_p(app->meta->prev_0_path)) {
                path_extract_filename(app->meta->prev_0_path, path, true);

                int w = canvas_string_width(canvas, string_get_cstr(path));
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, 1, 1, w + 4, 12, 2);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_str_aligned(canvas, 3, 3, AlignLeft, AlignTop, string_get_cstr(path));
                string_reset(path);
            }

            // last 3
            canvas_set_color(canvas, ColorBlack);

            if(!string_empty_p(app->meta->prev_1_path)) {
                path_extract_filename(app->meta->prev_1_path, path, true);
                canvas_draw_str_aligned(canvas, 3, 15, AlignLeft, AlignTop, string_get_cstr(path));
                string_reset(path);
            }

            if(!string_empty_p(app->meta->prev_2_path)) {
                path_extract_filename(app->meta->prev_2_path, path, true);
                canvas_draw_str_aligned(canvas, 3, 26, AlignLeft, AlignTop, string_get_cstr(path));
                string_reset(path);
            }

            if(!string_empty_p(app->meta->prev_3_path)) {
                path_extract_filename(app->meta->prev_3_path, path, true);
                canvas_draw_str_aligned(canvas, 3, 37, AlignLeft, AlignTop, string_get_cstr(path));
                string_reset(path);
            }

            string_clear(path);
        }

        // draw controls
        {
            const int ctl_w = 20;
            const int ctl_h = 20;

            const int disc_r = 5;
            const int box_l = 10;

            canvas_set_color(canvas, ColorBlack);

            // draw texts
            if(app->worker->ctl_pause) {
                canvas_draw_disc(
                    canvas, WIDTH - ctl_w / 2, HEIGHT / 2 - ctl_h / 2, disc_r);
            } else {
                canvas_draw_box(
                    canvas, WIDTH - ctl_w / 2 - box_l / 2, HEIGHT / 2 - ctl_h / 2 - box_l / 2, box_l, box_l);
            }
        }
        break;
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    Playlist* app = ctx;
    furi_message_queue_put(app->input_queue, event, 0);
}

////////////////////////////////////////////////////////////////////////////////

Playlist* sequencerz_alloc(DisplayMeta* meta) {
    Playlist* app = malloc(sizeof(Playlist));
    string_init(app->file_path);
    string_set_str(app->file_path, PLAYLIST_FOLDER);

    app->meta = meta;
    app->worker = NULL;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(32, sizeof(InputEvent));

    // view port
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    // gui
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

void sequencerz_start_worker(Playlist* app, DisplayMeta* meta) {
    app->worker = sequencerz_worker_alloc(meta);

    // count sequencerz items
    Storage* storage = furi_record_open(RECORD_STORAGE);
    app->meta->total_count =
        sequencerz_count_sequencerz_items(storage, string_get_cstr(app->file_path));
    furi_record_close(RECORD_STORAGE);

    // start thread
    sequencerz_worker_start(app->worker, string_get_cstr(app->file_path));
}

void sequencerz_free(Playlist* app) {
    string_clear(app->file_path);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);

    sequencerz_meta_free(app->meta);

    free(app);
}

int32_t sequencerz_app(void* p) {
    UNUSED(p);

    // create sequencerz folder
    {
        Storage* storage = furi_record_open(RECORD_STORAGE);

        if(!storage_simply_mkdir(storage, PLAYLIST_FOLDER)) {
            goto exit_cleanup;
        }

        furi_record_close(RECORD_STORAGE);
    }

    // create app
    DisplayMeta* meta = sequencerz_meta_alloc();
    Playlist* app = sequencerz_alloc(meta);
    meta->view_port = app->view_port;

    // select sequencerz file
    {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        const bool res = dialog_file_browser_show(
            dialogs, app->file_path, app->file_path, PLAYLIST_EXT, true, &I_sub1_10px, true);
        furi_record_close(RECORD_DIALOGS);
        // check if a file was selected
        if(!res) {
            goto exit_cleanup;
        }
    }

    ////////////////////////////////////////////////////////////////////////////////

    sequencerz_start_worker(app, meta);
    meta_set_state(app->meta, STATE_OVERVIEW);

    bool exit_loop = false;
    InputEvent input;
    while(1) { // close application if no file was selected
        furi_check(
            furi_message_queue_get(app->input_queue, &input, FuriWaitForever) == FuriStatusOk);

        switch(input.key) {
        case InputKeyOk:
            if(input.type == InputTypeShort) {
                // toggle pause state
                if(!app->worker->is_running) {
                    app->worker->ctl_pause = false;
                    app->worker->ctl_request_exit = false;
                    sequencerz_worker_start(app->worker, string_get_cstr(app->file_path));
                } else {
                    app->worker->ctl_pause = !app->worker->ctl_pause;
                }
            }
            break;
        case InputKeyBack:
            exit_loop = true;
            break;
        default:
            break;
        }

        furi_mutex_release(app->mutex);

        // exit application
        if(exit_loop == true) {
            break;
        }

        view_port_update(app->view_port);
    }

exit_cleanup:
    if(app->worker != NULL) {
        if(sequencerz_worker_running(app->worker)) {
            sequencerz_worker_stop(app->worker);
        }
        sequencerz_worker_free(app->worker);
    }

    sequencerz_free(app);
    return 0;
}
