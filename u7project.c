#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "hardware/timer.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "pico/binary_info.h"
#include "inc/ssd1306.h"
#include "hardware/i2c.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/apps/mqtt.h"

//#define globla vars
char locker_password[] = " ";
char aux_password[] = "";
uint8_t password_writing = false;
uint8_t btn_b_pressed = false;

//#define led rgb
#define RGB_RED 13
#define RGB_GREEN 11
#define RGB_BLUE 12

//#define btn b pin, irq
#define BTN_B 6
static void gpio_irq_handler(uint gpio, uint32_t events);


//#define lock pin
#define LOCK_PIN 9

//#define adc specs
#define ADC_YAXIS 26
#define ADC_CLOCK_DIV 97.f
#define SAMPLES 1
#define ADC_THRESHOLD_UP 4000
#define ADC_THRESHOLD_DOWN 100
#define ADC_0_POS 1900
uint dma_channel;
dma_channel_config dma_cfg;
uint16_t joystick_value[SAMPLES];

//#define oled specs
#define I2C_SDA 14
#define I2C_SCL 15

struct render_area frame_area = {
    start_column : 0,
    end_column : ssd1306_width - 1,
    start_page : 0,
    end_page : ssd1306_n_pages - 1
};

uint8_t ssd[ssd1306_buffer_length];
ssd1306_t ssd_bm;

//#define wifi specs
#define WIFI_SSID "SSID_DA_SUA_REDE"
#define WIFI_PASS "SENHA_DA_SUA_REDE"

//#define mqtt specs
#define MQTT_HOST_IP "localhost"
#define MQTT_HOST_PORT 1883

typedef struct MQTT_CLIENT_DATA_T_ {
    mqtt_client_t* mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    uint8_t data[MQTT_OUTPUT_RINGBUF_SIZE];
    uint8_t topic[100];
    uint32_t len;
} MQTT_CLIENT_DATA_T;

MQTT_CLIENT_DATA_T *mqtt;

struct mqtt_connect_client_info_t mqtt_client_info=
{
  "BITDOGLAB",
  NULL, /* user */
  NULL, /* pass */
  0,  /* keep alive */
  NULL, /* will_topic */
  NULL, /* will_msg */
  0,    /* will_qos */
  0     /* will_retain */
#if LWIP_ALTCP && LWIP_ALTCP_TLS
  , NULL
#endif
};

void mqtt_publish_msg(mqtt_client_t *client, char * topic, char *payload, int payload_size);

//RGB functions
void rgb_init(){
    gpio_init(RGB_RED);
    gpio_set_dir(RGB_RED, GPIO_OUT);
    gpio_init(RGB_GREEN);
    gpio_set_dir(RGB_GREEN, GPIO_OUT);
    gpio_init(RGB_BLUE);
    gpio_set_dir(RGB_BLUE, GPIO_OUT);
}

void rgb_set_color(int rgb_color){
    switch(rgb_color){
        case RGB_RED:
            gpio_put(RGB_RED, true);
            gpio_put(RGB_GREEN, false);
            gpio_put(RGB_BLUE, false);
            break;
        case RGB_GREEN:
            gpio_put(RGB_RED, false);
            gpio_put(RGB_GREEN, true);
            gpio_put(RGB_BLUE, false);
            break;
        case RGB_BLUE:
            gpio_put(RGB_RED, false);
            gpio_put(RGB_GREEN, false);
            gpio_put(RGB_BLUE, true);
            break;
        case 0:
            gpio_put(RGB_RED, false);
            gpio_put(RGB_GREEN, false);
            gpio_put(RGB_BLUE, false);
            break;    
        case 1:
            gpio_put(RGB_RED, true);
            gpio_put(RGB_GREEN, true);
            gpio_put(RGB_BLUE, true);
            break;    
    }

}

//BTN functions
void gpio_irq_handler (uint gpio, uint32_t events){
    static absolute_time_t last_press_time = 0;     
    bool button_pressed = !gpio_get(gpio); 

    if (button_pressed && absolute_time_diff_us(last_press_time, get_absolute_time()) > 200000) {
      last_press_time = get_absolute_time();
      if(gpio == BTN_B){
        btn_b_pressed = true;
      }    
    } 
}

