#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "pico/platform.h"
#include "pico/multicore.h"
#include "ps1_mc_data_interface.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ps1_mc_spi.pio.h"
#include "debug.h"
#include "ps1/ps1_memory_card.h"
#include "game_db/game_db.h"

#include "hardware/structs/iobank0.h"

static uint64_t us_startup;

static size_t byte_count;
#if LOG_LEVEL_PS1_MC == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS1_MC, level, fmt, ##x)
#endif


static uint64_t us_startup;
static volatile int reset;
static uint8_t flag;
static uint8_t* curr_page = NULL;

static char received_game_id[0x10];
static volatile uint8_t mc_pro_command;


typedef struct {
    uint32_t offset;
    uint32_t sm;
} pio_t;

static pio_t cmd_reader, dat_writer, cntrl_reader;
static volatile int mc_exit_request, mc_exit_response, mc_enter_request, mc_enter_response;

enum { RECEIVE_RESET, RECEIVE_EXIT, RECEIVE_OK };


static void __time_critical_func(reset_pio)(void) {
    pio_set_sm_mask_enabled(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << cntrl_reader.sm), false);
    pio_restart_sm_mask(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << cntrl_reader.sm));

    pio_sm_exec(pio0, cmd_reader.sm, pio_encode_jmp(cmd_reader.offset));
    pio_sm_exec(pio0, dat_writer.sm, pio_encode_jmp(dat_writer.offset));
    pio_sm_exec(pio0, cntrl_reader.sm, pio_encode_jmp(cntrl_reader.offset));

    pio_sm_clear_fifos(pio0, cmd_reader.sm);
    pio_sm_clear_fifos(pio0, cntrl_reader.sm);
    pio_sm_drain_tx_fifo(pio0, dat_writer.sm);

    pio_enable_sm_mask_in_sync(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << cntrl_reader.sm));

    reset = 1;
}

static void __time_critical_func(init_pio)(void) {
    /* Set all pins as floating inputs */
    gpio_set_dir(PIN_PSX_ACK, 0);
    gpio_set_dir(PIN_PSX_SEL, 0);
    gpio_set_dir(PIN_PSX_CLK, 0);
    gpio_set_dir(PIN_PSX_CMD, 0);
    gpio_set_dir(PIN_PSX_DAT, 0);
    gpio_disable_pulls(PIN_PSX_ACK);
    gpio_disable_pulls(PIN_PSX_SEL);
    gpio_disable_pulls(PIN_PSX_CLK);
    gpio_disable_pulls(PIN_PSX_CMD);
    gpio_disable_pulls(PIN_PSX_DAT);

    cmd_reader.offset = pio_add_program(pio0, &cmd_reader_program);
    cmd_reader.sm = pio_claim_unused_sm(pio0, true);

    dat_writer.offset = pio_add_program(pio0, &dat_writer_program);
    dat_writer.sm = pio_claim_unused_sm(pio0, true);

    cntrl_reader.offset = pio_add_program(pio0, &cmd_reader_program);
    cntrl_reader.sm = pio_claim_unused_sm(pio0, true);

    cmd_reader_program_init(pio0, cmd_reader.sm, cmd_reader.offset);
    dat_writer_program_init(pio0, dat_writer.sm, dat_writer.offset);
    controller_program_init(pio0, cntrl_reader.sm, cntrl_reader.offset);
}

static void __time_critical_func(card_deselected)(uint gpio, uint32_t event_mask) {
    if (gpio == PIN_PSX_SEL && (event_mask & GPIO_IRQ_EDGE_RISE))
        reset_pio();
}

static uint8_t __time_critical_func(recv_cmd)(uint8_t* cmd) {
    while (pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm) && pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm))  {
        if (mc_exit_request)
            return RECEIVE_EXIT;
        if (reset)
            return RECEIVE_RESET;
    }
    *cmd = (pio_sm_get(pio0, cmd_reader.sm) >> 24);
    return RECEIVE_OK;
}


