#include "picostation.h"

#include <stdio.h>
#include <time.h>

#include "cmd.h"
#include "disc_image.h"
#include "directory_listing.h"
#include "drive_mechanics.h"
#include "hardware/pwm.h"
#include <hardware/i2c.h>
#include "i2s.h"
#include "logging.h"
#include "main.pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pseudo_atomics.h"
#include "subq.h"
#include "values.h"
#include "si5351.h"

#if CONTROLLER_SNIFF
    #include "controller_sniff.pio.h"
#endif

#if DEBUG_MAIN
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) while (0)
#endif

// To-do: Implement lid switch behavior
// To-do: Implement a console side menu to select the cue file
// To-do: Implement level meter mode to command $AX - AudioCTRL
// To-do: Fix seeks that go into the lead-in + track 1 pregap areas, possibly sending bad data over I2S

// To-do: Make an ODE class and move these to members
picostation::I2S m_i2s;
picostation::MechCommand m_mechCommand;

bool picostation::g_subqDelay = false;  // core0: r/w

static int s_currentPlaybackSpeed = 1;
int picostation::g_targetPlaybackSpeed = 1;  // core0: r/w

mutex_t picostation::g_mechaconMutex;
pseudoatomic<bool> picostation::g_coreReady[2];

unsigned int picostation::g_audioCtrlMode = audioControlModes::NORMAL;

pseudoatomic<picostation::FileListingStates> picostation::g_fileListingState;
pseudoatomic<uint32_t> picostation::g_fileArg;

static unsigned int s_mechachonOffset;
unsigned int picostation::g_soctOffset;
unsigned int picostation::g_subqOffset;

static uint8_t s_resetPending = 0;

static picostation::PWMSettings pwmDataClock = 
{
	.gpio = Pin::DA15,
	.wrap = (1 * 32) - 1, 
	.clkdiv = 4, 
	.invert = true, 
	.level = (32 / 2)
};

static picostation::PWMSettings pwmLRClock = 
{
	.gpio = Pin::LRCK,
	.wrap = (48 * 32) - 1,
	.clkdiv = 4,
	.invert = false,
	.level = (48 * (32 / 2))
};

static picostation::PWMSettings pwmMainClock =
{
	.gpio = Pin::CLK,
	.wrap = 1,
	.clkdiv = 2,
	.invert = false,
	.level = 1
};

static void initPWM(picostation::PWMSettings *settings);

static void __time_critical_func(interruptHandler)(unsigned int gpio, uint32_t events)
{
    static uint64_t lastLowEvent = 0;
	static uint64_t lastLowEventDoor = 0;

    switch (gpio)
    {
        case Pin::RESET:
        {
            if (events & GPIO_IRQ_LEVEL_LOW)
            {
                lastLowEvent = time_us_64();
                // Disable low signal edge detection
                gpio_set_irq_enabled(Pin::RESET, GPIO_IRQ_LEVEL_LOW, false);
                // Enable high signal edge detection
                gpio_set_irq_enabled(Pin::RESET, GPIO_IRQ_LEVEL_HIGH, true);
            } 
            else if (events & GPIO_IRQ_LEVEL_HIGH)
            {
                // Disable the rising edge detection
                gpio_set_irq_enabled(Pin::RESET, GPIO_IRQ_LEVEL_HIGH, false);

                const uint64_t c_now = time_us_64();
                const uint64_t c_timeElapsed = c_now - lastLowEvent;
                
                if (c_timeElapsed >= 50000U)  // Debounce, only reset if the pin was low for more than 50000us(50 ms)
                {
                    if (c_timeElapsed >= 1000000U) // pressed more one second
					{
						s_resetPending = 2;
					}
					else
					{
						s_resetPending = 1;
					}
                }
                
				// Enable the low signal edge detection again
				gpio_set_irq_enabled(Pin::RESET, GPIO_IRQ_LEVEL_LOW, true);
            }
        } break;
        
        case Pin::DOOR:
        {
            if (events & GPIO_IRQ_LEVEL_HIGH)
            {
                lastLowEventDoor = time_us_64();
                // Disable low signal edge detection
                gpio_set_irq_enabled(Pin::DOOR, GPIO_IRQ_LEVEL_HIGH, false);
                // Enable high signal edge detection
                gpio_set_irq_enabled(Pin::DOOR, GPIO_IRQ_LEVEL_LOW, true);
            }
            else if (events & GPIO_IRQ_LEVEL_LOW)
            {
                // Disable the rising edge detection
                gpio_set_irq_enabled(Pin::DOOR, GPIO_IRQ_LEVEL_LOW, false);

                const uint64_t c_now = time_us_64();
                const uint64_t c_timeElapsed = c_now - lastLowEventDoor;
                if (c_timeElapsed >= 50000U)  // Debounce, only reset if the pin was low for more than 50000us(50 ms)
                {
                    m_i2s.s_doorPending = true;
                }
                
                // Enable the low signal edge detection again
                gpio_set_irq_enabled(Pin::DOOR, GPIO_IRQ_LEVEL_HIGH, true);
            }
        } break;

        case Pin::XLAT:
        {
            m_mechCommand.processLatchedCommand();
        } break;
    }
}

