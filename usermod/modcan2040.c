#include <string.h>
#include "py/runtime.h"
#include "py/obj.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "can2040.h"

#define RX_BUF_SIZE 128

static struct can2040 cbus;
static bool started = false;
static uint32_t active_pio = 0;
static int active_tx = -1;

static struct can2040_msg rx_buf[RX_BUF_SIZE];
static volatile uint32_t rx_head = 0, rx_tail = 0;
static volatile uint32_t rx_dropped = 0, tx_count = 0, err_count = 0;

// Runs in interrupt context: no allocation, only write into the ring buffer
static void can_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
    if (notify & CAN2040_NOTIFY_RX) {
        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = *msg;
            rx_head = next;
        } else {
            rx_dropped++;
        }
    } else if (notify & CAN2040_NOTIFY_TX) {
        tx_count++;
    } else if (notify & CAN2040_NOTIFY_ERROR) {
        err_count++;
    }
}

static void pio_irq_handler(void) {
    can2040_pio_irq_handler(&cbus);
}

// can2040.start(rx, tx, bitrate=500000, pio=0)
static mp_obj_t mod_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_rx, ARG_tx, ARG_bitrate, ARG_pio };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_rx,      MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_tx,      MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_bitrate, MP_ARG_INT, {.u_int = 500000} },
        { MP_QSTR_pio,     MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);

    uint32_t pio = a[ARG_pio].u_int;
    if (pio > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("pio must be 0 or 1"));
    }
    uint irq = (pio == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;

    if (started) {  // e.g. after a soft reset: remove the old handler first
        irq_set_enabled(irq, false);
        irq_remove_handler(irq, pio_irq_handler);
    }
    rx_head = rx_tail = 0;

    can2040_setup(&cbus, pio);
    can2040_callback_config(&cbus, can_cb);
    irq_set_exclusive_handler(irq, pio_irq_handler);
    irq_set_priority(irq, 0x40);
    irq_set_enabled(irq, true);
    can2040_start(&cbus, clock_get_hz(clk_sys), a[ARG_bitrate].u_int,
                  a[ARG_rx].u_int, a[ARG_tx].u_int);
    started = true;
    active_pio = pio;
    active_tx = a[ARG_tx].u_int;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_start_obj, 0, mod_start);

// can2040.stop(): switch CAN off, release the bus (TX pin high = recessive)
static mp_obj_t mod_stop(void) {
    if (started) {
        uint irq = (active_pio == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
        irq_set_enabled(irq, false);
        irq_remove_handler(irq, pio_irq_handler);
        PIO pio = (active_pio == 0) ? pio0 : pio1;
        pio_set_sm_mask_enabled(pio, 0xF, false);
        gpio_init(active_tx);
        gpio_set_dir(active_tx, GPIO_OUT);
        gpio_put(active_tx, 1);
        started = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_stop_obj, mod_stop);

// can2040.send(id, data=None, extended=False, rtr=False) -> True/False
static mp_obj_t mod_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_id, ARG_data, ARG_ext, ARG_rtr };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_id,       MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_data,     MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_extended, MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_rtr,      MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);

    if (!started) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("call start() first"));
    }
    struct can2040_msg m;
    memset(&m, 0, sizeof(m));
    mp_buffer_info_t buf = {0};
    if (a[ARG_data].u_obj != mp_const_none) {
        mp_get_buffer_raise(a[ARG_data].u_obj, &buf, MP_BUFFER_READ);
        if (buf.len > 8) {
            mp_raise_ValueError(MP_ERROR_TEXT("max 8 data bytes"));
        }
        memcpy(m.data, buf.buf, buf.len);
    }
    m.dlc = buf.len;
    m.id = a[ARG_id].u_int;
    if (a[ARG_ext].u_bool) m.id |= CAN2040_ID_EFF;
    if (a[ARG_rtr].u_bool) m.id |= CAN2040_ID_RTR;
    return mp_obj_new_bool(can2040_transmit(&cbus, &m) == 0);  // False = TX queue full
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_send_obj, 0, mod_send);

// can2040.recv() -> None or (id, data, extended, rtr)
static mp_obj_t mod_recv(void) {
    if (rx_head == rx_tail) {
        return mp_const_none;
    }
    struct can2040_msg m = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;

    bool ext = m.id & CAN2040_ID_EFF;
    bool rtr = m.id & CAN2040_ID_RTR;
    uint32_t id = m.id & (ext ? 0x1FFFFFFFu : 0x7FFu);
    uint32_t dlc = m.dlc > 8 ? 8 : m.dlc;

    mp_obj_t t[4] = {
        mp_obj_new_int_from_uint(id),
        mp_obj_new_bytes(m.data, dlc),
        mp_obj_new_bool(ext),
        mp_obj_new_bool(rtr),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_recv_obj, mod_recv);

// can2040.stats() -> (tx_ok, errors, rx_dropped)
static mp_obj_t mod_stats(void) {
    mp_obj_t t[3] = {
        mp_obj_new_int_from_uint(tx_count),
        mp_obj_new_int_from_uint(err_count),
        mp_obj_new_int_from_uint(rx_dropped),
    };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_stats_obj, mod_stats);

static const mp_rom_map_elem_t can2040_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_can2040) },
    { MP_ROM_QSTR(MP_QSTR_start),    MP_ROM_PTR(&mod_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),     MP_ROM_PTR(&mod_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),     MP_ROM_PTR(&mod_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),     MP_ROM_PTR(&mod_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats),    MP_ROM_PTR(&mod_stats_obj) },
};
static MP_DEFINE_CONST_DICT(can2040_globals, can2040_globals_table);

const mp_obj_module_t can2040_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&can2040_globals,
};
MP_REGISTER_MODULE(MP_QSTR_can2040, can2040_user_cmodule);
