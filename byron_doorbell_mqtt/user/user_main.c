#include "ets_sys.h"
#include "driver/uart.h"
#include "osapi.h"
#include "mqtt.h"
#include "wifi.h"
#include "debug.h"
#include "gpio.h"
#include "user_interface.h"
#include "mem.h"
#include "user_main.h"

volatile uint32 gpio_status;
volatile uint32 gpio_input;
volatile uint32 interrupt_timestamp;
volatile uint32 interrupt_pos_timestamp;
volatile uint32 interrupt_neg_timestamp;
volatile uint32 pulse_width;
volatile uint8 interrupt_count;
volatile uint8 preamble;
volatile uint32 code[33];
volatile uint32 decoded;
volatile uint8 capture_count;
volatile uint8 trigger;

MQTT_Client mqttClient;
os_event_t user_procTaskQueue[user_procTaskQueueLen];
os_timer_t timer_enable_interrupt;

static void ICACHE_FLASH_ATTR wifiConnectCb(uint8_t status) {
  if (status == STATION_GOT_IP) {
    MQTT_Connect(&mqttClient);
  } else {
    MQTT_Disconnect(&mqttClient);
  }
}

static void ICACHE_FLASH_ATTR mqttConnectedCb(uint32_t *args) {
  MQTT_Client* client = (MQTT_Client*)args;
  INFO("MQTT: Connected\r\n");

  MQTT_Publish(client, "/mqtt/topic/0", "connected", 9, 0, 0);
}

static void ICACHE_FLASH_ATTR mqttDisconnectedCb(uint32_t *args) {
  MQTT_Client* client = (MQTT_Client*)args;
  INFO("MQTT: Disconnected\r\n");
}

static void ICACHE_FLASH_ATTR mqttPublishedCb(uint32_t *args) {
  MQTT_Client* client = (MQTT_Client*)args;
  INFO("MQTT: Published\r\n");
}

static void ICACHE_FLASH_ATTR mqttDataCb(uint32_t *args, const char* topic, uint32_t topic_len, const char *data, uint32_t data_len) {
  char *topicBuf = (char*)os_zalloc(topic_len + 1);
  char *dataBuf = (char*)os_zalloc(data_len + 1);

  MQTT_Client* client = (MQTT_Client*)args;
  os_memcpy(topicBuf, topic, topic_len);
  topicBuf[topic_len] = 0;
  os_memcpy(dataBuf, data, data_len);
  dataBuf[data_len] = 0;
  INFO("Receive topic: %s, data: %s \r\n", topicBuf, dataBuf);
  os_free(topicBuf);
  os_free(dataBuf);
}

static void ICACHE_FLASH_ATTR loop(os_event_t *events) {
  if(preamble) {
    if(interrupt_count == 33) {
      preamble = 0;
      interrupt_count = 0;

      decoded = 0;
      int i;
      for(i = 0; i <= 33; i++) {
        decoded |= (code[i] << i);
      }

      if((decoded == 0xCBA1B4D5) | (decoded == 0xC9ABCF09)) {
        capture_count++;

        if(capture_count >= 10) {
          capture_count = 0;
          trigger = 1;

          // Disable Interrupts
          gpio_pin_intr_state_set(GPIO_ID_PIN(5), GPIO_PIN_INTR_DISABLE);
        }
      }
    }
  }

  if(trigger) {
    trigger = 0;
    os_printf("CODE: %02X\r\n", decoded);
    MQTT_Publish(&mqttClient, "/mqtt/topic/0", "doorbell", 8, 0, 0);
    os_timer_setfn(&timer_enable_interrupt, (os_timer_func_t *)timer_enable_interrupt_cb, NULL);
    os_timer_arm(&timer_enable_interrupt, 1000, 0);
  }

  system_os_post(user_procTaskPrio, 0, 0);
}