static void __time_critical_func(mech_irq_hnd)()
{
	// Update latching
	m_mechCommand.updateMech();
	pio_interrupt_clear(PIOInstance::MECHACON, 0);
}

static void __time_critical_func(send_subq)(const int Sector)
{
	picostation::SubQ subq(&picostation::g_discImage);
	
	subq.start_subq(Sector);
	picostation::g_subqDelay = false;
}

//=====================================================
// CONTROLLER_SNIFF
// Inspired from the functional original (ManiacVera)
// https://github.com/ManiacVera/PicoIGR
//=====================================================
#if CONTROLLER_SNIFF

#define CONT_CLKDIV 50
#define CONT_PIO    pio1
#define CONT_SM_CMD 0
#define CONT_SM_DAT 1
#define PAD_READ 0x42

typedef enum {
    WAIT_CMD,
    WAIT_TAP,
    WAIT_DAT0,
    WAIT_DAT1
} ContState;

enum Action {
	ACTION_NONE,
	ACTION_SHORT_RESET,
	ACTION_LONG_RESET
};

ContState contState = WAIT_CMD;
static uint8_t dat0 = 0;
volatile Action detected_action = ACTION_NONE;
volatile bool ready_to_process = false;
static bool cont_frame_consumed = false;
static bool combo_lock = false;

static absolute_time_t frame_start_time;
static bool valid_pad_frame = false;

// Contagem de hold
static uint32_t hold_frames = 0;
#define HOLD_FRAMES_500MS 30   // ~500 ms a 60Hz

// Bitfield PSX – active-high (apos ^ 0xFFFF)
// bit = 1 -> botão pressionado
// bit = 0 -> botão solto
#define BTN_MASK_SELECT    (1u << 0)
#define BTN_MASK_L3        (1u << 1)
#define BTN_MASK_R3        (1u << 2)
#define BTN_MASK_START     (1u << 3)
#define BTN_MASK_UP        (1u << 4)
#define BTN_MASK_RIGHT     (1u << 5)
#define BTN_MASK_DOWN      (1u << 6)
#define BTN_MASK_LEFT      (1u << 7)
#define BTN_MASK_L2        (1u << 8)
#define BTN_MASK_R2        (1u << 9)
#define BTN_MASK_L1        (1u << 10)
#define BTN_MASK_R1        (1u << 11)
#define BTN_MASK_TRIANGLE  (1u << 12)
#define BTN_MASK_CIRCLE    (1u << 13)
#define BTN_MASK_X         (1u << 14)
#define BTN_MASK_SQUARE    (1u << 15)
//-------------------------
#define MASK_SHORT_RESET  (BTN_MASK_START | BTN_MASK_SELECT | BTN_MASK_CIRCLE)
#define MASK_LONG_RESET   (BTN_MASK_START | BTN_MASK_SELECT | BTN_MASK_X)
#define MASK_RETURN_MENU  (BTN_MASK_START | BTN_MASK_SELECT | BTN_MASK_TRIANGLE)
//-------------------------

void __time_critical_func(picostation::do_reset)(uint32_t sleepMS) {
	gpio_set_dir(Pin::RESET_OUT, GPIO_OUT);
	gpio_put(Pin::RESET_OUT, 0);
    absolute_time_t start = get_absolute_time();
    while (absolute_time_diff_us(start, get_absolute_time()) < (int64_t)sleepMS * 1000){
        tight_loop_contents();
    }
	gpio_put(Pin::RESET_OUT, 1);
	gpio_set_dir(Pin::RESET_OUT, GPIO_IN);
    gpio_disable_pulls(Pin::RESET_OUT);
}