static uint8_t __time_critical_func(recv_cntrl)(uint8_t* cmd) {
    while (pio_sm_is_rx_fifo_empty(pio0, cntrl_reader.sm) && pio_sm_is_rx_fifo_empty(pio0, cntrl_reader.sm))  {
        if (mc_exit_request)
            return RECEIVE_EXIT;
        if (reset)
            return RECEIVE_RESET;
    }
    *cmd = (pio_sm_get(pio0, cntrl_reader.sm) >> 24);
    return RECEIVE_OK;
}

#define receiveOrNextCmd(cmd)          \
    if (recv_cmd(cmd) == RECEIVE_RESET) \
    return;


static void __time_critical_func(ps1_mc_respond)(uint8_t ch) {
    pio_sm_put_blocking(pio0, dat_writer.sm, ~ch & 0xFF);
}
/*
  Send Reply Comment
  81h  N/A   Memory card address
  53h  FLAG  Send Get ID Command (ASCII "S"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  00h  5Ch   Receive Command Acknowledge 1
  00h  5Dh   Receive Command Acknowledge 2
  00h  04h   Receive 04h
  00h  00h   Receive 00h
  00h  00h   Receive 00h
  00h  80h   Receive 80h
   */

static void __time_critical_func(mc_cmd_get_card_id)(void) {
    uint8_t _;
    log(LOG_TRACE, "PS1:    Card ID\n");
    ps1_mc_respond(0x5A); receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D); receiveOrNextCmd(&_);
    ps1_mc_respond(0x5C); receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D); receiveOrNextCmd(&_);
    ps1_mc_respond(0x04); receiveOrNextCmd(&_);
    ps1_mc_respond(0x00); receiveOrNextCmd(&_);
    ps1_mc_respond(0x00); receiveOrNextCmd(&_);
    ps1_mc_respond(0x80); receiveOrNextCmd(&_);
}

/*
  Send Reply Comment
  81h  N/A   Memory card address
  52h  FLAG  Send Read Command (ASCII "R"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
  LSB  (pre) Send Address LSB  ;/
  00h  5Ch   Receive Command Acknowledge 1  ;<-- late /ACK after this byte-pair
  00h  5Dh   Receive Command Acknowledge 2
  00h  MSB   Receive Confirmed Address MSB
  00h  LSB   Receive Confirmed Address LSB
  00h  ...   Receive Data Sector (128 bytes)
  00h  CHK   Receive Checksum (MSB xor LSB xor Data bytes)
  00h  47h   Receive Memory End Byte (should be always 47h="G"=Good for Read)
  */
static void __time_critical_func(mc_cmd_read)(bool long_read) {
    uint8_t page_msb = 0U, page_lsb = 0U;
    uint8_t offset = 0;
    uint8_t chk = 0;
    uint16_t page = 0U;
    uint8_t _;

    ps1_mc_respond(0x5A);       receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D);       receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);       receiveOrNextCmd(&page_msb);
    ps1_mc_respond(page_msb);   receiveOrNextCmd(&page_lsb);
    chk = page_msb ^ page_lsb;
    ps1_mc_respond(0x5C);       receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D);       receiveOrNextCmd(&_);
    ps1_mc_respond(page_msb);   receiveOrNextCmd(&_);
    page = (page_msb << 8) | page_lsb;
    log(LOG_TRACE, "PS1:    Read page %u\n", page);
    ps1_mc_data_interface_setup_read_page(page);
    ps1_mc_respond(page_lsb);   receiveOrNextCmd(&_);
    do {
        curr_page = ps1_mc_data_interface_get_page(page);
        for (offset = 0; offset < 128; offset++) {
            ps1_mc_data_interface_wait_for_byte(offset);
            ps1_mc_respond(curr_page[offset]);
            chk ^= curr_page[offset];
            receiveOrNextCmd(&_)
        }
        page++;
        ps1_mc_data_interface_setup_read_page(page);
    } while (long_read);
    ps1_mc_respond(chk);        receiveOrNextCmd(&_);
    ps1_mc_respond(0x47);
    curr_page = NULL;
}


/**
  Send Reply Comment
  81h  N/A   Memory card address
  57h  FLAG  Send Write Command (ASCII "W"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
  LSB  (pre) Send Address LSB  ;/
  ...  (pre) Send Data Sector (128 bytes)
  CHK  (pre) Send Checksum (MSB xor LSB xor Data bytes)
  00h  5Ch   Receive Command Acknowledge 1
  00h  5Dh   Receive Command Acknowledge 2
  00h  4xh   Receive Memory End Byte (47h=Good, 4Eh=BadChecksum, FFh=BadSector)
   */
