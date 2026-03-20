#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#include "protocols.h"
#include "transmit.h"

#include <stdio.h>

typedef enum {
    FieldModel,
    FieldID,
    FieldChannel,
    FieldCommand,
    FieldIntensity,
    FieldCount,
} EditField;

typedef struct {
    ShockerModel model;
    uint16_t shocker_id;
    uint8_t channel;
    ShockerCommand command;
    uint8_t intensity;
    EditField selected_field;
    bool transmitting;
    FuriMutex* mutex;
} AppState;

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

static void ensure_valid_state(AppState* state) {
    uint8_t ch_max = max_channel(state->model);
    if(state->channel > ch_max) state->channel = ch_max;

    if(!openshock_command_supported(state->model, state->command)) {
        for(int i = 0; i < ShockerCmdCount; i++) {
            if(openshock_command_supported(state->model, (ShockerCommand)i)) {
                state->command = (ShockerCommand)i;
                return;
            }
        }
    }
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 11, "OpenShock");

    if(state->transmitting) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Transmitting...");
        furi_mutex_release(state->mutex);
        return;
    }

    canvas_set_font(canvas, FontSecondary);

    char buf[32];
    int y = 22;
    const int label_x = 0;
    const int value_x = 64;
    const int row_h = 10;

    if(state->selected_field == FieldModel)
        canvas_draw_str(canvas, label_x, y, "> Model:");
    else
        canvas_draw_str(canvas, label_x, y, "  Model:");
    canvas_draw_str(canvas, value_x, y, openshock_model_name(state->model));
    y += row_h;

    if(state->selected_field == FieldID)
        canvas_draw_str(canvas, label_x, y, "> ID:");
    else
        canvas_draw_str(canvas, label_x, y, "  ID:");
    snprintf(buf, sizeof(buf), "%u", state->shocker_id);
    canvas_draw_str(canvas, value_x, y, buf);
    y += row_h;

    if(state->selected_field == FieldChannel)
        canvas_draw_str(canvas, label_x, y, "> Channel:");
    else
        canvas_draw_str(canvas, label_x, y, "  Channel:");
    snprintf(buf, sizeof(buf), "%u", state->channel);
    canvas_draw_str(canvas, value_x, y, buf);
    y += row_h;

    if(state->selected_field == FieldCommand)
        canvas_draw_str(canvas, label_x, y, "> Command:");
    else
        canvas_draw_str(canvas, label_x, y, "  Command:");
    canvas_draw_str(canvas, value_x, y, openshock_command_name(state->command));
    y += row_h;

    if(state->selected_field == FieldIntensity)
        canvas_draw_str(canvas, label_x, y, "> Intensity:");
    else
        canvas_draw_str(canvas, label_x, y, "  Intensity:");
    snprintf(buf, sizeof(buf), "%u", state->intensity);
    canvas_draw_str(canvas, value_x, y, buf);

    furi_mutex_release(state->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, input_event, FuriWaitForever);
}

static void cycle_command_forward(AppState* state) {
    for(int i = 1; i < ShockerCmdCount; i++) {
        ShockerCommand next = (state->command + i) % ShockerCmdCount;
        if(openshock_command_supported(state->model, next)) {
            state->command = next;
            return;
        }
    }
}

static void cycle_command_backward(AppState* state) {
    for(int i = ShockerCmdCount - 1; i >= 1; i--) {
        ShockerCommand prev = (state->command + i) % ShockerCmdCount;
        if(openshock_command_supported(state->model, prev)) {
            state->command = prev;
            return;
        }
    }
}

static uint16_t id_step(InputType type) {
    return (type == InputTypeRepeat) ? 100 : 1;
}

static uint8_t intensity_step(InputType type) {
    return (type == InputTypeRepeat) ? 5 : 1;
}