static inline void controller_cmd_init(PIO pio, uint sm, uint offset)
{
    pio_sm_config c = controller_cmd_program_get_default_config(offset);

    sm_config_set_in_pins(&c, Pin::CONT_CMD);

    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_CMD, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_ATT, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_CLK, 1, false);

    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, CONT_CLKDIV);

    pio_sm_init(pio, sm, offset, &c);
}

static inline void controller_dat_init(PIO pio, uint sm, uint offset)
{
    pio_sm_config c = controller_dat_program_get_default_config(offset);

    sm_config_set_in_pins(&c, Pin::CONT_DAT);

    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_DAT, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_ATT, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, Pin::CONT_CLK, 1, false);

    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, CONT_CLKDIV);

    pio_sm_init(pio, sm, offset, &c);
}

static inline uint8_t cont_read(PIO pio, uint sm){
    return (uint8_t)(pio_sm_get(pio, sm) >> 24);
}

void controller_init(void){
    uint off_cmd = pio_add_program(CONT_PIO, &controller_cmd_program);
    uint off_dat = pio_add_program(CONT_PIO, &controller_dat_program);
    controller_cmd_init(CONT_PIO, CONT_SM_CMD, off_cmd);
    controller_dat_init(CONT_PIO, CONT_SM_DAT, off_dat);
    pio_sm_set_enabled(CONT_PIO, CONT_SM_CMD, true);
    pio_sm_set_enabled(CONT_PIO, CONT_SM_DAT, true);
}

void __time_critical_func(controller_poll)(){
    switch (contState)
    {
        case WAIT_CMD:
        {
            if (gpio_get(Pin::CONT_ATT) == 0) {
                cont_frame_consumed = false;
                frame_start_time = get_absolute_time();
            }

            if (cont_frame_consumed)
                return;

            if (pio_sm_is_rx_fifo_empty(CONT_PIO, CONT_SM_CMD))
                return;

            uint8_t cmd = cont_read(CONT_PIO, CONT_SM_CMD);

            if (cmd == PAD_READ) {
                valid_pad_frame = true;
                contState = WAIT_TAP;
            } else {
                // não é leitura de controle
                valid_pad_frame = false;
                cont_frame_consumed = true;
                contState = WAIT_CMD;
            }
            break;
        }

        case WAIT_TAP:
        {
            if (pio_sm_is_rx_fifo_empty(CONT_PIO, CONT_SM_CMD))
                return;

            // Ignora TAP
            cont_read(CONT_PIO, CONT_SM_CMD);
            contState = WAIT_DAT0;
            break;
        }

        case WAIT_DAT0:
        {
            if (pio_sm_is_rx_fifo_empty(CONT_PIO, CONT_SM_DAT))
                return;

            dat0 = cont_read(CONT_PIO, CONT_SM_DAT);
            contState = WAIT_DAT1;
            break;
        }

        case WAIT_DAT1:
        {
            if (pio_sm_is_rx_fifo_empty(CONT_PIO, CONT_SM_DAT))
                return;

            uint8_t dat1 = cont_read(CONT_PIO, CONT_SM_DAT);

            // Ignora se não for frame válido de controle
            if (!valid_pad_frame) {
                cont_frame_consumed = true;
                contState = WAIT_CMD;
                return;
            }

            // Ignora frame longo (memory card)
            int64_t frame_us =
                absolute_time_diff_us(frame_start_time, get_absolute_time());

            if (frame_us > 1000) { // > 1 ms = suspeito
                cont_frame_consumed = true;
                contState = WAIT_CMD;
                return;
            }

            uint16_t buttons = (dat1 | (dat0 << 8)) ^ 0xFFFF;

            bool short_reset_now =
                ((buttons & MASK_SHORT_RESET) == MASK_SHORT_RESET);

            bool long_reset_now =
                ((buttons & MASK_LONG_RESET) == MASK_LONG_RESET);

            // ================= HOLD 500ms =================
            if (short_reset_now || long_reset_now) {
                hold_frames++;
            } else {
                hold_frames = 0;
            }

            if (hold_frames >= HOLD_FRAMES_500MS && !combo_lock) {
                if (short_reset_now) {
                    detected_action = ACTION_SHORT_RESET;
                } else if (long_reset_now) {
                    detected_action = ACTION_LONG_RESET;
                }
                ready_to_process = true;
                combo_lock = true;
            }

            // Libera ao soltar
            if (!short_reset_now && !long_reset_now) {
                combo_lock = false;
                hold_frames = 0;
            }

            cont_frame_consumed = true;
            contState = WAIT_CMD;
            break;
        }
    }
}
#endif
//=====================================================