static void ICACHE_FLASH_ATTR user_main(void) {
  uart_init(BIT_RATE_115200, BIT_RATE_115200);
  MQTT_InitConnection(&mqttClient, MQTT_HOST, MQTT_PORT, DEFAULT_SECURITY);
  //MQTT_InitConnection(&mqttClient, "192.168.11.122", 1880, 0);

  if ( !MQTT_InitClient(&mqttClient, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, MQTT_KEEPALIVE, MQTT_CLEAN_SESSION) )
  {
    INFO("MQTT Init Failed, Check Version!\r\n");
    return;
  }
  //MQTT_InitClient(&mqttClient, "client_id", "user", "pass", 120, 1);
  MQTT_InitLWT(&mqttClient, "/lwt", "offline", 0, 0);
  MQTT_OnConnected(&mqttClient, mqttConnectedCb);
  //MQTT_OnDisconnected(&mqttClient, mqttDisconnectedCb);
  MQTT_OnPublished(&mqttClient, mqttPublishedCb);
  MQTT_OnData(&mqttClient, mqttDataCb);

  WIFI_Connect(STA_SSID, STA_PASS, wifiConnectCb);

  system_os_task(&loop, user_procTaskPrio,user_procTaskQueue, user_procTaskQueueLen);
  system_os_post(user_procTaskPrio, 0, 0 );
}

void user_init(void) {
  preamble = 0;
  interrupt_count = 0;
  capture_count = 0;

  gpio_init();
  // GPIO5 as GPIO
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5);

  // Disable pull up
  PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO5_U);
  gpio_output_set(0, 0, 0, GPIO_ID_PIN(RX_GPIO_PIN));

  ETS_GPIO_INTR_DISABLE();
  ETS_GPIO_INTR_ATTACH(gpio_intr_handler, RX_GPIO_PIN);

  gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO_PIN), GPIO_PIN_INTR_NEGEDGE);
  ETS_GPIO_INTR_ENABLE();
  system_init_done_cb(user_main);
}

void ICACHE_FLASH_ATTR timer_enable_interrupt_cb() {
  os_timer_disarm(&timer_enable_interrupt);

  // Interrupt enable for next preamble
  gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO_PIN), GPIO_PIN_INTR_NEGEDGE);
}

LOCAL void gpio_intr_handler(void *arg) {
  gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
  gpio_input = gpio_input_get();
  interrupt_timestamp = system_get_time();

  // GPIO 5 Interrupt
  if(gpio_status & BIT(RX_GPIO_PIN)) {
    gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO_PIN), GPIO_PIN_INTR_DISABLE); // Disable Interrupt
    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status);                    // Clear Interrupt

    if(gpio_input & BIT(RX_GPIO_PIN)) {
      // GPIO 5 Interrupt High
      interrupt_pos_timestamp = interrupt_timestamp;
      pulse_width = interrupt_pos_timestamp - interrupt_neg_timestamp;

      if(!preamble) {
        if((pulse_width >= (7000)) && (pulse_width <= (7100))) {
          preamble = 1;
          interrupt_count = 0;
        }
      }

      gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO_PIN), GPIO_PIN_INTR_NEGEDGE); // Negative Edge Interrupt Enable

    } else {
      // GPIO 5 Interrupt Low
      interrupt_neg_timestamp = interrupt_timestamp;
      pulse_width = interrupt_neg_timestamp - interrupt_pos_timestamp;

      if(preamble) {
        if((pulse_width >= 450) && (pulse_width <= 600)) {
          code[interrupt_count] = 0;
          interrupt_count++;
        } else if ((pulse_width >= 1400) && (pulse_width <= 1600)) {
          code[interrupt_count] = 1;
          interrupt_count++;
        } else {
          // Unexpected pulse width
          preamble = 0;
          interrupt_count = 0;
        }
      }

      gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO_PIN), GPIO_PIN_INTR_POSEDGE); // Positive Edge Interrupt Enable
    }
  }
}
