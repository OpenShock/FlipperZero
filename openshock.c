#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#include "protocols.h"
#include "transmit.h"
#include "receiver.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>

// --- Types ---

typedef enum {
    ScreenMenu,
    ScreenEdit,
    ScreenSaved,
    ScreenReceive,
} Screen;

typedef enum {
    MenuTransmit,
    MenuSaved,
    MenuReceive,
    MenuCount,
} MenuItem;

typedef enum {
    FieldModel,
    FieldID,
    FieldChannel,
    FieldCommand,
    FieldIntensity,
    FieldCount,
} EditField;

typedef struct {
    // Screen
    Screen screen;
    MenuItem menu_sel;

    // Edit state
    ShockerModel model;
    uint16_t shocker_id;
    uint8_t channel;
    ShockerCommand command;
    uint8_t intensity;
    EditField selected_field;
    bool transmitting;
    uint32_t save_flash_tick; // non-zero = show "Saved!" until this tick

    // Saved list
    SavedShocker saved[OPENSHOCK_MAX_SAVED];
    size_t saved_count;
    int saved_sel;

    // Receive
    bool rx_listening;
    bool rx_found;
    DecodedShocker rx_result;

    // Shared
    FuriMutex* mutex;
    OpenshockTx* tx;
    OpenshockRx* rx;
} AppState;

// --- Helpers ---

static uint8_t max_channel(ShockerModel model) {
    switch(model) {
    case ShockerModelCaiXianlin:
        return 15;
    case ShockerModelPetrainer:
        return 0;
    case ShockerModelPetrainer998DR:
        return 1;
    case ShockerModelT330:
        return 1;
    case ShockerModelD80:
        return 2;
    default:
        return 0;
    }
}

static void ensure_valid_state(AppState* s) {
    uint8_t ch_max = max_channel(s->model);
    if(s->channel > ch_max) s->channel = ch_max;
    if(!openshock_command_supported(s->model, s->command)) {
        for(int i = 0; i < ShockerCmdCount; i++) {
            if(openshock_command_supported(s->model, (ShockerCommand)i)) {
                s->command = (ShockerCommand)i;
                break;
            }
        }
    }
}

static const char* menu_label(MenuItem item) {
    switch(item) {
    case MenuTransmit:
        return "Transmit";
    case MenuSaved:
        return "Saved Shockers";
    case MenuReceive:
        return "Receive";
    default:
        return "";
    }
}

// --- Draw ---

static void draw_menu(Canvas* canvas, AppState* s) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 11, "OpenShock");
    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < MenuCount; i++) {
        int y = 24 + i * 12;
        if(i == (int)s->menu_sel)
            canvas_draw_str(canvas, 4, y, ">");
        canvas_draw_str(canvas, 14, y, menu_label((MenuItem)i));
    }
}

static void draw_edit(Canvas* canvas, AppState* s) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 11, "OpenShock");

    if(s->transmitting) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Transmitting...");
        return;
    }

    if(s->save_flash_tick && furi_get_tick() < s->save_flash_tick) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Saved!");
        return;
    }
    s->save_flash_tick = 0;

    canvas_set_font(canvas, FontSecondary);
    char buf[40];
    const int rh = 10;
    const int max_visible = 4;
    const int content_y = 20;
    const int vx = 56;

    // Scroll window to keep selection visible
    int scroll = 0;
    if((int)s->selected_field >= max_visible)
        scroll = (int)s->selected_field - max_visible + 1;

    const char* labels[] = {"Model:", "ID:", "Ch:", "Cmd:", "Int:"};
    for(int vi = 0; vi < max_visible; vi++) {
        int i = scroll + vi;
        if(i >= FieldCount) break;
        int y = content_y + vi * rh;

        const char* prefix = (i == (int)s->selected_field) ? ">" : " ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, labels[i]);
        canvas_draw_str(canvas, 0, y, buf);

        switch(i) {
        case FieldModel:
            canvas_draw_str(canvas, vx, y, openshock_model_name(s->model));
            break;
        case FieldID:
            snprintf(buf, sizeof(buf), "%u", s->shocker_id);
            canvas_draw_str(canvas, vx, y, buf);
            break;
        case FieldChannel:
            snprintf(buf, sizeof(buf), "%u", s->channel);
            canvas_draw_str(canvas, vx, y, buf);
            break;
        case FieldCommand:
            canvas_draw_str(canvas, vx, y, openshock_command_name(s->command));
            break;
        case FieldIntensity:
            snprintf(buf, sizeof(buf), "%u", s->intensity);
            canvas_draw_str(canvas, vx, y, buf);
            break;
        }
    }

    // Scroll indicators
    if(scroll > 0) canvas_draw_str(canvas, 122, content_y, "^");
    if(scroll + max_visible < FieldCount) canvas_draw_str(canvas, 122, content_y + (max_visible - 1) * rh, "v");

    canvas_draw_str(canvas, 0, 64, "Hold OK:TX  Long OK:Save");
}