static void handle_input(AppState* state, InputEvent* event) {
    if(event->type != InputTypePress && event->type != InputTypeRepeat) return;

    switch(event->key) {
    case InputKeyUp:
        if(state->selected_field > 0) state->selected_field--;
        break;
    case InputKeyDown:
        if(state->selected_field < FieldCount - 1) state->selected_field++;
        break;
    case InputKeyLeft:
        switch(state->selected_field) {
        case FieldModel:
            state->model =
                (state->model == 0) ? (ShockerModelCount - 1) : (state->model - 1);
            ensure_valid_state(state);
            break;
        case FieldID: {
            uint16_t step = id_step(event->type);
            state->shocker_id = (state->shocker_id >= step) ?
                                    (state->shocker_id - step) :
                                    (65535 - step + 1 + state->shocker_id);
            break;
        }
        case FieldChannel: {
            uint8_t ch_max = max_channel(state->model);
            state->channel = (state->channel == 0) ? ch_max : (state->channel - 1);
            break;
        }
        case FieldCommand:
            cycle_command_backward(state);
            break;
        case FieldIntensity: {
            uint8_t step = intensity_step(event->type);
            state->intensity = (state->intensity >= step) ?
                                   (state->intensity - step) :
                                   (100 - step + 1 + state->intensity);
            break;
        }
        default:
            break;
        }
        break;
    case InputKeyRight:
        switch(state->selected_field) {
        case FieldModel:
            state->model = (state->model + 1) % ShockerModelCount;
            ensure_valid_state(state);
            break;
        case FieldID: {
            uint16_t step = id_step(event->type);
            uint32_t next = (uint32_t)state->shocker_id + step;
            state->shocker_id =
                (next <= 65535) ? (uint16_t)next : (uint16_t)(next - 65536);
            break;
        }
        case FieldChannel: {
            uint8_t ch_max = max_channel(state->model);
            state->channel = (state->channel >= ch_max) ? 0 : (state->channel + 1);
            break;
        }
        case FieldCommand:
            cycle_command_forward(state);
            break;
        case FieldIntensity: {
            uint8_t step = intensity_step(event->type);
            uint16_t next = (uint16_t)state->intensity + step;
            state->intensity = (next <= 100) ? (uint8_t)next : (uint8_t)(next - 101);
            break;
        }
        default:
            break;
        }
        break;
    default:
        break;
    }
}

int32_t openshock_app(void* p) {
    UNUSED(p);

    AppState state = {
        .model = ShockerModelCaiXianlin,
        .shocker_id = 0,
        .channel = 0,
        .command = ShockerCmdVibrate,
        .intensity = 0,
        .selected_field = FieldModel,
        .transmitting = false,
        .mutex = furi_mutex_alloc(FuriMutexTypeNormal),
    };

    OpenshockTx* tx = openshock_tx_alloc();
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, &state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(event_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.key == InputKeyBack) {
            if(event.type == InputTypePress || event.type == InputTypeShort) {
                running = false;
            }
            continue;
        }

        if(event.key == InputKeyOk) {
            if(event.type == InputTypePress) {
                // Set UI to transmitting immediately
                furi_mutex_acquire(state.mutex, FuriWaitForever);
                state.transmitting = true;
                furi_mutex_release(state.mutex);
                view_port_update(view_port);

                // Encode and kick off TX in background thread
                OokPulse pulses[OPENSHOCK_MAX_PULSES];
                size_t count = openshock_encode(
                    state.model,
                    state.shocker_id,
                    state.command,
                    state.intensity,
                    state.channel,
                    pulses,
                    OPENSHOCK_MAX_PULSES);

                if(count > 0) {
                    openshock_tx_start(tx, pulses, count);
                }
            } else if(event.type == InputTypeRelease) {
                openshock_tx_stop(tx);

                furi_mutex_acquire(state.mutex, FuriWaitForever);
                state.transmitting = false;
                furi_mutex_release(state.mutex);
                view_port_update(view_port);
            }
            continue;
        }

        furi_mutex_acquire(state.mutex, FuriWaitForever);
        handle_input(&state, &event);
        furi_mutex_release(state.mutex);
        view_port_update(view_port);
    }

    openshock_tx_free(tx);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(state.mutex);
    furi_record_close(RECORD_GUI);

    return 0;
}