void btn_init(){
    gpio_init(BTN_B);
    gpio_set_dir(BTN_B, GPIO_IN);
    gpio_pull_up(BTN_B);
    gpio_init(LOCK_PIN);
    gpio_set_dir(LOCK_PIN, GPIO_OUT);
    gpio_put(LOCK_PIN, 0);
    gpio_set_irq_enabled_with_callback(BTN_B, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

}

//ADC functions
void adc_sample(){
    adc_fifo_drain();
    adc_run(false); 
    dma_channel_configure(dma_channel, &dma_cfg,
        joystick_value, 
        &(adc_hw->fifo), 
        SAMPLES, 
        true 
      );
    adc_run(true);
    dma_channel_wait_for_finish_blocking(dma_channel);
    adc_run(false);
}

void joystick_init(){
    adc_init();
    adc_gpio_init(ADC_YAXIS);
    adc_select_input(0);
    adc_fifo_setup(
        true, 
        true, 
        1, 
        false, 
        false
      );
    adc_set_clkdiv(ADC_CLOCK_DIV);
    dma_channel = dma_claim_unused_channel(true);
    dma_cfg = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_16); 
    channel_config_set_read_increment(&dma_cfg, false); 
    channel_config_set_write_increment(&dma_cfg, true); 
    channel_config_set_dreq(&dma_cfg, DREQ_ADC);
}

//OLED functions
void oled_init(){
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_init();

    calculate_render_area_buffer_length(&frame_area);

    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);
}

void oled_write_line(int line, char *string){
    ssd1306_draw_string(ssd, 5, line*8, string);
}

void oled_update(){
    render_on_display(ssd, &frame_area);
}

void oled_clear(){
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);
}

//WIFI functions 
void wifi_init(){
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar o Wi-Fi\n");
    }
    else{
        cyw43_arch_enable_sta_mode();
    
        if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
            printf("Falha ao conectar ao Wi-Fi\n");
        }
    }
}

//LOCK functions/callback
int64_t activate_lock_callback(alarm_id_t id, void *user_data) {
    gpio_put(LOCK_PIN, true);
    rgb_set_color(true);
    char msg[] = "Tranca ativada";
    mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
    oled_clear();
    oled_write_line(2, "Tranca Ativada");
    oled_update();
    return 0;
}

void deactivate_lock(){
    rgb_set_color(RGB_GREEN);
    gpio_put(LOCK_PIN, 0);
    add_alarm_in_ms(10000, activate_lock_callback, NULL, false);
    char msg[] = "Tranca desativada";
    mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
    oled_clear();
    oled_write_line(2, "Tranca");
    oled_write_line(3, "     desativada");
    oled_update();
}

//MQTT functions
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    MQTT_CLIENT_DATA_T* mqtt_client = (MQTT_CLIENT_DATA_T*)arg;
    LWIP_UNUSED_ARG(data);

    strncpy(mqtt_client->data, data, len);
    mqtt_client->len=len;
    mqtt_client->data[len]='\0';

    if(strcmp(mqtt_client->topic, "password") == 0){
        if(password_writing){
            char msg[100] = "Senha esta sendo inserida, nao eh possivel atulizar via web no momento";
            mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
        }
        else{
            strcpy(locker_password, mqtt_client->data);
            char msg[100] = "Senha definida: ";
            strcat(msg, locker_password);
            strcat(msg, " via Web");
            mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
        }

    }
    if(strcmp(mqtt_client->topic, "unlock") == 0){
        if((strcmp(" ", locker_password) == 0)){
            char msg[100] = "Senha ainda nao definida";
            mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
        }
        else{
            deactivate_lock();
        }
    }
}

static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    MQTT_CLIENT_DATA_T* mqtt_client = (MQTT_CLIENT_DATA_T*)arg;
    strcpy(mqtt_client->topic, topic);
}

static void mqtt_request_cb(void *arg, err_t err) {
    MQTT_CLIENT_DATA_T* mqtt_client = ( MQTT_CLIENT_DATA_T*)arg;
  
    LWIP_PLATFORM_DIAG(("MQTT client \"%s\" request cb: err %d\n", mqtt_client->mqtt_client_info.client_id, (int)err));
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    MQTT_CLIENT_DATA_T* mqtt_client = (MQTT_CLIENT_DATA_T*)arg;
    LWIP_UNUSED_ARG(client);
  
    LWIP_PLATFORM_DIAG(("MQTT client \"%s\" connection cb: status %d\n", mqtt_client->mqtt_client_info.client_id, (int)status));
  
    if (status == MQTT_CONNECT_ACCEPTED) {
        //topic subscribes
        mqtt_sub_unsub(client,
            "password", 0,
            mqtt_request_cb, arg,
            1);
        mqtt_sub_unsub(client,
            "unlock", 0,
            mqtt_request_cb, arg,
            1);

    }
    else{
        printf("MQTT client denied\n");
    }
}

static void mqtt_pub_request_cb(void *arg, err_t result)
{
  if(result != ERR_OK) {
    printf("Publish result: %d\n", result);
  }
}

void mqtt_publish_msg(mqtt_client_t *client, char * topic, char *payload, int payload_size)
{
  err_t err;
  u8_t qos = 2; 
  u8_t retain = 0; 
  err = mqtt_publish(client, topic, payload, payload_size, qos, retain, mqtt_pub_request_cb,NULL);
  if(err != ERR_OK) {
    printf("Publish err: %d\n", err);
  }
}

