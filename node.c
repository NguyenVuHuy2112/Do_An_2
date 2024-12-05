#include "contiki.h"
#include "net/mac/tsch/tsch.h"
#include "net/routing/routing.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "sf-simple.h"
#include "sys/log.h"
#include "sys/node-id.h"
#include "net/ipv6/simple-udp.h"
#include "sys/rtimer.h"
#include "net/routing/rpl-lite/rpl.h"
#include "net/ipv6/uip-debug.h"
#include <stdlib.h>
#include <stdio.h>
#include "project-conf.h"

/************************************************
 *                  Constants                   *
 ************************************************/
#define LOG_MODULE "Sensor Node"
#define LOG_LEVEL LOG_LEVEL_DBG
#define UDP_PORT 1234
#define CHECK_INTERVAL (CLOCK_SECOND * 5)
#define PING_INTERVAL (CLOCK_SECOND * 4)
// #define RF_CONF_TXPOWER 7

/************************************************
 *                  Structs                     *
 ************************************************/
/* Structure for sensor payload including PING data */
typedef struct {
  uint8_t node_id;          // Node ID
  uint16_t tx_count;        // Transmission count
  uint16_t rx_count;        // Reception count (PONG received)
  uint64_t send_time;       // Send time (network uptime ticks)
  int16_t temperature;      // Temperature in Celsius
  uip_ipaddr_t parent_addr; // Parent IP address
  uint16_t ping_sent;       // Total PINGs sent
  uint16_t pong_received;   // Total PONGs received
  uint16_t rtt;             // Round-Trip Time in ms
} sensor_payload_t;

/* Structure for PING message */
typedef struct {
  uint8_t msg_type; // 'P' for PING
  uint8_t node_id;
  uint16_t seq_num;
} ping_message_t;

/* Structure for PONG message */
typedef struct {
  uint8_t msg_type; // 'O' for PONG
  uint8_t node_id;
  uint16_t seq_num;
} pong_message_t;

/************************************************
 *              Global variables                *
 ************************************************/
static struct simple_udp_connection udp_conn;
static uint16_t node_tx_count = 0;
static uint16_t ping_sent_count = 0;
static uint16_t pong_received_count = 0;
static rtimer_clock_t last_ping_time;
static uint16_t last_rtt = 0; // Global variable to store the last valid RTT
static uint16_t ping_seq_num = 0; // Sequence number for PING messages
static int16_t nodes_to_check[] = {8, 15, 145}; // Valid node IDs

/************************************************
 *                  Functions                   *
 ************************************************/
//static int get_node_index();
static void send_temperature_data();
static void send_ping();
static void udp_ping_callback(struct simple_udp_connection *c,
                              const uip_ipaddr_t *sender_addr,
                              uint16_t sender_port,
                              const uip_ipaddr_t *receiver_addr,
                              uint16_t receiver_port,
                              const uint8_t *data,
                              uint16_t datalen);

/************************************************
 *                  Processes                   *
 ************************************************/
PROCESS(sensor_node_process, "Sensor Node Process");
AUTOSTART_PROCESSES(&sensor_node_process);

PROCESS_THREAD(sensor_node_process, ev, data) {
  static struct etimer et;
  PROCESS_BEGIN();

  LOG_INFO("Starting sensor node %u...\r\n", node_id);
  sixtop_add_sf(&sf_simple_driver);
  NETSTACK_MAC.on();

  /* Register UDP connection with callback */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_ping_callback);

  /* Set timer for periodic data transmission */
  etimer_set(&et, CLOCK_SECOND * 10);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    send_temperature_data();
    send_ping();
    etimer_reset(&et);
  }

  PROCESS_END();
}

static int get_node_index(uint16_t node_id) {
  for(int i = 0; i < sizeof(nodes_to_check) / sizeof(nodes_to_check[0]); i++) {
    if(nodes_to_check[i] == node_id) {
      return i; // Return the index if node_id matches
    }
  }
  return -1; // Return -1 if node_id is not valid
}


