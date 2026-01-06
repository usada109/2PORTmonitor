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


void measure_freqs(void);
bool __no_inline_not_in_flash_func(get_bootsel_button)();
void pll_358(PLL pll);
void put_rgb(uint8_t red, uint8_t green, uint8_t blue);
void setFiFo0(char data);
void setFiFo1(char data);
void transConsoleFiFo( void );


//////////////////////////////////
int main() {
    stdio_init_all();   //To use USB

    // 制御端子

    // シリアル通信1
    gpio_set_function(8, GPIO_FUNC_UART);
    gpio_set_function(9, GPIO_FUNC_UART);
    gpio_pull_up(9);

    // シリアル通信0
    gpio_set_function(12, GPIO_FUNC_UART);
    gpio_set_function(13, GPIO_FUNC_UART);
    gpio_pull_up(13);

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

        printf("\n２ポート  モニター\n");

        // Set up our UART with the required speed.
        int res0 = uart_init(uart0, 57600);
        int res1 = uart_init(uart1, 57600);
        printf("\nボーレート = %d\n",res0);
        printf("gpio(9) port1\n");
        printf("gpio(13) port0\n");
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

    int led1_time = 300;
    int led2_time = 300;


    timer_hw->pause = 0;
    uint32_t btime;


    //Loop
    int ledflg = 0 ;
    int rts_dtr = 16;
    uint32_t time = time_us_32();
    while (true){
        //////////////////////////////////////////////////////////
        ///////////////////////////////////////////// シリアル⇒USB  
        while( uart_is_readable(uart0) ){       // 入力信号があれば
            char data = uart_getc(uart0);       // シリアルから情報を取り
            setFiFo0(data);
            led1_time = 5;
        }
        while( uart_is_readable(uart1) ){       // 入力信号があれば
            char data = uart_getc(uart1);       // シリアルから情報を取り
            setFiFo1(data);
            led2_time = 5;
        }

        transConsoleFiFo();  // USBへ転送

        {   //// PCからの制御信号 ////
            uint8_t ggg = 15 & tud_cdc_n_get_line_state(0); // Bit 0:  DTR (Data Terminal Ready), Bit 1: RTS (Request to Send)
            if(rts_dtr != ggg){
                rts_dtr = ggg;
                int nn=ggg&1,mm=(ggg>>1)&1;
                ///gpio_put(2,mm);     // RTS
                ///gpio_put(3,nn);     // DTR
                printf("\nRTS=%d DTR=%d\n",mm,nn);
            }
        }


        ///////////////////////////////////////////////////////////
        ////////////////////////////////////////////// USB⇒シリアル
        if(tud_cdc_available() > 0){
            char getDAT;
            tud_cdc_read(&getDAT, 1);           // 情報があれば
            uart_putc(uart0,getDAT);            // UARTに投げる
            uart_putc(uart1,getDAT);            // UARTに投げる
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
                int res0 = uart_init(uart0, back.bit_rate);
                int res1 = uart_init(uart1, back.bit_rate);
                printf("\n変更後ボーレート = %d\n",res0);
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

                // LED制御
                int rr = 0;
                int bb = 0;
                if(led1_time > 0) rr = 32;
                if(led2_time > 0) bb = 32;
                put_rgb(rr,0,bb);

                if(led1_time > 0) led1_time -= 1;
                if(led2_time > 0) led2_time -= 1;
            }
        }


    }//while loop
}

static uint8_t buf0[256],buf1[256];
static uint8_t wp0=0,wp1=0;

void setFiFo0(char data){
    buf0[wp0++] = data;
}
void setFiFo1(char data){
    buf1[wp1++] = data;
}


static uint8_t rp0=0,rp1=0;

uint8_t chkfifo0( void ){
    return (wp0 - rp0);
}
uint8_t chkfifo1( void ){
    return (wp1 - rp1);
}
char peekFiFo0( int offset ){
    if(offset >= chkfifo0() ) return 255;
    uint8_t idx = rp0 + offset;
    return buf0[idx];
}
char peekFiFo1( int offset ){
    if(offset >= chkfifo1() ) return 255;
    uint8_t idx = rp1 + offset;
    return buf1[idx];
}

void transConsoleFiFo( void ){
    char buff[256];
    
    for(int i = 0; i < chkfifo0();i++ ){
        char cc = buff[i] = peekFiFo0(i);
        if(cc != '\n') continue;
        // 改行コードまで到達

        if( tud_cdc_n_write_available(0) == 0 ) break;

        printf("PORT0:");
        for( int j=0; j <= i; j++ ){
            tud_cdc_n_write_char(0, buff[j]);
            rp0++;
        }
        tud_cdc_n_write_flush(0);
        break;
    }
    for(int i = 0; i < chkfifo1();i++ ){
        char cc = buff[i] = peekFiFo1(i);
        if(cc != '\n') continue;
        // 改行コードまで到達

        if( tud_cdc_n_write_available(0) == 0 ) break;

        printf("PORT1:");
        for( int j=0; j <= i; j++ ){
            tud_cdc_n_write_char(0, buff[j]);
            rp1++;
        }
        tud_cdc_n_write_flush(0);
        break;
    }
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