[[noreturn]] void __time_critical_func(picostation::core0Entry)()
{
    g_coreReady[0] = true;
    while (!g_coreReady[1].Load())
    {
        tight_loop_contents();
    }

    while (true)
    {
        if (s_resetPending)
        {
            while (gpio_get(Pin::RESET) == 0)
            {
                tight_loop_contents();
            }
			reset();
		}

        const int currentSector = g_driveMechanics.getSector();

        // Limit Switch
        gpio_put(Pin::LMTSW, currentSector > 3000);

        updatePlaybackSpeed();

        // Soct/Sled/seek
        if (m_mechCommand.getSoct())
        {
            if (pio_sm_get_rx_fifo_level(PIOInstance::SOCT, SM::SOCT))
            {
                pio_sm_drain_tx_fifo(PIOInstance::SOCT, SM::SOCT);
                m_mechCommand.setSoct(false);
                pio_sm_set_enabled(PIOInstance::SOCT, SM::SOCT, false);
            }
        }
        else if (!g_driveMechanics.isSledStopped())
        {
            g_driveMechanics.moveSled(m_mechCommand);
        }
        else if (m_mechCommand.getSens(SENS::GFS))
        {
            if (m_i2s.getSectorSending() == currentSector)
            {
                g_driveMechanics.moveToNextSector();
                g_subqDelay = true;

                add_alarm_in_us(time_us_64() - m_i2s.getLastSectorTime() + c_MaxSubqDelayTime,
					[](alarm_id_t id, void *user_data) -> int64_t {
						send_subq((const int) user_data);
						return 0;
					}, (void *) currentSector, true);
            }
        }

        #if CONTROLLER_SNIFF
            controller_poll();
            if (ready_to_process) {
    			ready_to_process = false;
    			switch (detected_action) {
        			case ACTION_SHORT_RESET: 
                        detected_action = ACTION_NONE;
                        do_reset(100);
                        ready_to_process = false;
                        s_resetPending = 1;
                        reset();
                    break;
        			case ACTION_LONG_RESET:  
                        detected_action = ACTION_NONE;
                    	do_reset(100);
                    	ready_to_process = false;
                        s_resetPending = 2;
                        reset();
                    break;                
        			default: 
                        detected_action = ACTION_NONE;
                    break;
    			}
    		}    
        #endif

    }
}

[[noreturn]] void picostation::core1Entry()
{
    m_i2s.start(m_mechCommand);
    while (1) asm("");
    __builtin_unreachable();
}

