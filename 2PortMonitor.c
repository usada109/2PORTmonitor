#include <stdio.h>
#include "pico/stdlib.h"
#include "pico.h"
#include "hardware/adc.h"
#include "hardware/resets.h"
#include "hardware/pll.h"
#include "hardware/structs/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/timer.h"
#include "class/cdc/cdc_device.h"
#include "tusb_option.h"
#include "generated/ws2812.pio.h"

#define UART_ID1 uart0
#define BAUD_RATE1 57600
#define UART_TX_PIN1 16
#define UART_RX_PIN1 17
#define LED_PIN 25

void measure_freqs(void);
bool __no_inline_not_in_flash_func(get_bootsel_button)();
void pll_358(PLL pll);
void put_rgb(uint8_t red, uint8_t green, uint8_t blue);

//////////////////////////////////
int main() {
    stdio_init_all();   //To use USB

    // 制御端子
    gpio_init(2);
    gpio_set_dir(2,GPIO_OUT);
    gpio_init(3);
    gpio_set_dir(3,GPIO_OUT);

    // シリアル通信
    gpio_set_function(UART_TX_PIN1, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN1, GPIO_FUNC_UART);
    gpio_pull_up(UART_RX_PIN1);

    /// 動作周波数を変更
    ///pll_358(pll_sys);
    ///clock_set_reported_hz(clk_sys ,229090909);
    ///clock_set_reported_hz(clk_peri,229090909);

    // WS2812初期化
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, 16, 800000, true);
    put_rgb(64,64,64);   // White

    {   /// 初期情報を提供 ///
        int cnt = 2;
        do{
            printf("\nBuild %s %s\n",__DATE__,__TIME__);
            measure_freqs();
            sleep_ms(750);
        }while(--cnt);

        printf("\nUSB⇔シリアル変換\n");

        // Set up our UART with the required speed.
        int res = uart_init(UART_ID1, BAUD_RATE1);
        printf("\nボーレート = %d\n",res);
        printf("pin(21) TX\n");
        printf("pin(22) RX\n");
        printf("pin(3) GND\n");
        printf("pin(4) RTS\n");
        printf("pin(5) DTR\n");
    }



    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);


    //uint32_t bit_rate;
    //uint8_t  stop_bits; ///< 0: 1 stop bit - 1: 1.5 stop bits - 2: 2 stop bits
    //uint8_t  parity;    ///< 0: None - 1: Odd - 2: Even - 3: Mark - 4: Space
    //uint8_t  data_bits; ///< can be 5, 6, 7, 8 or 16

    cdc_line_coding_t back ={
        57600,
        0,0,0
    };

    int led_time = 300;


    timer_hw->pause = 0;
    uint32_t btime;


    //Loop
    int ledflg = 0 ;
    int rts_dtr = 16;
    uint32_t time = time_us_32();
    while (true){
        //////////////////////////////////////////////////////////
        ///////////////////////////////////////////// シリアル⇒USB
        while( uart_is_readable(UART_ID1) ){    // 入力信号があれば
            char data = uart_getc(UART_ID1);    // シリアルから情報を取り
            printf("%c",data);                  // USB側へ出力
            gpio_put(LED_PIN,1);                // LED点灯
            led_time = 5;
        }

        {   //// PCからの制御信号 ////
            uint8_t ggg = 15 & tud_cdc_n_get_line_state(0); // Bit 0:  DTR (Data Terminal Ready), Bit 1: RTS (Request to Send)
            if(rts_dtr != ggg){
                rts_dtr = ggg;
                int nn=ggg&1,mm=(ggg>>1)&1;
                gpio_put(2,mm);     // RTS
                gpio_put(3,nn);     // DTR
                printf("\nRTS=%d DTR=%d\n",mm,nn);
            }
        }


        ///////////////////////////////////////////////////////////
        ////////////////////////////////////////////// USB⇒シリアル
        if(tud_cdc_available() > 0){
            char getDAT;
            tud_cdc_read(&getDAT, 1);           // 情報があれば
            uart_putc(UART_ID1,getDAT);         // UARTに投げる
        }else{

            /// 設定変更があれば ///
            cdc_line_coding_t param;
            tud_cdc_n_get_line_coding (0, &param);
            if(back.parity != param.parity){
                back.parity = param.parity;
                printf("\nパリティーの設定はできません\n");
            }
            if(back.stop_bits != param.stop_bits){
                back.stop_bits = param.stop_bits;
                printf("\nストップビットの設定はできません\n");
            }
            if(back.bit_rate != param.bit_rate){
                back.bit_rate = param.bit_rate;
                int res = uart_init(UART_ID1, back.bit_rate);
                printf("\n変更後ボーレート = %d\n",res);
            }
        }

        ////uint16_t result = adc_read();
        //sleep_ms(250);

        {   /////////////////////////////////////// SW TEST
            static uint16_t lvl;
            lvl += lvl;
            lvl += get_bootsel_button();
            if(lvl == 0x7FFF){
                btime = time_us_32();
            }
            else if(lvl == 0x8000){
                printf("\nリセット");
                watchdog_enable(20, 1);
                while(1);                           // リセットが掛かるまで待つ
            }
            else if(lvl == 0xFFFF){
                uint32_t now = time_us_32();
                uint32_t delta = now - btime;
                if(delta >= 1000000){   // 長押し(1秒)
                    printf("\nブートローダを起動");
                    watchdog_enable(20, 1);
                    while(1);                       // リセットが掛かるまで待つ
                }
            }
        }


        {   ///////////// 時間を管理 //////////////
            uint32_t tmp = time_us_32() - time; 
            if(tmp >= 1000){
                time += 1000;
                if(led_time > 0) led_time -= 1;
                else gpio_put(LED_PIN,0);           // LED消灯
            }
        }


    }//while loop
}