/* UDP receive callback function for handling PONG messages */
static void udp_ping_callback(struct simple_udp_connection *c,
                              const uip_ipaddr_t *sender_addr,
                              uint16_t sender_port,
                              const uip_ipaddr_t *receiver_addr,
                              uint16_t receiver_port,
                              const uint8_t *data,
                              uint16_t datalen) {
  /* Check if the message is a PONG response */
  if (datalen == sizeof(pong_message_t)) {
    pong_message_t pong_msg;
    memcpy(&pong_msg, data, sizeof(pong_message_t));
    LOG_INFO("Received PONG message:\r\n");
    LOG_INFO("  msg_type: %c\r\n", pong_msg.msg_type);
    LOG_INFO("  node_id: %u\r\n", pong_msg.node_id);
    LOG_INFO("  seq_num: %u\r\n", pong_msg.seq_num);

    LOG_INFO("Expected values:\r\n");
    LOG_INFO("  msg_type: 'O'\r\n");
    LOG_INFO("  node_id: %u\r\n", node_id);
    LOG_INFO("  seq_num: %u\r\n", ping_seq_num);
    int index = get_node_index(pong_msg.node_id);
    if (index >= 0 && pong_msg.msg_type == 'O' && pong_msg.seq_num == ping_seq_num) {
      pong_received_count++;
      rtimer_clock_t current_time = RTIMER_NOW();
      last_rtt = (current_time - last_ping_time) * 1000 / RTIMER_SECOND;
      LOG_INFO("PONG received from Coordinator | RTT: %u ms\r\n", last_rtt);
    } else {
      LOG_WARN("Received PONG with invalid node_id or mismatched seq_num\r\n");
    }
  } else {
    LOG_ERR("Received unexpected PONG message size: %u bytes\r\n", datalen);
  }
}

/* Function to send PING message to Coordinator */
static void send_ping() {
  uip_ipaddr_t dest_ipaddr;

  if(NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
    ping_sent_count++;
    last_ping_time = RTIMER_NOW();

    ping_seq_num++; // Increment sequence number

    ping_message_t ping_msg;
    ping_msg.msg_type = 'P';
    ping_msg.node_id = node_id;
    ping_msg.seq_num = ping_seq_num;

    simple_udp_sendto(&udp_conn, &ping_msg, sizeof(ping_msg), &dest_ipaddr);

    /* Calculate Packet Loss */
    int packet_loss = (ping_sent_count > 0) ? 
                      (int)((1 - (float)pong_received_count / ping_sent_count) * 100) : 100;

    LOG_INFO("PING sent to Coordinator | PING count: %u | PONG received: %u | Packet Loss: %d%%\r\n",
             ping_sent_count, pong_received_count, packet_loss);
  } else {
    LOG_ERR("Failed to get coordinator IP address for PING.\r\n");
  }
}

/* Function to send temperature data along with PING metrics */
static void send_temperature_data() {
  uip_ipaddr_t dest_ipaddr;
  int8_t real_temp = 20 + (rand() % 100) / 10; // Random temperature value between 20 and 29.9

  node_tx_count++;
  uint64_t send_time = tsch_get_network_uptime_ticks();

  sensor_payload_t payload;
  payload.node_id = node_id;
  payload.tx_count = node_tx_count;
  payload.rx_count = pong_received_count;
  payload.send_time = send_time;
  payload.temperature = real_temp;

  /* Copy parent address */
  rpl_dag_t *dag = rpl_get_any_dag();
  if (dag != NULL && dag->preferred_parent != NULL) {
    uip_ipaddr_t *parent_addr = rpl_neighbor_get_ipaddr(dag->preferred_parent);
    if (parent_addr != NULL) {
      uip_ipaddr_copy(&payload.parent_addr, parent_addr);
    } else {
      uip_create_unspecified(&payload.parent_addr);
    }
  } else {
    uip_create_unspecified(&payload.parent_addr);
  }

  /* Include PING data */
  payload.ping_sent = ping_sent_count;
  payload.pong_received = pong_received_count;

  // Use last valid RTT calculated in udp_ping_callback()
  payload.rtt = last_rtt;

  if (NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
    simple_udp_sendto(&udp_conn, &payload, sizeof(payload), &dest_ipaddr);
    LOG_INFO("Node %u: Sent data | TX: %u | Temp: %dC | PING Sent: %u | PONG Received: %u | RTT: %u ms\r\n",
             node_id, node_tx_count, real_temp, payload.ping_sent, payload.pong_received, payload.rtt);
  } else {
    LOG_ERR("Failed to get coordinator IP address.\r\n");
  }
}