void __time_critical_func(picostation::initHW)()
{
#if DEBUG_LOGGING_ENABLED
	stdio_init_all();
    stdio_set_chars_available_callback(NULL, NULL);
    sleep_ms(1250);
#endif

    DEBUG_PRINT("Initializing...\n");

    mutex_init(&g_mechaconMutex);

    for (const unsigned int pin : Pin::allPins)
    {
        gpio_init(pin);
    }

    #if CONTROLLER_SNIFF
        gpio_init(Pin::RESET_OUT);

        // SEM PULLS
        gpio_disable_pulls(Pin::CONT_DAT);
        gpio_disable_pulls(Pin::CONT_CMD);
        gpio_disable_pulls(Pin::CONT_CLK);
        gpio_disable_pulls(Pin::CONT_ATT);
        gpio_disable_pulls(Pin::RESET_OUT);

        // Garante entrada real
        gpio_set_dir(Pin::CONT_DAT, GPIO_IN);
        gpio_set_dir(Pin::CONT_CMD, GPIO_IN);
        gpio_set_dir(Pin::CONT_CLK, GPIO_IN);
        gpio_set_dir(Pin::CONT_ATT, GPIO_IN);
        gpio_set_dir(Pin::RESET_OUT, GPIO_IN);
    #endif
    
    gpio_put(Pin::SD_CS, 1);
    gpio_set_dir(Pin::SD_CS, GPIO_OUT);
    
    gpio_set_dir(Pin::XLAT, GPIO_IN);
    gpio_set_dir(Pin::SQCK, GPIO_IN);
    gpio_set_dir(Pin::LMTSW, GPIO_OUT);
    gpio_set_dir(Pin::DOOR, GPIO_IN);
    gpio_set_dir(Pin::RESET, GPIO_IN);
    gpio_set_dir(Pin::SENS, GPIO_OUT);
    gpio_set_dir(Pin::DA15, GPIO_OUT);
    gpio_set_dir(Pin::DA16, GPIO_OUT);
    gpio_set_dir(Pin::LRCK, GPIO_OUT);
    gpio_set_dir(Pin::SCOR, GPIO_OUT);
    gpio_put(Pin::SCOR, 0);
    gpio_set_dir(Pin::SQSO, GPIO_OUT);
    gpio_put(Pin::SQSO, 0);
    gpio_set_dir(Pin::CLK, GPIO_OUT);
    gpio_set_dir(Pin::CMD_CK, GPIO_IN);
    gpio_set_dir(Pin::CMD_DATA, GPIO_IN);

    gpio_set_input_hysteresis_enabled(Pin::XLAT, true);
    gpio_set_input_hysteresis_enabled(Pin::SQCK, true);
    gpio_set_input_hysteresis_enabled(Pin::RESET, true);
    gpio_set_input_hysteresis_enabled(Pin::CMD_CK, true);
    
    i2c_init(i2c0, 400*1000);
	gpio_set_function(Pin::EXP_I2C0_SDA, GPIO_FUNC_I2C);
	gpio_set_function(Pin::EXP_I2C0_SCL, GPIO_FUNC_I2C);
	gpio_pull_up(Pin::EXP_I2C0_SDA);
	gpio_pull_up(Pin::EXP_I2C0_SCL);
	
	// Initialize the Si5351
	if (si5351_Init(0))
	{
		si5351_SetupCLK1(53203425, SI5351_DRIVE_STRENGTH_8MA);
		si5351_SetupCLK2(53693175, SI5351_DRIVE_STRENGTH_8MA);
		si5351_EnableOutputs((1<<1) | (1<<2));
	}

    initPWM(&pwmMainClock);
    initPWM(&pwmDataClock);
    initPWM(&pwmLRClock);

    uint32_t i2s_pio_offset = pio_add_program(PIOInstance::I2S_DATA, &i2s_data_program);
    i2s_data_program_init(PIOInstance::I2S_DATA, SM::I2S_DATA, i2s_pio_offset, Pin::DA15, Pin::DA16);

    s_mechachonOffset = pio_add_program(PIOInstance::MECHACON, &mechacon_program);
    mechacon_program_init(PIOInstance::MECHACON, SM::MECHACON, s_mechachonOffset, Pin::CMD_DATA);

    g_soctOffset = pio_add_program(PIOInstance::SOCT, &soct_program);
    g_subqOffset = pio_add_program(PIOInstance::SUBQ, &subq_program);

    pio_sm_set_enabled(PIOInstance::I2S_DATA, SM::I2S_DATA, true);
    pwm_set_mask_enabled((1 << pwmLRClock.sliceNum) | (1 << pwmDataClock.sliceNum) | (1 << pwmMainClock.sliceNum));

    uint64_t startTime = time_us_64();
    gpio_set_dir(Pin::RESET, GPIO_OUT);
    gpio_put(Pin::RESET, 0);
    sleep_ms(300);
    gpio_set_dir(Pin::RESET, GPIO_IN);

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(Pin::RESET) == 0)
        {
            startTime = time_us_64();
        }
    }

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(Pin::CMD_CK) == 0)
        {
            startTime = time_us_64();
        }
    }

    gpio_set_irq_enabled_with_callback(Pin::RESET, GPIO_IRQ_LEVEL_LOW, true, &interruptHandler);
    gpio_set_irq_enabled_with_callback(Pin::DOOR, GPIO_IRQ_LEVEL_HIGH, true, &interruptHandler);
    gpio_set_irq_enabled_with_callback(Pin::XLAT, GPIO_IRQ_EDGE_FALL, true, &interruptHandler);

    pio_sm_set_enabled(PIOInstance::MECHACON, SM::MECHACON, true);
    
    pio_set_irq0_source_enabled(PIOInstance::MECHACON, (enum pio_interrupt_source)pis_interrupt0, true);
    
    
    pio_interrupt_clear(PIOInstance::MECHACON, 0);
    irq_set_exclusive_handler(PIO0_IRQ_0, mech_irq_hnd);
    irq_set_enabled(PIO0_IRQ_0, true);

    g_coreReady[0] = false;
    g_coreReady[1] = false;

    #if CONTROLLER_SNIFF
        controller_init();
    #endif

    DEBUG_PRINT("ON!\n");
}