int mqtt_init(){
    mqtt=(MQTT_CLIENT_DATA_T*)calloc(1, sizeof(MQTT_CLIENT_DATA_T));

    if (!mqtt) {
        printf("mqtt client instant ini error\n");
        return 0;
    }
    mqtt->mqtt_client_info = mqtt_client_info;

    ip_addr_t addr;
    if (!ip4addr_aton(MQTT_HOST_IP, &addr)) {
        printf("ip error\n");
        return 0;
    }

    mqtt->mqtt_client_inst = mqtt_client_new();
    mqtt_set_inpub_callback(mqtt->mqtt_client_inst, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, mqtt);

    err_t err = mqtt_client_connect(mqtt->mqtt_client_inst, &addr, MQTT_HOST_PORT, &mqtt_connection_cb, mqtt, &mqtt->mqtt_client_info);
    if (err != ERR_OK) {
      printf("connect error\n");
      return 0;
    }

    char msg[] = "Device Online";

    mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));

    return 1;
    
}

//uptade aux_password with user input
void password_input(){
    rgb_set_color(false);
    strcpy(aux_password, "");
    int aux_value = 0;
    int password_value = 0;
    int i;
    uint16_t joystick_last_value = -1;
    char display_string_password[] = "";

    password_writing = true;
    oled_clear();
    oled_write_line(2, "Insira a senha");
    oled_update();
    for(i = 0; i < 4; i++){
        aux_value = 0;
        itoa(password_value * 10 + aux_value, display_string_password, 10);
        oled_write_line(4, display_string_password);
        oled_update();
        while(!btn_b_pressed){
            adc_sample();
            if (joystick_value[0] > ADC_THRESHOLD_UP && abs(joystick_last_value - joystick_value[0]) > 200 ){
                joystick_last_value = joystick_value[0];
                aux_value++;
                if(aux_value >9){
                    aux_value = 0;
                }
                itoa(password_value * 10 + aux_value, display_string_password, 10);
                oled_write_line(4, display_string_password);
                oled_update();
            }
            if (joystick_value[0] < ADC_THRESHOLD_DOWN && abs(joystick_last_value - joystick_value[0]) > 200){
                joystick_last_value = joystick_value[0];
                aux_value--;
                if(aux_value < 0){
                    aux_value = 9;
                }
                itoa(password_value * 10 + aux_value, display_string_password, 10);
                oled_write_line(4, display_string_password);
                oled_update();
            }
            if (abs(joystick_value[0] - ADC_0_POS) < 100 && abs(joystick_last_value - joystick_value[0]) > 200){
                joystick_last_value = joystick_value[0];
            }
        }
        btn_b_pressed = false;
        password_value = password_value * 10 + aux_value;
    }
    password_writing = false;
    itoa(password_value, aux_password, 10);
}

int main()
{
    stdio_init_all();
    btn_init();
    rgb_init();
    joystick_init();
    oled_init();
    wifi_init();
    if(!mqtt_init()){
        printf("Mqtt init failed\n");
    }

    oled_write_line(1, "Sistema");
    oled_write_line(2, "   Inicializado");
    oled_write_line(4, "Aperte btn B");
    oled_write_line(5, "para nova senha");
    oled_write_line(7, "Ou att pela web");
    oled_update();

    rgb_set_color(RGB_BLUE);
    //waiting password config
    while((strcmp(" ", locker_password) == 0) && !btn_b_pressed){
        
        sleep_ms(50);
    }
    
    //local password config
    if(btn_b_pressed){
        btn_b_pressed = false;
        password_input();
        strcpy(locker_password, aux_password);
    }

    //publish new config
    char msg[100] = "Senha definida: ";
    strcat(msg, locker_password);
    strcat(msg, " via Local");
    mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));

    oled_clear();
    oled_write_line(2, "Senha definida");
    oled_update();
    //callback to activate lock
    add_alarm_in_ms(1000, activate_lock_callback, NULL, false);

    //main loop
    while (true) {
        if(btn_b_pressed){
            btn_b_pressed = false;
            password_input();
            strcpy(msg, "Tentativa de senha local - ");
            if(strcmp(aux_password, locker_password) == 0){
                strcat(msg, "Valida");
                mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
                deactivate_lock();
                sleep_ms(10000);
            }
            else{
                rgb_set_color(RGB_RED);
                oled_write_line(6, "Senha incorreta");
                oled_update();
                oled_write_line(6, "");
                add_alarm_in_ms(2000, activate_lock_callback, NULL, false); //lock is already activaded, this line just update oled screen and RGB
                strcat(msg, "Invalida");
                mqtt_publish_msg(mqtt->mqtt_client_inst,"devicestatus", msg, sizeof(msg));
                sleep_ms(2000);
            }
        }
        sleep_ms(50);
    }
}