static void draw_saved(Canvas* canvas, AppState* s) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 11, "Saved Shockers");
    canvas_set_font(canvas, FontSecondary);

    if(s->saved_count == 0) {
        canvas_draw_str(canvas, 4, 30, "No saved shockers");
        canvas_draw_str(canvas, 0, 64, "Back:Menu");
        return;
    }

    // Show up to 4 visible items
    int visible_start = 0;
    if(s->saved_sel > 3) visible_start = s->saved_sel - 3;

    for(int i = visible_start; i < (int)s->saved_count && i < visible_start + 4; i++) {
        int y = 22 + (i - visible_start) * 10;
        char line[48];
        snprintf(
            line,
            sizeof(line),
            "%s %s #%u",
            (i == s->saved_sel) ? ">" : " ",
            openshock_model_name(s->saved[i].model),
            s->saved[i].shocker_id);
        canvas_draw_str(canvas, 0, y, line);
    }

    canvas_draw_str(canvas, 0, 64, "OK:Load  Left:Del  Back:Menu");
}

static void draw_receive(Canvas* canvas, AppState* s) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 11, "Receive");

    if(s->save_flash_tick && furi_get_tick() < s->save_flash_tick) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Saved!");
        return;
    }
    s->save_flash_tick = 0;

    canvas_set_font(canvas, FontSecondary);

    if(!s->rx_found) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Listening...");
    } else {
        char buf[48];
        int y = 24;
        snprintf(buf, sizeof(buf), "Model: %s", openshock_model_name(s->rx_result.model));
        canvas_draw_str(canvas, 4, y, buf);
        y += 10;
        snprintf(buf, sizeof(buf), "ID: %u  CH: %u", s->rx_result.shocker_id, s->rx_result.channel);
        canvas_draw_str(canvas, 4, y, buf);
        y += 10;
        snprintf(
            buf,
            sizeof(buf),
            "Cmd: %s  Int: %u",
            openshock_command_name(s->rx_result.command),
            s->rx_result.intensity);
        canvas_draw_str(canvas, 4, y, buf);
    }

    const char* hint = s->rx_found ? "OK:Use  Right:Save  Back:Menu" : "Back:Menu";
    canvas_draw_str(canvas, 0, 64, hint);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* s = ctx;
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    canvas_clear(canvas);

    switch(s->screen) {
    case ScreenMenu:
        draw_menu(canvas, s);
        break;
    case ScreenEdit:
        draw_edit(canvas, s);
        break;
    case ScreenSaved:
        draw_saved(canvas, s);
        break;
    case ScreenReceive:
        draw_receive(canvas, s);
        break;
    }

    furi_mutex_release(s->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

// --- Input handling per screen ---

static uint16_t id_step(InputType type) {
    return (type == InputTypeRepeat) ? 100 : 1;
}
static uint8_t intensity_step(InputType type) {
    return (type == InputTypeRepeat) ? 5 : 1;
}

static void cycle_cmd_fwd(AppState* s) {
    for(int i = 1; i < ShockerCmdCount; i++) {
        ShockerCommand n = (s->command + i) % ShockerCmdCount;
        if(openshock_command_supported(s->model, n)) {
            s->command = n;
            return;
        }
    }
}
static void cycle_cmd_back(AppState* s) {
    for(int i = ShockerCmdCount - 1; i >= 1; i--) {
        ShockerCommand n = (s->command + i) % ShockerCmdCount;
        if(openshock_command_supported(s->model, n)) {
            s->command = n;
            return;
        }
    }
}

// Returns true if app should exit
static bool handle_menu(AppState* s, InputEvent* e) {
    if(e->type == InputTypePress || e->type == InputTypeRepeat) {
        switch(e->key) {
        case InputKeyUp:
            s->menu_sel = (s->menu_sel == 0) ? (MenuCount - 1) : (s->menu_sel - 1);
            break;
        case InputKeyDown:
            s->menu_sel = (s->menu_sel + 1) % MenuCount;
            break;
        case InputKeyOk:
            switch(s->menu_sel) {
            case MenuTransmit:
                s->screen = ScreenEdit;
                break;
            case MenuSaved:
                s->saved_count = openshock_shocker_list(s->saved, OPENSHOCK_MAX_SAVED);
                s->saved_sel = 0;
                s->screen = ScreenSaved;
                break;
            case MenuReceive:
                s->rx_found = false;
                s->screen = ScreenReceive;
                openshock_rx_start(s->rx);
                break;
            default:
                break;
            }
            break;
        case InputKeyBack:
            return true; // exit app
        default:
            break;
        }
    }
    return false;
}

static void handle_edit_field(AppState* s, InputKey key, InputType type) {
    switch(s->selected_field) {
    case FieldModel:
        if(key == InputKeyLeft)
            s->model = (s->model == 0) ? (ShockerModelCount - 1) : (s->model - 1);
        else
            s->model = (s->model + 1) % ShockerModelCount;
        ensure_valid_state(s);
        break;
    case FieldID: {
        uint16_t step = id_step(type);
        if(key == InputKeyLeft)
            s->shocker_id =
                (s->shocker_id >= step) ? (s->shocker_id - step) :
                                          (uint16_t)(65536 - step + s->shocker_id);
        else {
            uint32_t n = (uint32_t)s->shocker_id + step;
            s->shocker_id = (n <= 65535) ? (uint16_t)n : (uint16_t)(n - 65536);
        }
        break;
    }
    case FieldChannel: {
        uint8_t mx = max_channel(s->model);
        if(key == InputKeyLeft)
            s->channel = (s->channel == 0) ? mx : (s->channel - 1);
        else
            s->channel = (s->channel >= mx) ? 0 : (s->channel + 1);
        break;
    }
    case FieldCommand:
        if(key == InputKeyLeft)
            cycle_cmd_back(s);
        else
            cycle_cmd_fwd(s);
        break;
    case FieldIntensity: {
        uint8_t step = intensity_step(type);
        if(key == InputKeyLeft)
            s->intensity =
                (s->intensity >= step) ? (s->intensity - step) :
                                         (uint8_t)(101 - step + s->intensity);
        else {
            uint16_t n = (uint16_t)s->intensity + step;
            s->intensity = (n <= 100) ? (uint8_t)n : (uint8_t)(n - 101);
        }
        break;
    }
    default:
        break;
    }
}

static void
    handle_edit(AppState* s, InputEvent* e, OpenshockTx* tx, OpenshockTx** tx_active) {
    // OK press: start TX
    if(e->key == InputKeyOk && e->type == InputTypePress) {
        s->transmitting = true;

        OokPulse pulses[OPENSHOCK_MAX_PULSES];
        size_t count = openshock_encode(
            s->model, s->shocker_id, s->command, s->intensity, s->channel, pulses,
            OPENSHOCK_MAX_PULSES);
        if(count > 0) {
            openshock_tx_start(tx, pulses, count);
            *tx_active = tx;
        }
        return;
    }

    // OK release: stop TX
    if(e->key == InputKeyOk && e->type == InputTypeRelease && *tx_active) {
        openshock_tx_stop(tx);
        *tx_active = NULL;
        s->transmitting = false;
        return;
    }

    // OK long: save
    if(e->key == InputKeyOk && e->type == InputTypeLong) {
        if(*tx_active) {
            openshock_tx_stop(tx);
            *tx_active = NULL;
            s->transmitting = false;
        }
        char name[32];
        snprintf(name, sizeof(name), "%s_%u", openshock_model_name(s->model), s->shocker_id);
        openshock_shocker_save(name, s->model, s->shocker_id, s->channel);
        s->save_flash_tick = furi_get_tick() + 1000;
        return;
    }

    // Back: return to menu
    if(e->key == InputKeyBack && (e->type == InputTypePress || e->type == InputTypeShort)) {
        if(*tx_active) {
            openshock_tx_stop(tx);
            *tx_active = NULL;
            s->transmitting = false;
        }
        s->screen = ScreenMenu;
        return;
    }

    if(e->type != InputTypePress && e->type != InputTypeRepeat) return;

    switch(e->key) {
    case InputKeyUp:
        if(s->selected_field > 0) s->selected_field--;
        break;
    case InputKeyDown:
        if(s->selected_field < FieldCount - 1) s->selected_field++;
        break;
    case InputKeyLeft:
    case InputKeyRight:
        handle_edit_field(s, e->key, e->type);
        break;
    default:
        break;
    }
}

static void handle_saved(AppState* s, InputEvent* e) {
    if(e->type != InputTypePress && e->type != InputTypeRepeat) return;

    switch(e->key) {
    case InputKeyUp:
        if(s->saved_sel > 0) s->saved_sel--;
        break;
    case InputKeyDown:
        if(s->saved_sel < (int)s->saved_count - 1) s->saved_sel++;
        break;
    case InputKeyOk:
        if(s->saved_count > 0) {
            SavedShocker* sel = &s->saved[s->saved_sel];
            s->model = sel->model;
            s->shocker_id = sel->shocker_id;
            s->channel = sel->channel;
            ensure_valid_state(s);
            s->screen = ScreenEdit;
        }
        break;
    case InputKeyLeft:
        // Delete selected
        if(s->saved_count > 0) {
            openshock_shocker_delete(s->saved[s->saved_sel].filename);
            s->saved_count = openshock_shocker_list(s->saved, OPENSHOCK_MAX_SAVED);
            if(s->saved_sel >= (int)s->saved_count && s->saved_sel > 0) s->saved_sel--;
        }
        break;
    case InputKeyBack:
        s->screen = ScreenMenu;
        break;
    default:
        break;
    }
}

static void handle_receive(AppState* s, InputEvent* e) {
    if(e->type != InputTypePress && e->type != InputTypeShort) return;

    switch(e->key) {
    case InputKeyOk:
        if(s->rx_found) {
            // Load decoded values into editor
            s->model = s->rx_result.model;
            s->shocker_id = s->rx_result.shocker_id;
            s->channel = s->rx_result.channel;
            s->command = s->rx_result.command;
            s->intensity = s->rx_result.intensity;
            ensure_valid_state(s);
            openshock_rx_stop(s->rx);
            s->rx_listening = false;
            s->screen = ScreenEdit;
        }
        break;
    case InputKeyRight:
        if(s->rx_found) {
            char name[32];
            snprintf(
                name,
                sizeof(name),
                "%s_%u",
                openshock_model_name(s->rx_result.model),
                s->rx_result.shocker_id);
            openshock_shocker_save(
                name, s->rx_result.model, s->rx_result.shocker_id, s->rx_result.channel);
            s->save_flash_tick = furi_get_tick() + 1000;
        }
        break;
    case InputKeyBack:
        openshock_rx_stop(s->rx);
        s->rx_listening = false;
        s->screen = ScreenMenu;
        break;
    default:
        break;
    }
}

// --- Main ---

int32_t openshock_app(void* p) {
    UNUSED(p);

    AppState state;
    memset(&state, 0, sizeof(state));
    state.screen = ScreenMenu;
    state.command = ShockerCmdVibrate;
    state.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state.tx = openshock_tx_alloc();
    state.rx = openshock_rx_alloc();

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, &state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    OpenshockTx* tx_active = NULL;
    bool running = true;

    while(running) {
        // Poll for RX results when in receive mode
        if(state.screen == ScreenReceive) {
            DecodedShocker decoded;
            if(openshock_rx_get_result(state.rx, &decoded)) {
                furi_mutex_acquire(state.mutex, FuriWaitForever);
                state.rx_result = decoded;
                state.rx_found = true;
                furi_mutex_release(state.mutex);
                view_port_update(view_port);
            }
        }

        FuriStatus status = furi_message_queue_get(event_queue, &event, 50);
        if(status != FuriStatusOk) {
            view_port_update(view_port); // Refresh for timed UI updates (save flash, etc.)
            continue;
        }

        furi_mutex_acquire(state.mutex, FuriWaitForever);

        switch(state.screen) {
        case ScreenMenu:
            running = !handle_menu(&state, &event);
            break;
        case ScreenEdit:
            handle_edit(&state, &event, state.tx, &tx_active);
            break;
        case ScreenSaved:
            handle_saved(&state, &event);
            break;
        case ScreenReceive:
            handle_receive(&state, &event);
            break;
        }

        furi_mutex_release(state.mutex);
        view_port_update(view_port);
    }

    // Cleanup
    if(tx_active) openshock_tx_stop(state.tx);
    if(state.rx_listening) openshock_rx_stop(state.rx);

    openshock_tx_free(state.tx);
    openshock_rx_free(state.rx);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(state.mutex);
    furi_record_close(RECORD_GUI);

    return 0;
}