static void __time_critical_func(initPWM)(picostation::PWMSettings *settings)
{
    gpio_set_function(settings->gpio, GPIO_FUNC_PWM);
    settings->sliceNum = pwm_gpio_to_slice_num(settings->gpio);
    settings->config = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&settings->config, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&settings->config, settings->wrap);
    pwm_config_set_clkdiv(&settings->config, settings->clkdiv);
    pwm_config_set_output_polarity(&settings->config, settings->invert, settings->invert);
    pwm_init(settings->sliceNum, &settings->config, false);
    pwm_set_both_levels(settings->sliceNum, settings->level, settings->level);
}

void __time_critical_func(picostation::updatePlaybackSpeed)()
{
    static constexpr unsigned int c_clockDivNormal = 4;
    static constexpr unsigned int c_clockDivDouble = 2;

    if (s_currentPlaybackSpeed != g_targetPlaybackSpeed)
    {
        s_currentPlaybackSpeed = g_targetPlaybackSpeed;
        const unsigned int clock_div = (s_currentPlaybackSpeed == 1) ? c_clockDivNormal : c_clockDivDouble;
        pwm_set_mask_enabled(0);
        pwm_config_set_clkdiv_int(&pwmDataClock.config, clock_div);
        pwm_config_set_clkdiv_int(&pwmLRClock.config, clock_div);
        pwm_hw->slice[pwmDataClock.sliceNum].div = pwmDataClock.config.div;
        pwm_hw->slice[pwmLRClock.sliceNum].div = pwmLRClock.config.div;
        pwm_set_mask_enabled((1 << pwmLRClock.sliceNum) | (1 << pwmDataClock.sliceNum) | (1 << pwmMainClock.sliceNum));
        DEBUG_PRINT("x%i\n", s_currentPlaybackSpeed);
    }
}

void __time_critical_func(picostation::reset)()
{
    DEBUG_PRINT("RESET!\n");
    m_i2s.i2s_set_state(0);
    pio_sm_set_enabled(PIOInstance::SUBQ, SM::SUBQ, false);
    pio_sm_set_enabled(PIOInstance::SOCT, SM::SOCT, false);
    pio_sm_restart(PIOInstance::MECHACON, SM::MECHACON);

    pio_sm_clear_fifos(PIOInstance::MECHACON, SM::MECHACON);
    pio_sm_clear_fifos(PIOInstance::SOCT, SM::SOCT);
    pio_sm_clear_fifos(PIOInstance::SUBQ, SM::SUBQ);

    g_targetPlaybackSpeed = 1;
    updatePlaybackSpeed();
    
    if (s_resetPending == 2)
    {
        if (s_dataLocation != picostation::DiscImage::DataLocation::RAM)
		{
			g_discImage.unload();
			g_discImage.makeDummyCue();
			m_i2s.menu_active = true;
			g_discImage.set_skip_bootsector(false);
			g_discImage.set_skip_edc(false);
            m_mechCommand.resetBootSectorPattern();
		}
		picostation::DirectoryListing::gotoRoot();
		s_dataLocation = picostation::DiscImage::DataLocation::RAM;
    }

    mechacon_program_init(PIOInstance::MECHACON, SM::MECHACON, s_mechachonOffset, Pin::CMD_DATA);
    g_subqDelay = false;
    m_mechCommand.setSoct(false);

    gpio_put(Pin::SCOR, 0);
    gpio_put(Pin::SQSO, 0);
	g_driveMechanics.resetDrive();
	m_i2s.reinitI2S();
    g_discImage.set_skip_bootsector(false);
    m_mechCommand.resetBootSectorPattern();
	
	uint64_t startTime = time_us_64();
	
	while ((time_us_64() - startTime) < 30000)
	{
		if (gpio_get(Pin::RESET) == 0)
		{
			startTime = time_us_64();
		}
	}
	
	while ((time_us_64() - startTime) < 30000)
	{
		if (gpio_get(Pin::CMD_CK) == 0)
		{
			startTime = time_us_64();
		}
	}
	
    pio_sm_set_enabled(PIOInstance::MECHACON, SM::MECHACON, true);
    
	s_resetPending = 0;
	gpio_set_irq_enabled(Pin::RESET, GPIO_IRQ_LEVEL_LOW, true);
}