static void __time_critical_func(mc_cmd_write)(void) {
    uint8_t page_msb = 0U, page_lsb = 0U;
    uint8_t offset = 0;
    uint8_t in = 0;
    uint8_t prev;
    uint8_t chk = 0;
    uint8_t _;
    log(LOG_TRACE, "PS1:    Write command\n");

    flag = 0;

    ps1_mc_respond(0x5A);       receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D);       receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);       receiveOrNextCmd(&page_msb);
    ps1_mc_respond(page_msb);   receiveOrNextCmd(&page_lsb);
    chk = page_msb ^ page_lsb;
    prev = page_lsb;
    for (offset = 0; offset < 128; offset++) {
        ps1_mc_respond(prev);   receiveOrNextCmd(&in);
        ps1_mc_data_interface_write_byte(((page_msb * 256) + page_lsb) * 128 + offset, in);
        chk ^= in;
        prev = in;
    }
    ps1_mc_respond(prev);           receiveOrNextCmd(&in);
    ps1_mc_respond(0x5C);           receiveOrNextCmd(&_);
    ps1_mc_respond(0x5D);           receiveOrNextCmd(&_);
    if (in == chk) {
        ps1_mc_data_interface_write_mc((page_msb * 256) + page_lsb);
        ps1_mc_respond(0x47);       //receiveOrNextCmd(&_);
    } else {
        ps1_mc_respond(0x4E);       //receiveOrNextCmd(&_);
    }
}

static void __time_critical_func(mc_mmce_ping)(void) {
    uint8_t _;
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x27);   receiveOrNextCmd(&_);
    ps1_mc_respond(0xFF);   receiveOrNextCmd(&_);
}

static void __time_critical_func(mc_mmce_set_game_id)(void) {
    uint8_t length = 0;
    uint8_t game_id[256] = {0};
    uint8_t prev = 0;
    uint8_t _;
    memset(received_game_id, 0, sizeof(received_game_id));
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);       // Reserved
    ps1_mc_respond(0x00);   receiveOrNextCmd(&length);  // length
    prev = length;

    for (uint8_t i = 0; i < length; i++) {
        ps1_mc_respond(prev);   receiveOrNextCmd(&game_id[i]);
        prev = game_id[i];
    }
    game_db_extract_title_id(game_id, received_game_id, length, sizeof(received_game_id));
    if (!game_db_sanity_check_title_id(received_game_id))
        memset(received_game_id, 0, sizeof(received_game_id));
    mc_pro_command = MCP_GAME_ID;
}

static void __time_critical_func(mc_mmce_prev_channel)(void) {
    uint8_t _;
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x20);   receiveOrNextCmd(&_);
    ps1_mc_respond(0xFF);   receiveOrNextCmd(&_);
    mc_pro_command = MCP_PRV_CH;
}

static void __time_critical_func(mc_mmce_next_channel)(void) {
    uint8_t _;
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x20);   receiveOrNextCmd(&_);
    ps1_mc_respond(0xFF);   receiveOrNextCmd(&_);
    mc_pro_command = MCP_NXT_CH;
}

static void __time_critical_func(mc_mmce_prev_index)(void) {
    uint8_t _;
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x20);   receiveOrNextCmd(&_);
    ps1_mc_respond(0xFF);   receiveOrNextCmd(&_);
    mc_pro_command = MCP_PRV_CARD;
}

static void __time_critical_func(mc_mmce_next_index)(void) {
    uint8_t _;
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x00);   receiveOrNextCmd(&_);
    ps1_mc_respond(0x20);   receiveOrNextCmd(&_);
    ps1_mc_respond(0xFF);   receiveOrNextCmd(&_);
    mc_pro_command = MCP_NXT_CARD;
}