////////////////////////////////////////////////////////////
//////////////////////////////////////////// 動作周波数を変更
void pll_358(PLL pll) {
    //// 12.0/11 = 1.09090909MHz  * 840 = 458.18181818MHz
    pll->cs        = 11;        //refdiv;
    pll->fbdiv_int = 840;       //fbdiv;

    while (!(pll->cs & PLL_CS_LOCK_BITS)) tight_loop_contents();

    //// 周波数を1/4にする 229.0909090909MHz
    uint32_t pdiv    = (2 << PLL_PRIM_POSTDIV1_LSB) |
                        (2 << PLL_PRIM_POSTDIV2_LSB);
    pll->prim = pdiv;
}

///////////////////////////////////////////////////////////////////////
/////////////////////////////////////////// RAM上で動作させ、SW状態を返す
bool __no_inline_not_in_flash_func(get_bootsel_button)() {
    const uint CS_PIN_INDEX = 1;

    uint32_t flags = save_and_disable_interrupts();     // 割り込み禁止

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i);

    bool button_state = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);                          // 割込み復帰

    return button_state;
}


void measure_freqs(void) {
    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    printf("pll_sys  = %dkHz\n", f_pll_sys);
    printf("pll_usb  = %dkHz\n", f_pll_usb);
    printf("rosc     = %dkHz\n", f_rosc);
    printf("clk_sys  = %dkHz\n", f_clk_sys);
    printf("clk_peri = %dkHz\n", f_clk_peri);
    printf("clk_usb  = %dkHz\n", f_clk_usb);
    printf("clk_adc  = %dkHz\n", f_clk_adc);
    printf("clk_rtc  = %dkHz\n", f_clk_rtc);

    // Can't measure clk_ref / xosc as it is the ref
}


void put_pixel(uint32_t pixel_grb)
{
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}
void put_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    uint32_t mask = (green << 16) | (red << 8) | (blue << 0);
    put_pixel(mask);
}

//eof