/**
  01h  Hi-Z  Controller address
  42h  idlo  Receive ID bit0..7 (variable) and Send Read Command (ASCII "B")
  TAP  idhi  Receive ID bit8..15 (usually/always 5Ah)
  MOT  swlo  Receive Digital Switches bit0..7
  MOT  swhi  Receive Digital Switches bit8..15
  --------

  Switch Bits:
  0   Select Button    (0=Pressed, 1=Released)
  1   L3/Joy-button    (0=Pressed, 1=Released/None/Disabled) ;analog mode only
  2   R3/Joy-button    (0=Pressed, 1=Released/None/Disabled) ;analog mode only
  3   Start Button     (0=Pressed, 1=Released)
  4   Joypad Up        (0=Pressed, 1=Released)
  5   Joypad Right     (0=Pressed, 1=Released)
  6   Joypad Down      (0=Pressed, 1=Released)
  7   Joypad Left      (0=Pressed, 1=Released)


  8   L2 Button        (0=Pressed, 1=Released) (Lower-left shoulder)
  9   R2 Button        (0=Pressed, 1=Released) (Lower-right shoulder)
  10  L1 Button        (0=Pressed, 1=Released) (Upper-left shoulder)
  11  R1 Button        (0=Pressed, 1=Released) (Upper-right shoulder)
  12  /\ Button        (0=Pressed, 1=Released) (Triangle, upper button)
  13  () Button        (0=Pressed, 1=Released) (Circle, right button)
  14  >< Button        (0=Pressed, 1=Released) (Cross, lower button)
  15  [] Button        (0=Pressed, 1=Released) (Square, left button)
 */
static void mc_read_controller(void) {
    static uint8_t prevCommand = 0;
    uint8_t controller_in[5];
    uint8_t _;
    for (uint8_t i = 0; i < 5; i++) {
        recv_cntrl(&controller_in[i]);
    }
    #define HOTKEYS 0b00001111
    #define BTN_UP  0b00010000
    #define BTN_DWN 0b01000000
    #define BTN_LFT 0b10000000
    #define BTN_RGT 0b00100000
    #define IS_PRESSED(x,y) ((x&y) == 0)

    if (IS_PRESSED(controller_in[4], HOTKEYS)) {
        if (prevCommand == 0) {
            if (IS_PRESSED(controller_in[3], BTN_UP)) {
                log(LOG_TRACE, "PS1:    Up pressed\n");
                prevCommand = MCP_NXT_CARD;
            } else if (IS_PRESSED(controller_in[3], BTN_DWN)) {
                log(LOG_TRACE, "PS1:    Down pressed\n");
                prevCommand = MCP_PRV_CARD;
            } else if (IS_PRESSED(controller_in[3], BTN_LFT)) {
                log(LOG_TRACE, "PS1:    Left pressed\n");
                prevCommand = MCP_PRV_CH;
            } else if (IS_PRESSED(controller_in[3], BTN_RGT)) {
                log(LOG_TRACE, "PS1:    Right pressed\n");
                prevCommand = MCP_NXT_CH;
            }
        }
    } else if (prevCommand != 0){
        log(LOG_TRACE, "PS1:    Previous command: %u\n", prevCommand);
        mc_pro_command = prevCommand;
        prevCommand = 0;
    }

    while (true) {
        receiveOrNextCmd(&_);
        recv_cntrl(&_);
    }
}

static void __time_critical_func(mc_main_loop)(void) {
    flag = 8;

    while (1) {
        uint8_t ch = 0x00;

        //while (!reset && !reset && !reset && !reset && !reset) {
        //    if (mc_exit_request) {
        //        mc_exit_response = 1;
        //        return;
        //    }
        //}

        uint8_t received = recv_cmd(&ch);

        if (received == RECEIVE_EXIT) {
            mc_exit_response = 1;
            break;
        }
        /* If this ch belongs to the next command sequence */
        if (received == RECEIVE_RESET) {
            reset = 0;
            continue;
        }


        if (0x81 == ch) { /* Command is for MC - process! */
            ps1_mc_respond(flag);
            int ret = recv_cmd(&ch);
            if (ret == RECEIVE_RESET)
                continue;

            switch (ch) {
                case 'S': mc_cmd_get_card_id(); break;
                case 'R': mc_cmd_read(false); break;
                case 'B': mc_cmd_read(true); break;
                case 'W': mc_cmd_write(); break;
                case 0x20: mc_mmce_ping(); break;
                case 0x21: mc_mmce_set_game_id(); break;
                case 0x22: mc_mmce_prev_channel(); break;
                case 0x23: mc_mmce_next_channel(); break;
                case 0x24: mc_mmce_prev_index(); break;
                case 0x25: mc_mmce_next_index(); break;
                default: log(LOG_WARN, "Unknown command: 0x%.02x\n", ch); break;
            }
        } else if (0x01 == ch) {
            mc_read_controller();
        } else {
            /* If the command sequence is not to be processed (e.g. unknown) or the are leftover bytes */
            while (recv_cmd(&ch) == RECEIVE_OK){
                tight_loop_contents();
            }
        }

    }

}

static void __no_inline_not_in_flash_func(mc_main)(void) {
    while (1) {
        while (!mc_enter_request)
        {}
        mc_enter_response = 1;

        mc_main_loop();
    }
}

static gpio_irq_callback_t callbacks[NUM_CORES];

static void __time_critical_func(RAM_gpio_acknowledge_irq)(uint gpio, uint32_t events) {
    check_gpio_param(gpio);

    iobank0_hw->intr[gpio / 8] = events << (4 * (gpio % 8));
}

static void __time_critical_func(RAM_gpio_default_irq_handler)(void) {
    uint core = get_core_num();
    gpio_irq_callback_t callback = callbacks[core];
    io_bank0_irq_ctrl_hw_t *irq_ctrl_base = core ? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;
    for (uint gpio = 0; gpio < NUM_BANK0_GPIOS; gpio+=8) {
        uint32_t events8 = irq_ctrl_base->ints[gpio >> 3u];
        // note we assume events8 is 0 for non-existent GPIO
        for(uint i=gpio;events8 && i<gpio+8;i++) {
            uint32_t events = events8 & 0xfu;
            if (events) {
                RAM_gpio_acknowledge_irq(i, events);
                if (callback) {
                    callback(i, events);
                }
            }
            events8 >>= 4;
        }
    }
}

static void my_gpio_set_irq_callback(gpio_irq_callback_t callback) {
    uint core = get_core_num();
    if (callbacks[core]) {
        if (!callback) {
            irq_remove_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler);
        }
        callbacks[core] = callback;
    } else if (callback) {
        callbacks[core] = callback;
        irq_add_shared_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler, GPIO_IRQ_CALLBACK_ORDER_PRIORITY);
    }
}

static void my_gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool enabled, gpio_irq_callback_t callback) {
    gpio_set_irq_enabled(gpio, events, enabled);
    my_gpio_set_irq_callback(callback);
    if (enabled) irq_set_enabled(IO_IRQ_BANK0, true);
}

void ps1_memory_card_main(void) {
    multicore_lockout_victim_init();

    init_pio();

    us_startup = time_us_64();
    DPRINTF("Secondary core!\n");

    my_gpio_set_irq_enabled_with_callback(PIN_PSX_SEL, GPIO_IRQ_EDGE_RISE, 1, card_deselected);

    gpio_set_slew_rate(PIN_PSX_DAT, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(PIN_PSX_DAT, GPIO_DRIVE_STRENGTH_4MA);

    mc_main();
}

static int memcard_running;

void ps1_memory_card_exit(void) {
    if (!memcard_running)
        return;

    mc_exit_request = 1;
    while (!mc_exit_response)
    {}
    mc_exit_request = mc_exit_response = 0;
    memcard_running = 0;
}

void ps1_memory_card_enter(void) {
    if (memcard_running)
        return;

    mc_enter_request = 1;
    while (!mc_enter_response)
    {}
    mc_enter_request = mc_enter_response = 0;
    memcard_running = 1;
    mc_pro_command = 0;
}

void ps1_memory_card_reset_ode_command(void) {
    mc_pro_command = 0;
}

uint8_t ps1_memory_card_get_ode_command(void) {
    return mc_pro_command;
}

const char* ps1_memory_card_get_game_id(void) {
    return received_game_id;
}

void ps1_memory_card_unload(void) {
    pio_remove_program(pio0, &cmd_reader_program, cmd_reader.offset);
    pio_sm_unclaim(pio0, cmd_reader.sm);
    pio_remove_program(pio0, &dat_writer_program, dat_writer.offset);
    pio_sm_unclaim(pio0, dat_writer.sm);
}
